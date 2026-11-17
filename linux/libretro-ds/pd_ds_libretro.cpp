// SPDX-License-Identifier: GPL-3.0-or-later
// Keep DSperate's libretro lifecycle, SRAM and state format. Only presentation
// and input are adapted; there is no second launcher or save writer.
#define retro_set_environment ds_set_environment
#define retro_set_video_refresh ds_set_video_refresh
#define retro_set_input_state ds_set_input_state
#define retro_get_system_info ds_get_system_info
#define retro_get_system_av_info ds_get_system_av_info
#define retro_load_game ds_load_game
#define retro_unload_game ds_unload_game
#define retro_run ds_run
#define retro_reset ds_reset
#define retro_unserialize ds_unserialize
#include "src/libretro.cpp"
#undef retro_set_environment
#undef retro_set_video_refresh
#undef retro_set_input_state
#undef retro_get_system_info
#undef retro_get_system_av_info
#undef retro_load_game
#undef retro_unload_game
#undef retro_run
#undef retro_reset
#undef retro_unserialize
#include "companion.h"
#include "companion_ui.h"
#include "battle_strip.h"
#include "layout.h"
#include "party_view.h"
#include "bag_view.h"
#include "dex_view.h"
#include "map_view.h"
#include "guide_view.h"
#include "browser.h"
#include "data_gen.h"
#include "pk_nds_field.h"
#include <ctime>
#include "pk_nds_call_memory.h"
#include "core/crypto/sha1.h"
#include <fstream>
#include <stdexcept>
extern "C" {
#include "pd_style.h"
#include "pd_ui.h"
#include "../hud.h"
#include "../quick_menu.h"
#include "pd_tables.h"
#include "pd_icon.h"
}
namespace pd_ds {
// PD_DS_PERF=1: every 300 frames, where a frame's time went (ms each): DSperate's own emulation, and PokeDaisy's
// present split into composing the screens, the RGB565 copy (none where the frontend takes XRGB8888) and the HUD. Off,
// it costs a getenv at load.
bool perfOn=false;
// The last frame handed over was this layout's field picture (not the companion's or the quick menu's), and whether the
// one before was a repeat: see present_frame's skip.
bool presentedField=false,repeatedLast=false;
DsArrangement presentedArrangement;
bool same_arrangement(const DsArrangement& a,const DsArrangement& b){
  auto rect=[](const DsRect& p,const DsRect& q){return p.x==q.x&&p.y==q.y&&p.w==q.w&&p.h==q.h;};
  return rect(a.top,b.top)&&rect(a.touch,b.touch)&&a.floating==b.floating&&a.modal==b.modal&&a.useField==b.useField;
}
double perfRun=0,perfPresent=0,perfCompose=0,perfConvert=0,perfHud=0,perfLayout=0;int perfFrames=0,perfSkipped=0;
double perf_now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
// dsperate_frameskip on a frontend with no audio buffer status callback (NXRedux, PokeDaisy's own host): nothing ever
// said a frame was late, so DSperate never skipped one. The core then times itself: a frame's work is DSperate's
// emulation and this file's drawing, not the time inside the frontend's video callback (which waits for the screen).
// Work past the budget adds up as debt; over skipDebt, the next frame's 3D raster and line output are skipped (the
// game, both CPUs and the sound run as ever), at most two in a row. A frame inside the budget pays the debt back, so a
// game that keeps up never skips. PD_DS_FRAME_BUDGET (ms) tunes the budget: a frame less what the frontend needs.
bool ownSkip=false;
double frameBudget=15.2,frameDebt=0,runStart=0,presentStart=0,frontendTime=0,frameWork=0;
constexpr double skipDebt=4.0,maxDebt=33.0;   // ms: when a skip is asked for, and the most a stall can leave owing
int skipRun=0;
void reset_skip(){frameDebt=0;skipRun=0;}
void perf_report(){
  if(!perfOn || ++perfFrames<300)return;
  const double n=perfFrames;
  std::fprintf(stderr,"PokeDaisy DS perf: emulation %.2f ms, present %.2f ms (layout %.2f, compose %.2f, rgb565 %.2f, hud %.2f) per frame, %d of %d frames skipped%s\n",
               (perfRun-perfPresent)/n,perfPresent/n,perfLayout/n,perfCompose/n,perfConvert/n,perfHud/n,
               perfSkipped,perfFrames,ownSkip?" (the core's own timing)":"");
  perfRun=perfPresent=perfCompose=perfConvert=perfHud=perfLayout=0;perfFrames=perfSkipped=0;
}
retro_environment_t environment;
retro_video_refresh_t video;
retro_input_state_t input;
const PkNdsFieldGame* game;
const DsBattleStyle* battleStyle=nullptr;
DsGame companionGame=DsGame::Unsupported;
bool soulSilver=false;
DsCompanion snapshot;
DsMapArt map;
std::vector<std::vector<uint32_t>> icons,hudItemIcons;
int repelFull=0;
hud_hits hudHits{};
int hudCursor=-1;
unsigned hudConsumed=0;
// The field HUD's own 256x192 picture, kept between frames: what it shows comes from the snapshot (read every 12th
// frame), the settings and the cursor, so it is drawn again only when one of those changes and blended over the game
// each frame. hud.c's hud_key does the same for the GB / GBA cores.
struct HudLayerKey {
  pd_settings settings;
  int right,height,cursor;
  bool minimal,small,pc;
};
uint32_t hud_pixels[256*192];
HudLayerKey hudLayerKey;
hud_hits hudLayerHits{};
bool hudLayerValid=false;
void clear_hud_focus(){hudHits.n=0;hudCursor=-1;hudConsumed=0;hudLayerValid=false;}
bool supported=false, companion=false, quickOpen=false, swallowMenuInput=false;
int quickSelected=0,companionHold=0;
struct FlyDestination { int x,y; const char* name; int header=0; };
enum class FlyStage { Idle, Menu, Party, Member, Move, Town, Release };
struct FlyJob {
  FlyStage stage=FlyStage::Idle;
  unsigned started=0,ready=0,key=0,tapFrames=0;
  int slot=0,x=0,y=0;
  FlyDestination destination{};
  bool active() const {return stage!=FlyStage::Idle;}
} flyJob;
std::vector<FlyDestination> flyDestinations;
int flySelected=0,flySlot=-1;
uint32_t flyPersonality=0;
bool flyPicker=false,heldQuickConfirm=false;
int pcJob=0,flyCallJob=0;
bool flyChecking=false,autoRunThisFrame=false;
const char* flyWhy=nullptr;
constexpr size_t shortcutCapacity=6;
bool stageReady=false;
uint32_t quick_pixels[341*256];
DsCompanionUi shell;
DsScenePolicy scene;
DsBattleStrip battleStrip;
DsBattleTap battleTap;
DsBattleView drawnBattle,battleBalls;
std::string battleName;
std::array<uint32_t,256*192> battlePanelArt;
bool battlePanelArtValid=false;
unsigned battleHeld=0;
int holdStrip=0,restoreGrace=0;
bool battleActive=false,injectBattleTap=false,wasInBattle=false;
constexpr unsigned battleKeys=(1u<<RETRO_DEVICE_ID_JOYPAD_A)|(1u<<RETRO_DEVICE_ID_JOYPAD_B)|
  (1u<<RETRO_DEVICE_ID_JOYPAD_UP)|(1u<<RETRO_DEVICE_ID_JOYPAD_DOWN)|
  (1u<<RETRO_DEVICE_ID_JOYPAD_LEFT)|(1u<<RETRO_DEVICE_ID_JOYPAD_RIGHT);
DsArrangement arrangement;
bool manualTouch=false,fieldValid=false,fieldMenuFlow=false;
uint32_t stage[512*384],fieldFrame[256*192];
// A popup's backdrop (the held field, dimmed, and the popup's frame) stays in stage while the popup is up: ds_compose
// then draws only the live screens. fieldRevision counts fieldFrame's copies.
DsBackdrop backdrop;
unsigned fieldRevision=0;
// The HUD or the stylus cross was drawn into stage (they are part of the frame handed over): the quick menu, which
// opens over the game's picture alone, composes it again first.
bool stageMarked=false;
unsigned held_frames[16]={};
bool select_used=false, select_pulse=false;
unsigned old_buttons=0, buttons=0, tick=0, width=256, height=192;
int screen_override=0, cursor_x=128, cursor_y=96;
int layout=0; // 0 top, 1 bottom, 2 side by side, 3 stacked
// The frame as the frontend takes it: XRGB8888 as drawn, or (rgb565) converted into pixels.
bool rgb565=false;
uint16_t pixels[(512+4*248)*384];   // the wide frame too (EXPAND: wide::OUT_MAX)
uint32_t ui_pixels[341*256],quick_stage[512*384];
// The frontend's video callback, its time kept out of the frame's work (it may wait for the screen).
void show(const void* px,unsigned w,unsigned h,size_t pitch){
  if(!ownSkip){video(px,w,h,pitch);return;}
  const double t0=perf_now();
  video(px,w,h,pitch);
  frontendTime+=perf_now()-t0;
}
void submit(const uint32_t* px,unsigned w,unsigned h){
  if(!rgb565){show(px,w,h,w*4);return;}
  const double c0=perfOn?perf_now():0;
  for(unsigned i=0;i<w*h;++i)pixels[i]=ds_rgb565(px[i]);
  if(perfOn)perfConvert+=perf_now()-c0;
  show(pixels,w,h,w*2);
}

void geometry(unsigned w,unsigned h);
// ---- EXPAND (Capsule): the overworld's 3D drawn past the top screen's edges, to the phone's edges ----
// The frontend's panel ("pokedaisy_panel", WxH) sets the frame's shape: the one-screen stage (512 x 384, the DS's 2x)
// in the middle, margins left and right on a wide panel, above and below on a tall one. On the field the margins are
// the 3D of slice engines - the screen's neighbours on that side, fed every polygon the game submits (Gpu3D::wide);
// elsewhere they are black.
namespace wide {
constexpr int OVERLAP=4;        // a slice reaches this far into the screen (its own edge isn't shown)
constexpr int MAX_X=248,MAX_Y=184;   // DS pixels each side: a slice less its overlap, less a few
int mx=0,my=0;                  // DS pixels each side, across / up and down (on average); 0 = none
// Upright, the margin above takes most of the height: what's ahead, faded out where the game has drawn nothing
// (Black 2 / White 2 leave the map past the DS's view unloaded), and only a strip below, where a game culls the
// models nearest the camera it wouldn't show (a building cut to one wall). myT + myB = 2 my.
int myT=0,myB=0;
unsigned outW=512,outH=384;     // the frame
// 0 left, 1 right, 2 above, 3 below: (sx, sy) where the slice sits
constexpr int SX[4]={-1,1,0,0},SY[4]={0,0,-1,1};
std::unique_ptr<ds::gpu::Gpu3D> slices[4];
std::string panel;
constexpr unsigned OUT_MAX=(512+4*MAX_X)*384>(512*(384+4*MAX_Y))?(512+4*MAX_X)*384:512*(384+4*MAX_Y);
uint32_t out[OUT_MAX];
// The HUD over the whole wide frame (DS pixels, 2x on the frame): drawn by present, laid on by emit; 0 = none.
uint32_t hud[(256+2*MAX_X)*192];
int hudW=0,hudH=0;   // its size in DS pixels; from the frame's top left

void on_submit(ds::gpu::Gpu3D& g,const ds::gpu::Vertex* const* v,int n,bool facing,void*){
  for(int k=0;k<4;++k){
    if(!slices[k])continue;
    const int sx=SX[k],sy=SY[k];
    ds::gpu::Vertex cv[4];
    bool allL=true,allR=true,allT=true,allB=true,seen=false;
    for(int i=0;i<n;++i){
      cv[i]=*v[i];
      const int64_t w=cv[i].pos[3]&0x00FFFFFF;
      // The slice left of the screen is clip-space x in [-3w,-w]: moved by +2w (a little less: OVERLAP pixels) it's
      // [-w,w]. Above / below the same in y, which runs up the screen.
      const int64_t x=int64_t(cv[i].pos[0])-sx*(w*(256-OVERLAP)/128);
      const int64_t y=int64_t(cv[i].pos[1])+sy*(w*(192-OVERLAP)/96);
      cv[i].pos[0]=int32_t(x);cv[i].pos[1]=int32_t(y);
      if(w<=0){allL=allR=allT=allB=false;seen=true;continue;}
      allL&=x<-w; allR&=x>w; allB&=y<-w; allT&=y>w;
      // Only what the margin shows: the slice's columns (rows) within the margin, a few more for edge marking.
      // Screen x = (x / w + 1) 128, screen y = (1 - y / w) 96.
      if(sx<0)seen|=x*128>=int64_t(256-OVERLAP-mx-4-128)*w;
      else if(sx>0)seen|=x*128<=int64_t(OVERLAP+mx+4-128)*w;
      else if(sy<0)seen|=y*96<=int64_t(96-(192-OVERLAP-myT-4))*w;
      else seen|=y*96>=int64_t(96-(OVERLAP+myB+4))*w;
    }
    if(allL||allR||allT||allB||!seen)continue;
    slices[k]->wide_add(g,cv,n,facing);
  }
}
void swap(ds::gpu::Gpu3D& g,void*){for(auto& s:slices)if(s)s->wide_swap(g);}
void latched(ds::gpu::Gpu3D& g,void*){for(auto& s:slices)if(s)s->wide_render(g);}

void stop(){
  if(nds){auto& g=nds->gpu3d;g.wide=ds::gpu::Gpu3D::WideHooks{};}
  for(auto& s:slices)s.reset();
}
void start(){
  if(!nds||(mx<=0&&my<=0))return;
  for(int k=0;k<4;++k)if((k<2?mx:k==2?myT:myB)>0)slices[k]=std::make_unique<ds::gpu::Gpu3D>(*nds);
  auto& g=nds->gpu3d;
  g.wide.submit=on_submit;g.wide.swap=swap;g.wide.latched=latched;g.wide.user=nullptr;
  g.wide.box[0]=g.wide.box[1]=mx?mx+4:0;
  g.wide.box[2]=myT?myT+4:0;
  g.wide.box[3]=myB?myB+4:0;
}
// The panel asked for now: the frame's shape and the margins, the slices on or off.
void read_panel(bool force){
  retro_variable var{"pokedaisy_panel",nullptr};
  const std::string value=environment(RETRO_ENVIRONMENT_GET_VARIABLE,&var)&&var.value?var.value:"";
  if(!force&&value==panel)return;
  panel=value;
  int w=0,h=0,nx=0,ny=0;
  if(std::sscanf(value.c_str(),"%dx%d",&w,&h)==2&&w>0&&h>0){
    if(w*3>h*4)nx=std::clamp((384*w/h-512)/4,0,MAX_X);       // wider than 4:3: margins across
    else if(h*4>w*3)ny=std::clamp((512*h/w-384)/4,0,MAX_Y);  // taller: above and below
  }
  if(nx==mx&&ny==my&&!force)return;
  mx=nx;my=ny;outW=512+4*mx;outH=384+4*my;
  myT=std::min(my*3/2,MAX_Y);myB=2*my-myT;
  stop();
  if((mx>0||my>0)&&game)start();
}
// A 3D colour (RGB666, alpha in bits 24-28) as the top screen shows it: through engine A's master brightness.
uint32_t shade(uint32_t c,uint16_t mb){
  uint32_t r=c&63,g=(c>>8)&63,b=(c>>16)&63;
  const uint32_t mode=mb>>14,f=std::min<uint32_t>(mb&31,16);
  if(mode==1){r+=((63-r)*f)>>4;g+=((63-g)*f)>>4;b+=((63-b)*f)>>4;}
  else if(mode==2){r-=(r*f+15)>>4;g-=(g*f+15)>>4;b-=(b*f+15)>>4;}
  return (((r<<2)|(r>>4))<<16)|(((g<<2)|(g>>4))<<8)|((b<<2)|(b>>4));
}
bool ready(int k){return slices[k]&&slices[k]->wide_ready();}
// The slice's pixel at screen pixel (x, y) (DS pixels, the screen's (0, 0) at its top left).
uint32_t at(int k,int x,int y){
  const int px=std::clamp(x-SX[k]*(256-OVERLAP),0,255),py=std::clamp(y-SY[k]*(192-OVERLAP),0,191);
  return slices[k]->wide_line(py)[px];
}
}

// The frame out (XRGB8888, the 512 x 384 stage): as it is, or in the middle of the wide one.
void emit(const uint32_t* px,bool field3D){
  using namespace wide;
  if(mx<=0&&my<=0){geometry(512,384);submit(px,512,384);return;}
  const unsigned W=outW,H=outH,X0=2*mx,Y0=2*myT;
  geometry(W,H);
  field3D=field3D&&(mx?ready(0)&&ready(1):(!myT||ready(2))&&(!myB||ready(3)));
  uint16_t mb=0;
  bool engineA=true;
  for(int e=0;e<2;++e)if(nds->gpu.engine[e].screen()==0){mb=nds->gpu.engine[e].master_bright();engineA=e==0;}
  // Where no 3D is drawn: the top screen's backdrop (its palette's colour 0), as the 2D engine shows it.
  uint32_t back;
  {
    const uint32_t c=nds->bus.dma_read32(ds::Cpu::ARM9,engineA?0x05000000:0x05000400)&0x7FFF;
    back=shade(((c&31)<<1)|(((c>>5)&31)<<9)|(((c>>10)&31)<<17),mb);
  }
  auto near=[](uint32_t a,uint32_t b){
    const int dr=int((a>>16)&255)-int((b>>16)&255),dg=int((a>>8)&255)-int((b>>8)&255),db=int(a&255)-int(b&255);
    return std::abs(dr)<=32&&std::abs(dg)<=32&&std::abs(db)<=32;
  };
  auto lit=[](uint32_t c){return ((c>>24)&31)!=0;};
  // The 3D's clear colour as a slice's lines hold it (RGB666 | A5 << 24, render3d's rgb15_to_666): above and below,
  // a pixel no polygon drew - a sky colour where the game has left the map unloaded - goes dark like an undrawn one.
  uint32_t clear;
  {
    const uint32_t a=nds->gpu3d.render_state().clear_attr1;
    const uint32_t c=a&0x7FFF;
    auto ch=[c](unsigned sh){uint32_t v=(sh==0?(c<<1):(c>>sh))&0x3E;return v?v+1:0u;};
    clear=ch(0)|(ch(4)<<8)|(ch(9)<<16)|(((a>>16)&31)<<24);
  }
  auto drawn=[&](uint32_t c){return lit(c)&&(c&0x1F3F3F3Fu)!=clear;};
  for(unsigned Y=0;Y<H;++Y){
    uint32_t* row=out+Y*W;
    const int sy=int(Y)-int(Y0);   // stage row
    if(sy>=0&&sy<384)std::memcpy(row+X0,px+sy*512,512*4);
    if(!field3D){
      if(sy<0||sy>=384){std::fill(row,row+W,0u);continue;}
      std::fill(row,row+X0,0u);std::fill(row+X0+512,row+W,0u);continue;
    }
    if(mx){
      const int y=sy/2;
      for(unsigned X=0;X<X0;++X){
        const uint32_t c=at(0,int(X/2)-mx,y);
        row[X]=lit(c)?shade(c,mb):back;
        const uint32_t cr=at(1,256+int(X/2),y);
        row[X0+512+X]=lit(cr)?shade(cr,mb):back;
      }
      // The DS outlines the 3D where the screen cuts it (edge marking): a dark line once the picture goes on past
      // it. The screen's outer column takes the slice's own pixel there - where the next one in shows the slice's
      // colour too, so it's the 3D and not a text box or the HUD over it.
      const uint32_t in=at(0,1,y),edge=at(0,0,y);
      if(lit(in)&&lit(edge)&&near(px[sy*512+2],shade(in,mb)))row[X0]=row[X0+1]=shade(edge,mb);
      const uint32_t inR=at(1,254,y),edgeR=at(1,255,y);
      if(lit(inR)&&lit(edgeR)&&near(px[sy*512+509],shade(inR,mb)))row[X0+510]=row[X0+511]=shade(edgeR,mb);
    }else if(sy<0||sy>=384){
      const int y=sy<0?-((-sy+1)/2):sy/2;   // DS row, floor
      const int k=sy<0?2:3;
      // Where nothing is drawn, black rather than the backdrop (a sky colour over the whole width), and the margin
      // fading out to its outer edge: what's ahead goes off into the dark instead of stopping at a flat colour.
      const unsigned span=sy<0?Y0:H-Y0-384,from=sy<0?Y:H-1-Y;   // rows to the outer edge
      const unsigned ramp=std::max(1u,span*2/5);
      const unsigned f=from>=ramp?256:256*from/ramp;
      for(unsigned X=0;X<W;++X){
        const uint32_t c=at(k,int(X/2),y);
        uint32_t o=drawn(c)?shade(c,mb):0;
        if(f<256)o=((((o>>16)&255)*f>>8)<<16)|((((o>>8)&255)*f>>8)<<8)|((o&255)*f>>8);
        row[X]=o;
      }
    }
  }
  if(my&&field3D){
    // The screen's top and bottom rows, the same outline fix.
    for(int side=0;side<2;++side){
      const int k=side?3:2,ey=side?191:0,ny=side?190:1;
      uint32_t* r0=out+(Y0+ey*2)*W+X0;
      uint32_t* r1=r0+W;
      for(int x=0;x<256;++x){
        const uint32_t in=at(k,x,ny),edge=at(k,x,ey);
        if(lit(in)&&lit(edge)&&near(px[(ny*2)*512+x*2],shade(in,mb)))r0[2*x]=r0[2*x+1]=r1[2*x]=r1[2*x+1]=shade(edge,mb);
      }
    }
  }
  if(hudW==int(W/2)&&hudH>0&&unsigned(2*hudH)<=H){
    for(unsigned y=0;y<unsigned(2*hudH);++y){
      const uint32_t* src=hud+(y/2)*hudW;
      uint32_t* row=out+y*W;
      for(unsigned x=0;x<W;++x){
        const uint32_t p=src[x/2];
        if(p==QUICK_CLEAR)continue;
        const unsigned a=p>>24?p>>24:255;
        const uint32_t old=row[x];
        const unsigned r=(((p>>16)&255)*a+((old>>16)&255)*(255-a))/255;
        const unsigned g=(((p>>8)&255)*a+((old>>8)&255)*(255-a))/255;
        const unsigned b=((p&255)*a+(old&255)*(255-a))/255;
        row[x]=(r<<16)|(g<<8)|b;
      }
    }
  }
  hudW=hudH=0;
  submit(out,W,H);
}

uint32_t read32(uint32_t a) {
  if (!nds || a<0x02000000 || a>0x023ffffc) return 0;
  return nds->bus.dma_read32(ds::Cpu::ARM9,a);
}
void write32(uint32_t a,uint32_t value) {
  if(nds && a>=0x02000000 && a<=0x023ffffc)nds->bus.dma_write32(ds::Cpu::ARM9,a,value);
}
void set_wild_rates(){
  const auto now=std::time(nullptr);
  const auto* local=std::localtime(&now);
  const int savedMonth=ds_unova_month(nds->bus.main_ram.get(),nds->bus.main_ram_size(),companionGame);
  const int month=savedMonth>=1 && savedMonth<=12?savedMonth:local?local->tm_mon+1:1;
  const auto rates=ds_wild_rates(companionGame,month,soulSilver);
  pk_nds_field_set_rates(rates.triples,rates.count);
  pk_nds_field_reset_wild();
}
void clear_battle() {
  battleStrip={};battleTap={};drawnBattle={};battleBalls={};battleName.clear();battlePanelArtValid=false;
  battleHeld=0;holdStrip=restoreGrace=0;battleActive=injectBattleTap=wasInBattle=false;
}
bool wants_strip() {
  return game && battleStyle && shell.battleStrip && shell.layout==DS_ONE && !screen_override &&
    !manualTouch && !companion && !quickOpen;
}
DsBattleView battle_view(int bits) {
  if(!wants_strip() || (buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT)))return {};
  return ds_read_battle(game,read32,bits);
}
void update_battle(unsigned pressed,int field) {
  battleHeld&=buttons;
  if(!wants_strip() || (buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT))) {
    battleStrip={};battleTap={};injectBattleTap=false;return;
  }
  const auto view=battle_view(field);
  if(view.panel!=DsBattlePanel::Native)battleHeld|=buttons&battleKeys;
  if(restoreGrace>0){--restoreGrace;pressed=0;}
  auto hit=[&](unsigned id){return bool(pressed&(1u<<id));};
  battleStrip.select(view,
    hit(RETRO_DEVICE_ID_JOYPAD_RIGHT)-hit(RETRO_DEVICE_ID_JOYPAD_LEFT),
    hit(RETRO_DEVICE_ID_JOYPAD_DOWN)-hit(RETRO_DEVICE_ID_JOYPAD_UP),
    hit(RETRO_DEVICE_ID_JOYPAD_A),hit(RETRO_DEVICE_ID_JOYPAD_B));
  injectBattleTap=battleStrip.tapFrames>0;
  battleTap=battleStrip.tick();
}
void clear_field_actions() {
  pcJob=flyCallJob=0;
  flyChecking=autoRunThisFrame=false;
  flyWhy=nullptr;
  pk_nds_field_reset_wild();
  pk_nds_call_reset();
}

void clear() {
  clear_field_actions();
  clear_hud_focus();
  stageReady=false;
  pk_nds_field_set_rates(nullptr,0);
  shell.dexArt={};shell.dexPage={};shell.dexCachedSpecies=0;
  clear_battle();
  flyJob={};flyPicker=heldQuickConfirm=false;flyDestinations.clear();
  supported=companion=quickOpen=select_used=select_pulse=false;quickSelected=0; game=nullptr; snapshot={};map={};icons.clear();hudItemIcons.clear();repelFull=0;
  companionHold=0;
  battleStyle=nullptr;
  companionGame=DsGame::Unsupported;
  soulSilver=false;
  old_buttons=buttons=tick=0;std::fill(held_frames,held_frames+16,0);
  screen_override=0;cursor_x=128;cursor_y=96;layout=0;
  scene.reset();arrangement={};manualTouch=fieldValid=fieldMenuFlow=false;
  backdrop={};
}
bool env(unsigned cmd,void* data) {
  if(cmd==RETRO_ENVIRONMENT_SET_VARIABLES) {
    auto vars=static_cast<const retro_variable*>(data);
    std::vector<retro_variable> all;
    for(;vars && vars->key;++vars)all.push_back(*vars);
    all.push_back({"pokedaisy_ds_layout","Initial DS layout (saved companion settings take priority); auto|top|bottom|stacked"});
    all.push_back({"pokedaisy_ds_hud","Initial party HUD (saved companion settings take priority); enabled|disabled"});
    all.push_back({nullptr,nullptr});
    return environment(cmd,all.data());
  }
  if(cmd==RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
    // DSperate's own XRGB8888 where the frontend takes it: a frontend converts whatever it is given to its screen's
    // format (NXRedux: either one to RGBA, in NEON), so a pass to RGB565 here was the same work done twice. RGB565 only
    // for a frontend that takes nothing else.
    retro_pixel_format f=RETRO_PIXEL_FORMAT_XRGB8888;
    rgb565=!environment(cmd,&f);
    if(!rgb565)return true;
    f=RETRO_PIXEL_FORMAT_RGB565;
    return environment(cmd,&f);
  }
  if(cmd==RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK && data) {
    // Refused: the wrapper would turn dsperate_frameskip off. Keep it, on the core's own timing.
    ownSkip=!environment(cmd,data);
    return true;
  }
  return environment(cmd,data);
}
void geometry(unsigned w,unsigned h) {
  if(width==w && height==h)return;
  width=w;height=h;
  retro_game_geometry g{w,h,512+4*248,384+4*184,float(w)/h};
  environment(RETRO_ENVIRONMENT_SET_GEOMETRY,&g);
}
void refresh() {
  auto next=ds_read_companion(nds->bus.main_ram.get(),nds->bus.main_ram_size(),companionGame);
  // On the newer games an invalid transient read can occur as the game
  // decrypts a record in place. Keep the last coherent snapshot.
  if(next.valid||companionGame==DsGame::Platinum){
    snapshot=std::move(next);
    shell.guideLoadedPage=-1;
    pd_ui_guide_reset(&shell.frame);
  }
  ds_map_update(map,snapshot);
  hudLayerValid=false;
}
void refresh_battle_companion(){
  ds_read_battle_companion(game,read32,nds->bus.main_ram.get(),nds->bus.main_ram_size(),snapshot,shell.battle);
  shell.update_battle();
}
void choose_layout() {
  int bits=game?pk_nds_field_state(game,read32,nullptr,0):0;
  const bool inBattle=bits&PK_NDS_FIELD_BATTLE;
  // A screen peek in the overworld shouldn't carry into the next fight.
  // Screen changes made during a fight still give access to the native panel.
  if(inBattle&&!wasInBattle&&shell.layout==DS_ONE&&shell.battleStrip){
    screen_override=0;
    manualTouch=false;
  }
  wasInBattle=inBattle;
  auto one_colour=[](const uint32_t* px){
    uint32_t first=px[0]&0xffffff;
    for(int gy=0;gy<12;++gy)for(int gx=0;gx<16;++gx)
      if((px[(gy*16+8)*256+gx*16+8]&0xffffff)!=first)return false;
    return true;
  };
  bool topBlank=one_colour(nds->gpu.framebuffer(0)),bottomBlank=one_colour(nds->gpu.framebuffer(1));
  const bool fieldApp=!inBattle&&(bits&PK_NDS_FIELD_MENU);
  const bool fieldScreen=bits&PK_NDS_FIELD_SCREEN;
  const bool nativeX=(buttons&~hudConsumed&(1u<<RETRO_DEVICE_ID_JOYPAD_X)) &&
    shell.companion_id()!=RETRO_DEVICE_ID_JOYPAD_X && shell.screen_id()!=RETRO_DEVICE_ID_JOYPAD_X;
  if(fieldApp || (fieldScreen&&nativeX))fieldMenuFlow=true;
  else if(inBattle || (fieldScreen&&!(bits&PK_NDS_FIELD_LOCKED)&&!topBlank))fieldMenuFlow=false;
  if(topBlank&&!bottomBlank)bits|=PK_NDS_FIELD_TOUCH;
  const auto view=battle_view(bits);
  const bool stripWasUp=battleActive;
  battleActive=view.panel!=DsBattlePanel::Native;
  if(battleActive){
    const bool changed=drawnBattle.panel!=view.panel || drawnBattle.personality!=view.personality ||
       drawnBattle.partySlot!=view.partySlot || drawnBattle.species!=view.species ||
       (view.panel==DsBattlePanel::Moves &&
        (drawnBattle.moves!=view.moves || drawnBattle.pp!=view.pp));
    if(changed)battlePanelArtValid=false;
    // The ball rows change with the turn, and the command panel comes up once a turn: read them then, and whenever it
    // comes back after the strip was away (a turn spent in the bag or on a failed run never opens the moves).
    if(changed || !stripWasUp){
      battleBalls=view;
      if(view.panel==DsBattlePanel::Actions && companionGame!=DsGame::Unsupported)
        ds_read_battle_balls(game,read32,nds->bus.main_ram.get(),nds->bus.main_ram_size(),
                             ds_read_companion(nds->bus.main_ram.get(),nds->bus.main_ram_size(),companionGame),battleBalls);
    }
    if(supported && !battlePanelArtValid){
      const auto* lower=nds->gpu.framebuffer(1);
      const bool ready=view.panel==DsBattlePanel::Actions?ds_platinum_has_command_art(lower):
        ds_platinum_has_move_art(lower,view);
      if(ready){
        std::copy_n(lower,battlePanelArt.size(),battlePanelArt.begin());
        battlePanelArtValid=true;
      }
    }
    if(drawnBattle.personality!=view.personality || drawnBattle.partySlot!=view.partySlot ||
       drawnBattle.species!=view.species || battleName.empty()){
      battleName=ds_battle_species(view);
      if(companionGame!=DsGame::Unsupported){
        // The prompt names the Pokemon as the game does, by its nickname: Gen 4 finds it by personality, Gen 5 by its
        // party slot (its battler carries no personality), the species checked either way.
        const auto party=ds_read_companion(nds->bus.main_ram.get(),nds->bus.main_ram_size(),companionGame).party;
        const auto mon=view.gen5?
          (view.partySlot>=0 && size_t(view.partySlot)<party.size()?party.begin()+view.partySlot:party.end()):
          std::find_if(party.begin(),party.end(),[&](const DsMon& candidate){return candidate.personality==view.personality;});
        if(mon!=party.end() && mon->species==view.species && !mon->nickname.empty())battleName=mon->nickname;
      }
    }
    drawnBattle=view;
    drawnBattle.balls=battleBalls.balls;
    drawnBattle.partyBalls=battleBalls.partyBalls;drawnBattle.foeBalls=battleBalls.foeBalls;
    holdStrip=18;
  }else if(!wants_strip() || !inBattle || ((bits&PK_NDS_FIELD_TOUCH_BATTLE_MENU) &&
            !(battleStrip.moveCommitFrames>0&&battleStrip.cancelingMove))){
    holdStrip=0;drawnBattle={};battlePanelArtValid=false;
  }else if(holdStrip>0 && !(bits&PK_NDS_FIELD_TOUCH_FADES)){
    --holdStrip;
  }
  // Keep the stale move panel off the screen during a single-battle commit.
  // Explicit native transitions (including Bag and party) retain their fade.
  int displayBits=bits;
  if(inBattle&&battleStrip.moveCommitFrames>0&&!(bits&PK_NDS_FIELD_TOUCH_FADES))
    displayBits&=~(PK_NDS_FIELD_TOUCH|PK_NDS_FIELD_TOUCH_BATTLE_MENU);
  // PIP already exposes the native battle panel; it must not float twice.
  scene.tick(displayBits,topBlank,bottomBlank,!battleActive&&(shell.autoBattles||wants_strip())&&shell.layout!=DS_PIP);
  int mode=screen_override==1?DS_BOTTOM:screen_override==2?DS_TOP:shell.layout;
  arrangement=ds_arrange(mode,scene,game!=nullptr,manualTouch&&screen_override==0);
  if(mode==DS_ONE&&!manualTouch&&game&&!supported){
    arrangement=ds_arrange_one_screen(arrangement,scene,bits,topBlank,fieldValid,fieldMenuFlow);
  }else if(mode==DS_ONE&&!manualTouch&&fieldValid&&fieldMenuFlow&&!inBattle){
    // Present the last field image through the game's loading fades. The
    // native menu is shown only once the screen(s) it needs are drawn.
    if(fieldApp&&(bits&PK_NDS_FIELD_TOP_ONLY)&&!topBlank)
      arrangement={{80,60,352,264},{},false,true,true};
    else if(fieldApp&&!(bits&(PK_NDS_FIELD_TOP_ONLY|PK_NDS_FIELD_TOUCH_ONLY))&&!topBlank&&!bottomBlank)
      arrangement={{0,96,256,192},{256,96,256,192},false,true,true};
    else if(topBlank || (fieldApp&&!(bits&PK_NDS_FIELD_TOUCH_ONLY)))
      arrangement={{},{},false,false,true};
  }
  if(battleActive && !scene.dark)arrangement={{0,0,512,384},{}};
  layout=mode==DS_BOTTOM?1:mode==DS_SIDE?2:mode==DS_STACKED?3:0;
  if(scene.hud&&!scene.dark&&!(bits&PK_NDS_FIELD_LOCKED)){std::memcpy(fieldFrame,nds->gpu.framebuffer(0),sizeof fieldFrame);fieldValid=true;++fieldRevision;}
}
void draw_companion() {
  presentedField=false;
  pd_canvas c;pd_canvas_init(&c,341,256,ui_pixels);
  shell.draw(&c,snapshot,companionGame!=DsGame::Unsupported,map,icons);
  geometry(341,256);
  submit(ui_pixels,341,256);
}
constexpr int repelItems[]={79,76,77};
int repelChoices[3],repelCount=0,repelSelected=0;
std::string repelLabels[3];
bool repelPicker=false;
bool field_free(int bits){
  return (bits&PK_NDS_FIELD_SCREEN)&&!(bits&(PK_NDS_FIELD_LOCKED|PK_NDS_FIELD_BATTLE|PK_NDS_FIELD_MENU|PK_NDS_FIELD_TEXT));
}
bool field_free(){return field_free(pk_nds_field_state(game,read32,nullptr,0));}
// A quick-menu action's answer ("No one in your party knows Fly."), shown by the core itself for two seconds. A frontend
// message (RETRO_ENVIRONMENT_SET_MESSAGE) is only logged by NXRedux, so a refused action looked like a dropped press.
std::string notice;
unsigned noticeFrames=0;
void quick_notice(const char* label){
  notice=label;
  noticeFrames=120;
}
// A slate tab under the top edge, the text at 2x where it fits the frame and at 1x where it doesn't.
void draw_notice(uint32_t* out){
  pd_canvas c;pd_canvas_init(&c,512,384,out);
  const int scale=2*pd_text_width(notice.c_str())<=480?2:1;
  const int w=std::min(504,scale*pd_text_width(notice.c_str())+24), h=16*scale+8, x=(512-w)/2, y=8;
  pd_round_rect(&c,x,y,w,h,4,0x182028);
  pd_round_rect(&c,x+2,y+2,w-4,h-4,3,0xf8f8f8);
  pd_round_rect(&c,x+4,y+4,w-8,h-8,2,0x283848);
  pd_clip(&c,x+6,y+4,w-12,h-8);
  pd_text_scaled(&c,x+12,y+4-scale,notice.c_str(),0xf8f8f8,0x101820,scale);
  pd_unclip(&c);
}
// DSperate's CPU-visible bus handles TCM mirrors and invalidates recompiled
// code on writes. The field job and its ROM-specific recipes are shared with Android.
PkNdsCallMemory field_call_memory(){
  constexpr auto cpu=ds::Cpu::ARM9;
  return {nds.get(),
    [](void* ctx,uint32_t addr){return static_cast<ds::NDS*>(ctx)->bus.dma_read32(cpu,addr);},
    [](void* ctx,uint32_t addr,uint32_t value){static_cast<ds::NDS*>(ctx)->bus.dma_write32(cpu,addr,value);},
    [](void* ctx,uint32_t addr,void* dst,uint32_t len){
      auto& bus=static_cast<ds::NDS*>(ctx)->bus;
      auto* bytes=static_cast<uint8_t*>(dst);
      for(uint32_t i=0;i<len;++i)bytes[i]=bus.dma_read8(cpu,addr+i);
    },
    [](void* ctx,uint32_t addr,const void* src,uint32_t len){
      auto& bus=static_cast<ds::NDS*>(ctx)->bus;
      const auto* bytes=static_cast<const uint8_t*>(src);
      for(uint32_t i=0;i<len;++i)bus.dma_write8(cpu,addr+i,bytes[i]);
    }};
}
void open_pc(){
  if(!field_free() || !pk_nds_call_supported(game) || nds->cpu(ds::Cpu::ARM9).itcm_size<0x02000000){
    quick_notice("PC is unavailable here.");
    return;
  }
  pcJob=pk_nds_call_action(field_call_memory(),game,PK_NDS_ACTION_PC,nullptr);
  if(!pcJob){quick_notice("PC is unavailable here.");return;}
  heldQuickConfirm=true;
  quickOpen=false;
}
void update_pc(){
  if(!pcJob)return;
  pk_nds_call_frame(field_call_memory(),game);
  const int status=pk_nds_call_status(pcJob);
  if(!status)return;
  uint32_t results[PK_NDS_MAX_RESULTS];
  if(status<0 || (game->gen==5 && (!pk_nds_call_results(pcJob,results) || !results[1])))
    quick_notice("PC could not open here.");
  pcJob=0;
}

// Verified native overlays for Black 2 USA/Europe revision 0. Use input so the
// game checks field eligibility and owns confirmation, animation and travel.
constexpr int BLACK2_PARTY_OVERLAY=165,BLACK2_FLY_OVERLAY=144;
bool fly_supported(){return companionGame==DsGame::Black2 || (pk_nds_call_supported(game)&&!ds_fly_spots(companionGame).empty());}
int field_overlay(){
  const uint32_t system=read32(game->fieldSystemPtr);
  const uint32_t procs=read32(system+game->offProcSys);
  const uint32_t proc=read32(procs+game->offCurrentProc);
  return proc>=0x02000000&&proc<0x02400000?int(read32(proc+game->offProcOverlay)):-1;
}

void open_fly(){
  if(!field_free()){quick_notice("Fly is available in the overworld.");return;}
  const auto current=ds_read_companion(nds->bus.main_ram.get(),nds->bus.main_ram_size(),companionGame);
  if(!current.valid){quick_notice("Party data is unavailable.");return;}
  const auto flyer=std::find_if(current.party.begin(),current.party.end(),[](const DsMon& mon){
    return !mon.egg&&std::find(mon.moves.begin(),mon.moves.end(),19)!=mon.moves.end();
  });
  if(flyer==current.party.end()){quick_notice("No one in your party knows Fly.");return;}
  flySlot=int(flyer-current.party.begin());
  flyPersonality=flyer->personality;
  flyDestinations.clear();
  if(companionGame!=DsGame::Black2){
    const auto& spots=ds_fly_spots(companionGame);
    const uint32_t args[]={game->gen!=5?uint32_t(spots.size()):0,0,0,0};
    if(nds->cpu(ds::Cpu::ARM9).itcm_size>=0x02000000)
      flyCallJob=pk_nds_call_action(field_call_memory(),game,PK_NDS_ACTION_FLY_CHECK,args);
    if(!flyCallJob){quick_notice("Fly is unavailable here.");return;}
    flyChecking=true;
    heldQuickConfirm=true;
    quickOpen=false;
    return;
  }
  for(const auto& point:pd_ds_data::sequelMapPoints){
    const int kind=point[4],bit=point[5]-2469;
    if((kind!=0&&kind!=3&&kind!=5)||bit<0||bit>=62||!(current.arrivals>>bit&1))continue;
    flyDestinations.push_back({point[2],point[3],ds_place(point[0],companionGame)});
  }
  if(flyDestinations.empty()){quick_notice("No Fly destinations unlocked.");return;}
  flySelected=0;
  flyPicker=true;
}

void update_fly_call(){
  if(!flyCallJob)return;
  pk_nds_call_frame(field_call_memory(),game);
  const int status=pk_nds_call_status(flyCallJob);
  if(!status)return;
  uint32_t results[PK_NDS_MAX_RESULTS]{};
  const bool ok=status>0 && pk_nds_call_results(flyCallJob,results);
  flyCallJob=0;
  if(!flyChecking){
    if(!ok || (game->gen==5 && !results[1]))quick_notice("Fly could not start here.");
    return;
  }
  flyChecking=false;
  if(!ok || results[1]){
    const char* message="Fly is unavailable here.";
    if(ok && game->gen!=5 && results[1]==2)message="You need the badge to use Fly.";
    if(ok && game->gen!=5 && results[1]==3)message="You cannot Fly with a partner.";
    flyWhy=message;
    quickOpen=true;
    return;
  }
  const auto current=ds_read_companion(nds->bus.main_ram.get(),nds->bus.main_ram_size(),companionGame);
  const auto& spots=ds_fly_spots(companionGame);
  for(size_t i=0;i<spots.size();++i){
    const auto& spot=spots[i];
    const bool open=companionGame==DsGame::Platinum
      ? i+2<std::size(results) && results[i+2] : ds_fly_unlocked(current,spot);
    if(open)flyDestinations.push_back({spot.x,spot.z,ds_place(spot.nameMap,companionGame),spot.header});
  }
  if(flyDestinations.empty()){
    flyWhy="No Fly destinations unlocked";
    quickOpen=true;
    return;
  }
  flySelected=0;
  flyPicker=quickOpen=true;
}

void confirm_fly(){
  if(!field_free()){quick_notice("Fly is unavailable here.");return;}
  const auto current=ds_read_companion(nds->bus.main_ram.get(),nds->bus.main_ram_size(),companionGame);
  if(flySlot<0 || size_t(flySlot)>=current.party.size()){
    quick_notice("Party data is unavailable.");return;
  }
  const auto& flyer=current.party[flySlot];
  if(flyer.personality!=flyPersonality || flyer.egg ||
     std::find(flyer.moves.begin(),flyer.moves.end(),19)==flyer.moves.end()){
    quick_notice("Your Fly user has changed.");return;
  }
  const auto& spot=flyDestinations[flySelected];
  if(companionGame==DsGame::Black2){
    flyJob={};flyJob.stage=FlyStage::Menu;flyJob.started=tick;
    flyJob.slot=flySlot;flyJob.destination=spot;
  }else{
    const uint32_t args[]={uint32_t(flySlot),uint32_t(spot.header),
      game->gen!=5?uint32_t(spot.x):0,game->gen!=5?uint32_t(spot.y):0};
    flyCallJob=pk_nds_call_action(field_call_memory(),game,PK_NDS_ACTION_FLY,args);
    if(!flyCallJob){quick_notice("Fly is unavailable here.");return;}
    heldQuickConfirm=true;
  }
  flyPicker=quickOpen=false;
}

// Match the whole native cyan Fly label so the shortcut works even when
// other field moves change its row in the party member's submenu.
int fly_row(const uint32_t* bottom){
  constexpr uint16_t glyph[]={0x1f,0xc1,0x81,0x81,0x4481,0x448f,0x4481,0x4481,0x7881,0x4081,0x4000,0x3800};
  const auto mask=[&](int y){
    uint16_t bits=0;
    for(int x=0;x<15;++x){
      const auto p=bottom[y*256+162+x];
      const int red=(p>>16)&255,green=(p>>8)&255,blue=p&255;
      if(green>125&&blue>125&&green>red+30)bits|=1<<x;
    }
    return bits;
  };
  for(int y=45;y<177;++y){
    if(mask(y)!=glyph[0])continue;
    unsigned row=1;
    while(row<std::size(glyph)&&mask(y+row)==glyph[row])++row;
    if(row==std::size(glyph))return y+5;
  }
  return -1;
}

void update_fly(){
  auto& job=flyJob;
  if(!job.active())return;
  job.key=0;
  if(job.tapFrames){--job.tapFrames;return;}
  const unsigned elapsed=tick-job.started;
  const int bits=pk_nds_field_state(game,read32,nullptr,0);
  const int overlay=field_overlay();
  if(elapsed>240||(bits&PK_NDS_FIELD_BATTLE)){
    job={};quick_notice("Fly could not open here.");return;
  }
  const auto advance=[&](FlyStage stage){job.stage=stage;job.started=tick;job.ready=0;};
  const auto tap=[&](int x,int y,FlyStage stage){job.x=x;job.y=y;job.tapFrames=5;advance(stage);};
  switch(job.stage){
  case FlyStage::Menu:
    if(!field_free()){job={};return;}
    job.key=1u<<RETRO_DEVICE_ID_JOYPAD_X;
    advance(FlyStage::Party);
    break;
  case FlyStage::Party:
    if((bits&(PK_NDS_FIELD_SCREEN|PK_NDS_FIELD_TOUCH))==(PK_NDS_FIELD_SCREEN|PK_NDS_FIELD_TOUCH)
       && elapsed>=55&&!scene.bottomBlank)tap(60,47,FlyStage::Member);
    break;
  case FlyStage::Member:
    if(overlay!=BLACK2_PARTY_OVERLAY||scene.bottomBlank){job.ready=0;break;}
    if(++job.ready>=24)tap(job.slot%2?192:64,32+(job.slot/2)*48+(job.slot%2)*8,FlyStage::Move);
    break;
  case FlyStage::Move:
    if(overlay!=BLACK2_PARTY_OVERLAY||elapsed<12)break;
    if(const int y=fly_row(nds->gpu.framebuffer(1));y>=0)tap(198,y,FlyStage::Town);
    break;
  case FlyStage::Town:
    if(overlay!=BLACK2_FLY_OVERLAY||scene.bottomBlank){job.ready=0;break;}
    if(++job.ready>=24)tap(job.destination.x,job.destination.y,FlyStage::Release);
    break;
  case FlyStage::Release:
    // A held since the picker must not confirm the native map.
    heldQuickConfirm=buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_A);
    job={};
    break;
  case FlyStage::Idle: break;
  }
}

void open_repel(){
  if(!field_free()){quick_notice("Repel is available in the overworld.");return;}
  refresh();
  if(snapshot.repelSteps){quick_notice("Repel is still active.");return;}
  repelCount=0;
  for(int id:repelItems){
    const auto item=std::find_if(snapshot.bag.begin(),snapshot.bag.end(),[&](const DsItem& i){return i.id==id&&i.pocket==0;});
    if(item==snapshot.bag.end())continue;
    repelChoices[repelCount]=id;
    repelLabels[repelCount++]=std::string(ds_item(id,companionGame))+" x"+std::to_string(item->count);
  }
  if(!repelCount){quick_notice("No Repel in the Bag.");return;}
  repelPicker=true;
  repelSelected=0;
}
void use_repel(){
  using R=DsRepelResult;
  const R result=field_free()?ds_use_repel(nds->bus.main_ram.get(),nds->bus.main_ram_size(),
    companionGame,repelChoices[repelSelected],[](uint32_t addr,uint8_t value){
      nds->bus.dma_write8(ds::Cpu::ARM9,addr,value);
    }):R::NotAvailable;
  if(result==R::Used){refresh();repelFull=0;}
  quick_notice(result==R::Used?"Repel used.":result==R::StillActive?"Repel is still active.":
               result==R::NoneLeft?"No Repel in the Bag.":"Repel is unavailable here.");
}
int ds_shortcuts(quick_item (&items)[shortcutCapacity]){
  int count=0;
  const bool field=field_free();
  const char* fieldWhy=field?nullptr:"Overworld only";
  const bool canCall=nds->cpu(ds::Cpu::ARM9).itcm_size>=0x02000000;
  const bool hasRepel=std::any_of(snapshot.bag.begin(),snapshot.bag.end(),[](const DsItem& item){
    return item.pocket==0 && item.count>0 &&
      std::find(std::begin(repelItems),std::end(repelItems),item.id)!=std::end(repelItems);
  });
  const bool hasFlyer=std::any_of(snapshot.party.begin(),snapshot.party.end(),[](const DsMon& mon){
    return !mon.egg && std::find(mon.moves.begin(),mon.moves.end(),19)!=mon.moves.end();
  });
  if(pk_nds_call_supported(game))
    items[count++]={QUICK_PC,false,false,fieldWhy?fieldWhy:!canCall?"PC unavailable":nullptr};
  if(fly_supported())
    items[count++]={QUICK_FLY,false,false,fieldWhy?fieldWhy:!hasFlyer?"No party member knows Fly":
      (!canCall && companionGame!=DsGame::Black2)?"Fly unavailable":flyWhy};
  if(companionGame!=DsGame::Unsupported){
    items[count++]={QUICK_REPEL,false,false,fieldWhy?fieldWhy:snapshot.repelSteps?"Repel still active":
      !hasRepel?"No Repel in the Bag":nullptr};
    if(game)items[count++]={QUICK_WILD,shell.settings.wildOff,false,fieldWhy};
    items[count++]={QUICK_HUD,false,false,hudHits.n?nullptr:"No HUD on screen"};
  }
  items[count++]={QUICK_COMPANION,false,false,nullptr};
  return count;
}
void draw_quick() {
  presentedField=false;
  pd_canvas c;pd_canvas_init(&c,341,256,quick_pixels);
  pd_game presentation{};
  quick_item items[shortcutCapacity]{};
  const int count=ds_shortcuts(items);
  quick_set_grid(shell.settings.quickGrid);
  if(repelPicker){
    const char* labels[3]={repelLabels[0].c_str(),repelLabels[1].c_str(),repelLabels[2].c_str()};
    quick_list_draw(&c,"REPEL",labels,repelCount,repelSelected,shell.settings.hudLook);
  }else if(flyPicker){
    std::vector<const char*> labels;
    for(const auto& destination:flyDestinations)labels.push_back(destination.name);
    quick_list_draw(&c,"FLY TO",labels.data(),int(labels.size()),flySelected,shell.settings.hudLook);
  }else quick_draw(&c,&presentation,items,count,std::min(quickSelected,count-1),shell.settings.hudLook);
  quick_finish(&c);
  // Overlay the menu on the paused composition at the same geometry as gameplay.
  if(!stageReady){
    choose_layout();
    ds_compose(stage,arrangement,nds->gpu.framebuffer(0),nds->gpu.framebuffer(1));
    backdrop.held=false;
    stageReady=true;stageMarked=false;
  }else if(stageMarked){
    // Never with a battle strip: the HUD needs a free field, the cross a touch screen in view.
    ds_compose(stage,arrangement,nds->gpu.framebuffer(0),nds->gpu.framebuffer(1),fieldValid?fieldFrame:nullptr);
    backdrop.held=false;
    stageMarked=false;
  }
  // The menu is drawn at 341x256 and shown at 1.5x. Nearest-neighbour doubled every other column and row and left the
  // rest single: uneven strokes. Area-weighted, as ds_compose scales a popup: an output pixel takes the menu pixels it
  // covers by how much, so two in three are one menu pixel whole and the third is half of each neighbour.
  static int cx[512],wx[512],cy[384],wy[384];
  static bool spansReady=false;
  if(!spansReady){
    auto spans=[](int n,int source,int* first,int* share){
      for(int i=0;i<n;++i){
        const int a=i*source*256/n, b=(i+1)*source*256/n, edge=(a/256+1)*256;
        first[i]=std::min(a/256,source-1);
        share[i]=b<=edge?256:(edge-a)*256/(b-a);
      }
    };
    spans(512,341,cx,wx);spans(384,256,cy,wy);
    spansReady=true;
  }
  for(unsigned y=0;y<384;++y)for(unsigned x=0;x<512;++x){
    uint32_t base=stage[y*512+x]&0xffffff;
    base=(base&0xfefefe)>>1;
    // The covered menu pixels' colour and alpha, summed by share (a clear pixel adds nothing).
    unsigned r=0,g=0,b=0,a=0;
    auto add=[&](int sx,int sy,unsigned share){
      const uint32_t p=quick_pixels[sy*341+sx];
      if(!share || p==QUICK_CLEAR)return;
      const unsigned alpha=(p>>24)*share;
      r+=((p>>16)&255)*alpha;g+=((p>>8)&255)*alpha;b+=(p&255)*alpha;a+=alpha;
    };
    const int x1=std::min(cx[x]+1,340),y1=std::min(cy[y]+1,255);
    const unsigned sx=wx[x],sy=wy[y];
    add(cx[x],cy[y],sx*sy);add(x1,cy[y],(256-sx)*sy);add(cx[x],y1,sx*(256-sy));add(x1,y1,(256-sx)*(256-sy));
    if(a){
      // [a] is alpha (0-255) times share (0-65536); colours are already multiplied by it.
      const unsigned keep=255u*65536u-a;
      const auto over=[&](unsigned top,unsigned under){return unsigned((uint64_t(top)+uint64_t(under)*keep)/(255u*65536u));};
      base=(over(r,(base>>16)&255)<<16)|(over(g,(base>>8)&255)<<8)|over(b,base&255);
    }
    quick_stage[y*512+x]=base;
  }
  // A refused action leaves the menu open: its answer goes over the menu.
  if(noticeFrames)draw_notice(quick_stage);
  emit(quick_stage,false);
}
// An overlay that draws nothing: while a notice is up, it keeps ds_compose from holding a popup's backdrop (the notice is
// drawn into stage after it, and a held backdrop would keep it there once it is over).
void keep_composing(uint32_t*){}
void draw_strip(uint32_t* out){
  const bool moves=drawnBattle.panel==DsBattlePanel::Moves;
  // A tap in flight shows on the plate it lands on: B's is the move panel's back plate.
  const bool pressed=battleStrip.tapFrames>0;
  const int focus=!moves?battleStrip.actionFocus:pressed&&battleStrip.cancelingMove?4:battleStrip.moveFocus;
  ds_draw_battle_strip(out,*battleStyle,drawnBattle,battleName,focus,battleActive&&!pressed,tick,
                       battlePanelArtValid?battlePanelArt.data():nullptr,pressed);
}
void present_frame(const void* data);
void present(const void* data,unsigned,unsigned,size_t) {
  const double t0=perfOn||ownSkip?perf_now():0;
  frontendTime=0;
  present_frame(data);
  if(perfOn)perfPresent+=perf_now()-t0;
  // Called by DSperate's run right after the frame: everything since runStart but the frontend's callback.
  if(ownSkip && runStart>0)frameWork=perf_now()-runStart-frontendTime;
}
void present_frame(const void* data) {
  if(companion){hudHits.n=0;draw_companion();return;}
  // Refresh composition even when DSperate skipped rasterization: switching
  // screens must still display the latest retained framebuffer.
  (void)data;
  const double l0=perfOn?perf_now():0;
  if(hudCursor<0)choose_layout();
  if(perfOn)perfLayout+=perf_now()-l0;
  // dsperate_frameskip: a frame DSperate skipped (will_skip_frame: asked for a frame ahead, and never for a frame that
  // captures) wasn't drawn, so the screens are the last ones.
  // Where nothing of ours changed (the same layout, no cursor), show the last frame again (video(nullptr)) instead of
  // composing, converting and drawing the HUD over the same picture - the time the skip was meant to give back. Never
  // twice in a row: the HUD, the strip and a popup are at most a frame behind.
  if(fs_mode!=Skip::Off && nds->gpu.will_skip_frame() && presentedField && !repeatedLast && hudCursor<0 && !quickOpen &&
     same_arrangement(arrangement,presentedArrangement)){
    // The HUD on screen is the last frame's, so its hit boxes stay: cleared here, the HUD button went unheard after it.
    repeatedLast=true;
    show(nullptr,width,height,width*(rgb565?2:4));
    return;
  }
  repeatedLast=false;
  hudHits.n=0;
  const unsigned w=512,h=384;
  const bool showStrip=holdStrip>0 && !scene.dark && arrangement.top.w==512 && wants_strip();
  const double c0=perfOn?perf_now():0;
  ds_compose(stage,arrangement,nds->gpu.framebuffer(0),nds->gpu.framebuffer(1),
             fieldValid?fieldFrame:nullptr,showStrip?draw_strip:noticeFrames?keep_composing:nullptr,&backdrop,fieldRevision);
  stageReady=true;stageMarked=false;
  if(perfOn)perfCompose+=perf_now()-c0;
  const double h0=perfOn?perf_now():0;
  if(hudCursor<0&&(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT))&&arrangement.touch.w>0){
    int cx=arrangement.touch.x+cursor_x*arrangement.touch.w/256;
    int cy=arrangement.touch.y+cursor_y*arrangement.touch.h/192;
    stageMarked=true;
    for(int d=-4;d<=4;++d){
      if(arrangement.touch.contains(cx+d,cy))stage[cy*w+cx+d]=0xffffff;
      if(arrangement.touch.contains(cx,cy+d))stage[(cy+d)*w+cx]=0xffffff;
    }
  }
  const auto& top=arrangement.top;
  if(companionGame!=DsGame::Unsupported && shell.settings.hud && scene.hud && !scene.dark &&
     (!game || field_free()) && !fieldMenuFlow &&
     top.w>0 && top.h>0 && !arrangement.floating && !arrangement.modal) {
    if(tick%12==0 || !snapshot.valid)refresh();
    stageMarked=true;
    const bool pc=pk_nds_call_supported(game)&&nds->cpu(ds::Cpu::ARM9).itcm_size>=0x02000000;
    const bool mini=shell.settings.hudSize!=PD_HUD_LARGE;
    const bool pip=arrangement.touch.w>0 && arrangement.touch.x>top.x && arrangement.touch.x<top.x+top.w;
    // On a wide frame (EXPAND) with the top screen whole on the stage, the HUD spans the frame, to the phone's edges.
    // Upright, it spans the margin above and the screen: the party and the minimap at the phone's top corners.
    const bool wideHud=(wide::mx>0||wide::my>0) && top.x==0 && top.y==0 && top.w==512 && top.h==384 && arrangement.touch.w==0;
    const int hudW=wideHud?256+2*wide::mx:256,hudH=wideHud?192+wide::myT:192;
    const int hudRight=wideHud?hudW:pip?(arrangement.touch.x-top.x)*256/top.w:256;
    // Inputs outside the key (icon art loaded late, the snapshot) catch up at refresh(), every 12 frames.
    HudLayerKey key;
    // Whole objects, padding too (memcmp below): the settings are one object, never copied member by member.
    std::memset(&key,0,sizeof key);
    std::memcpy(&key.settings,&shell.settings,sizeof key.settings);
    key.right=hudRight;key.height=hudH;key.cursor=hudCursor;key.minimal=shell.hudMinimal;key.small=pip&&!mini;key.pc=pc;
    if(hudLayerValid && !std::memcmp(&key,&hudLayerKey,sizeof key))hudHits=hudLayerHits;
    else{
    pd_canvas c;pd_canvas_init(&c,hudW,hudH,wideHud?wide::hud:hud_pixels);
    pd_snapshot party{};party.valid=snapshot.valid;party.partyCount=std::min(6,int(snapshot.party.size()));
    for(int i=0;i<party.partyCount;++i){
      const auto& m=snapshot.party[i];auto& d=party.party[i];
      d.species=m.egg?PD_SPECIES_EGG:1;d.level=m.level;d.hp=m.hp;d.maxHp=m.maxHp;d.status=m.status;
    }
    party.bagOk=snapshot.valid;
    auto& balls=party.bag[PD_POCKET_BALLS];
    for(const auto& item:snapshot.bag){
      // Unova stores Poké Balls in Items, alongside held items and field tools.
      const bool unovaBall=(item.id>=1&&item.id<=16)||(item.id>=492&&item.id<=500)||item.id==576;
      const bool ball=ds_unova(companionGame)?unovaBall:item.pocket==6;
      if(ball&&balls.count<PD_POCKET_SLOTS)balls.items[balls.count++]={uint16_t(item.id),uint16_t(item.count)};
    }
    const int steps=snapshot.repelSteps;
    if(!steps)repelFull=0;
    else if(steps>repelFull)repelFull=hud_repel_full(steps);
    const auto encounters=ds_guide_here(snapshot,soulSilver);
    int wild=encounters.count ? 0 : -1;
    if(wild>=0){
      bool counted[650]{};
      for(int i=0;i<encounters.count;++i){
        const unsigned id=encounters.entries[i].species;
        if(!id || id>=std::size(counted) || counted[id])continue;
        counted[id]=true;
        if(id>=snapshot.dexCaught.size() || !snapshot.dexCaught[id])++wild;
      }
    }
    hud_state state{steps,repelFull,pc,wild,-1};
    state.mini=mini;
    struct HudIcons {const pd_snapshot& party;bool small;};
    HudIcons hudIcons{party,key.small};
    hud_ds_draw(&c,&party,&state,&shell.settings,&hudIcons,[](pd_canvas* c,void* ctx,const pd_mon* m,int x,int y,bool){
      const auto& hud=*static_cast<HudIcons*>(ctx);
      const int slot=m-hud.party.party;
      const auto& mon=snapshot.party[slot];
      const int size=hud.small?20:32;
      if(hud.small){x+=6;y+=5;}
      const int id=mon.egg?ds_egg_icon(companionGame):mon.species;
      if(id<0||size_t(id)>=icons.size()||icons[id].size()!=1024){
        if(companionGame!=DsGame::Platinum){
          const char number[]={char('1'+slot),0};
          pd_text(c,x+(size-8)/2,y+8,number,0xffffff,0x282828);
        }
        return;
      }
      const auto& art=icons[id];
      for(int j=0;j<size;++j)for(int i=0;i<size;++i){
        const auto p=art[(j*32/size)*32+i*32/size];
        if(p>>24)pd_fill(c,x+i,y+j,1,1,p&0xffffff);
      }
    }, [](pd_canvas* c,int id,int x,int y){
      if(id<0||size_t(id)>=hudItemIcons.size()||hudItemIcons[id].empty())return false;
      const auto& art=hudItemIcons[id];
      for(int row=0;row<PD_ITEM_ICON;++row)for(int col=0;col<PD_ITEM_ICON;++col){
        const auto p=art[(row*32/PD_ITEM_ICON)*32+col*32/PD_ITEM_ICON];
        if(p>>24)pd_fill(c,x+col,y+row,1,1,p&0xffffff);
      }
      return true;
    },79,shell.hudMinimal,hudRight,&hudHits,&ds_party_palette(companionGame));
    const auto player=ds_map_position(snapshot);
    if(!shell.hudMinimal&&!(shell.settings.hudHidden&(1<<PD_HUD_MAP))&&!map.background.empty()&&player.x>=0&&player.y>=0)
      hud_ds_minimap(&c,&shell.settings,ds_place(snapshot.map,companionGame),[](pd_canvas* c,int x,int y,int w,int h){
        ds_draw_minimap(c,snapshot,map,x,y,w,h);
      },&hudHits);
    hud_focus_draw(&c,&hudHits,hudCursor);
    hudLayerKey=key;hudLayerHits=hudHits;hudLayerValid=true;
    }
    if(wideHud){wide::hudW=hudW;wide::hudH=hudH;}   // laid on the wide frame by emit
    // Scale with the top screen and keep the picture-in-picture touch screen clear.
    const bool doubled=top.w==512 && top.h==384;
    // x / 255 for x up to 255 * 255, exactly, without a divide.
    auto div255=[](unsigned x){return (x+1+(x>>8))>>8;};
    auto blend=[&](uint32_t p,uint32_t old){
      const unsigned a=p>>24?p>>24:255;
      if(a==255)return p&0xffffff;
      const unsigned r=div255(((p>>16)&255)*a+((old>>16)&255)*(255-a));
      const unsigned g=div255(((p>>8)&255)*a+((old>>8)&255)*(255-a));
      const unsigned b=div255((p&255)*a+(old&255)*(255-a));
      return uint32_t((r<<16)|(g<<8)|b);
    };
    if(wideHud){
    }else if(doubled && arrangement.touch.w==0){
      // The usual One Screen field: each HUD pixel is a 2x2 block. Walk the HUD's own 256x192 and skip clear pixels.
      for(int y=0;y<192;++y){
        const auto* row=hud_pixels+y*256;
        for(int x=0;x<256;++x){
          const uint32_t p=row[x];
          if(p==QUICK_CLEAR)continue;
          const unsigned at=(top.y+2*y)*w+top.x+2*x;
          const uint32_t q=blend(p,stage[at]);
          stage[at]=stage[at+1]=stage[at+w]=stage[at+w+1]=q;
        }
      }
    }else for(int y=0;y<top.h;++y){
      const auto* row=hud_pixels+(doubled?y/2:y*192/top.h)*256;
      for(int x=0;x<top.w;++x){
        const uint32_t p=row[doubled?x/2:x*256/top.w];
        if(p==QUICK_CLEAR || arrangement.touch.contains(top.x+x,top.y+y))continue;
        const unsigned at=(top.y+y)*w+top.x+x;
        stage[at]=blend(p,stage[at]);
      }
    }
  }
  if(perfOn)perfHud+=perf_now()-h0;
  // A notice goes over everything (a floating popup's dim, DS_BOTTOM's touch screen, the HUD), and marks the stage: the
  // quick menu composes it again before it opens over it.
  if(noticeFrames){draw_notice(stage);stageMarked=true;}
  presentedField=true;
  presentedArrangement=arrangement;
  // The field on the one screen, whole: the margins go on with its 3D.
  const bool field3D=game && arrangement.top.x==0 && arrangement.top.y==0 && arrangement.top.w==512 &&
    arrangement.top.h==384 && arrangement.touch.w==0 && !arrangement.floating && !arrangement.modal &&
    !battleActive && !wasInBattle && !scene.dark && scene.hud;
  emit(stage,field3D);
}
int16_t filtered_input(unsigned p,unsigned d,unsigned i,unsigned id) {
  if(flyCallJob)return 0;
  if(flyJob.active()){
    if(d==RETRO_DEVICE_JOYPAD)return id<16&&!!(flyJob.key&(1u<<id));
    if(d==RETRO_DEVICE_POINTER){
      if(id==RETRO_DEVICE_ID_POINTER_PRESSED)return flyJob.tapFrames>2;
      if(id==RETRO_DEVICE_ID_POINTER_X)return (flyJob.x*65535+255)/256-32767;
      if(id==RETRO_DEVICE_ID_POINTER_Y)return (flyJob.y*32768+191)/192;
    }
    return 0;
  }
  if(swallowMenuInput)return 0;
  if(d==RETRO_DEVICE_JOYPAD) {
    if(id<16 && (hudConsumed&(1u<<id)))return 0;
    if(id==RETRO_DEVICE_ID_JOYPAD_A&&heldQuickConfirm)return 0;
    if(id==RETRO_DEVICE_ID_JOYPAD_SELECT && select_pulse)return 1;
    if(companion || quickOpen || (buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT)))return 0;
    if(id<16 && (battleHeld&(1u<<id)))return 0;
    if(id==shell.companion_id() || int(id)==shell.screen_id())return 0;
    if(p==0 && id==RETRO_DEVICE_ID_JOYPAD_B && autoRunThisFrame)return !input(p,d,i,id);
  }
  // DSperate expects a pointer over stacked screens. Reproject the current
  // layout into that convention before its input::apply receives it.
  if(d==RETRO_DEVICE_POINTER) {
    bool stylus=buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT);
    if(stylus) {
      if(id==RETRO_DEVICE_ID_POINTER_PRESSED)return !!(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_A));
      if(id==RETRO_DEVICE_ID_POINTER_X)return (cursor_x*65535+255)/256-32767;
      if(id==RETRO_DEVICE_ID_POINTER_Y)return (cursor_y*32768+191)/192;
    }
    if(injectBattleTap) {
      if(id==RETRO_DEVICE_ID_POINTER_PRESSED)return battleTap.down;
      if(id==RETRO_DEVICE_ID_POINTER_X)return (battleTap.x*65535+255)/256-32767;
      if(id==RETRO_DEVICE_ID_POINTER_Y)return (battleTap.y*32768+191)/192;
    }
    int x=input(p,d,i,RETRO_DEVICE_ID_POINTER_X),y=input(p,d,i,RETRO_DEVICE_ID_POINTER_Y);
    // Over the wide frame (EXPAND), the stage is its middle 512 columns.
    auto pen=ds_pen(arrangement,int((x+32767)*int64_t(wide::outW)/65535)-2*wide::mx,
                    int((y+32767)*int64_t(wide::outH)/65535)-2*wide::myT);
    if(id==RETRO_DEVICE_ID_POINTER_PRESSED)return pen.valid?input(p,d,i,id):0;
    if(id==RETRO_DEVICE_ID_POINTER_X)return pen.valid?(pen.x*65535+255)/256-32767:0;
    if(id==RETRO_DEVICE_ID_POINTER_Y)return pen.valid?(pen.y*32768+191)/192:0;
  }
  return input(p,d,i,id);
}
void hud_open_target(int id) {
  if(id==HUD_PC){open_pc();return;}
  refresh();
  if(!snapshot.valid)return;
  shell.frame.overlay=PD_OVERLAY_NONE;
  shell.frame.moreOpened=false;
  shell.summary=shell.details=false;
  shell.placeRow=shell.mapAction=-1;
  if(id>=HUD_SLOT && id<HUD_SLOT+int(snapshot.party.size())){
    shell.frame.tab=PD_TAB_PARTY;
    shell.selected=id-HUD_SLOT;
    shell.summary=true;
    shell.page=shell.learnRow=0;
  }else if(id==HUD_MAP || id==HUD_MAP_BUTTON){
    shell.frame.tab=PD_TAB_MAP;
    shell.mapX=snapshot.mapX;
    shell.mapY=snapshot.mapY;
  }else if(id==HUD_WILD){
    shell.frame.tab=PD_TAB_GUIDE;
    shell.frame.guidePage=int(GuidePage::Here);
    shell.guideLoadedPage=-1;
    pd_ui_guide_reset(&shell.frame);
  }else if(id==HUD_COMPANION){
    refresh_battle_companion();
  }else if(id==HUD_BALLS || id==HUD_REPEL || id==HUD_BAG){
    shell.frame.tab=PD_TAB_BAG;
    shell.pocket=shell.scroll=0;
  }else return;
  companion=true;
}

void controls() {
  buttons=0;for(unsigned id=0;id<16;++id)if(input(0,RETRO_DEVICE_JOYPAD,0,id))buttons|=1u<<id;
  hudConsumed&=buttons;
  select_pulse=!(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT)) &&
    (old_buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT)) && !select_used;
  if(!(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT)))select_used=false;
  unsigned edge=buttons&~old_buttons;old_buttons=buttons;
  for(unsigned id=RETRO_DEVICE_ID_JOYPAD_UP;id<=RETRO_DEVICE_ID_JOYPAD_RIGHT;++id){
    held_frames[id]=(buttons&(1u<<id))?held_frames[id]+1:0;
    if((companion || quickOpen || hudCursor>=0) && held_frames[id]>15 && (held_frames[id]-15)%5==0)edge|=1u<<id;
  }
  auto hit=[&](unsigned id){return !!(edge&(1u<<id));};
  heldQuickConfirm&=!!(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_A));
  if(flyCallJob)return;
  if(flyJob.active()){
    if(hit(RETRO_DEVICE_ID_JOYPAD_B))flyJob={};
    return;
  }
  const int hudButton=shell.hud_id();
  if(hudCursor>=0){
    if(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT))select_used=true;
    if(hit(RETRO_DEVICE_ID_JOYPAD_B) || hit(RETRO_DEVICE_ID_JOYPAD_A) ||
       (hudButton>=0 && hit(hudButton))){
      const int target=hudCursor;
      hudCursor=-1;
      hudConsumed|=buttons;
      if(hit(RETRO_DEVICE_ID_JOYPAD_A) && !hit(RETRO_DEVICE_ID_JOYPAD_B))hud_open_target(target);
    }else{
      const int dx=hit(RETRO_DEVICE_ID_JOYPAD_RIGHT)-hit(RETRO_DEVICE_ID_JOYPAD_LEFT);
      const int dy=hit(RETRO_DEVICE_ID_JOYPAD_DOWN)-hit(RETRO_DEVICE_ID_JOYPAD_UP);
      if(dx || dy)hudCursor=hud_step(&hudHits,hudCursor,dy?0:dx,dy);
    }
    return;
  }
  if(!companion && !quickOpen && hudButton>=0 && hit(hudButton) && hudHits.n &&
     !(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT)) && field_free()){
    hudCursor=hud_first(&hudHits);
    hudConsumed|=1u<<hudButton;
    companionHold=0;
    return;
  }
  const bool selectDown=buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT);
  const bool stylusChord=selectDown && (buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_Y))
                         && opt("pokedaisy_ds_select_modifier")=="enabled";
  const bool companionDown=!stylusChord && (buttons&(1u<<shell.companion_id()));
  const bool companionTap=!companionDown&&companionHold>0;
  if(!companionDown)companionHold=0;
  else if(companionHold>=0)++companionHold;
  if(!companion && companionHold>0 && (selectDown || (!quickOpen&&companionHold>=24))){
    quickOpen=!quickOpen;repelPicker=flyPicker=false;select_used=selectDown;quickSelected=0;companionHold=-1;
    if(quickOpen){flyWhy=nullptr;refresh();}
    return;
  }
  if(quickOpen){
    if(hit(RETRO_DEVICE_ID_JOYPAD_B) || hit(shell.companion_id())){
      if(companionDown)companionHold=-1;
      if(repelPicker||flyPicker){repelPicker=flyPicker=false;return;}
      quickOpen=false;return;
    }
    quick_item shortcuts[shortcutCapacity]{};
    const int count=ds_shortcuts(shortcuts);
    int dx=hit(RETRO_DEVICE_ID_JOYPAD_RIGHT)-hit(RETRO_DEVICE_ID_JOYPAD_LEFT);
    int dy=hit(RETRO_DEVICE_ID_JOYPAD_DOWN)-hit(RETRO_DEVICE_ID_JOYPAD_UP);
    if(repelPicker){
      repelSelected=std::clamp(repelSelected+dy,0,repelCount-1);
      if(hit(RETRO_DEVICE_ID_JOYPAD_A)){use_repel();repelPicker=quickOpen=false;}
      return;
    }
    if(flyPicker){
      flySelected=std::clamp(flySelected+dy,0,int(flyDestinations.size())-1);
      if(hit(RETRO_DEVICE_ID_JOYPAD_A))confirm_fly();
      return;
    }
    quickSelected=std::min(quickSelected,count-1);
    if(dx || dy){
      quick_set_grid(shell.settings.quickGrid);
      quickSelected=quick_step(count,quickSelected,dx,dy,341,256);
    }
    if(hit(RETRO_DEVICE_ID_JOYPAD_A)){
      const auto& shortcut=shortcuts[quickSelected];
      if(shortcut.why)return;
      if(shortcut.action==QUICK_PC){open_pc();return;}
      if(shortcut.action==QUICK_FLY){open_fly();return;}
      if(shortcut.action==QUICK_REPEL){open_repel();return;}
      if(shortcut.action==QUICK_WILD){
        shell.settings.wildOff=!shell.settings.wildOff;
        const bool saved=shell.save();
        quick_notice(saved?(shell.settings.wildOff?"Wild encounters off.":"Wild encounters on."):shell.error.c_str());
        quickOpen=false;
        return;
      }
      if(shortcut.action==QUICK_HUD){
        hudCursor=hud_first(&hudHits);
        hudConsumed|=buttons;
      }
      else{
        refresh();refresh_battle_companion();companion=true;
        shell.mapX=snapshot.mapX;shell.mapY=snapshot.mapY;
      }
      quickOpen=false;
    }
    return;
  }
  if(companionTap) {
    companion=!companion;
    if(companion){refresh();refresh_battle_companion();shell.mapX=snapshot.mapX;shell.mapY=snapshot.mapY;}
  }
  if(!companion) {
    if(stylusChord && hit(RETRO_DEVICE_ID_JOYPAD_Y)){
      manualTouch=!manualTouch;screen_override=0;select_used=true;
    }else if(shell.screen_id()>=0 && hit(shell.screen_id())){
      if(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT)){
        manualTouch=!manualTouch;screen_override=0;select_used=true;
      }else{screen_override=(screen_override+1)%3;manualTouch=false;}
    }
    if(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_SELECT)) {
      if(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_UP))cursor_y-=shell.pen_step();
      if(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_DOWN))cursor_y+=shell.pen_step();
      if(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_LEFT))cursor_x-=shell.pen_step();
      if(buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_RIGHT))cursor_x+=shell.pen_step();
      cursor_x=std::clamp(cursor_x,0,255);cursor_y=std::clamp(cursor_y,0,191);
      // A SELECT tap is forwarded on release; SELECT plus pen controls is consumed.
      unsigned pen_keys=(1u<<RETRO_DEVICE_ID_JOYPAD_UP)|(1u<<RETRO_DEVICE_ID_JOYPAD_DOWN)|
        (1u<<RETRO_DEVICE_ID_JOYPAD_LEFT)|(1u<<RETRO_DEVICE_ID_JOYPAD_RIGHT)|(1u<<RETRO_DEVICE_ID_JOYPAD_A);
      if(buttons&pen_keys){
        select_used=true;
        if(arrangement.touch.w==0){
          if(shell.layout==DS_ONE||shell.layout==DS_PIP){manualTouch=true;screen_override=0;}
          else screen_override=1;
        }
      }
    }
    return;
  }
  unsigned previous_layout=shell.displayRevision;
  if(shell.control(edge) && (!shell.frame.moreOpened || !pd_ui_back(&shell.frame)))companion=false;
  if(shell.displayRevision!=previous_layout){screen_override=0;manualTouch=false;}

}
}
extern "C" {
RETRO_API void retro_set_environment(retro_environment_t cb){pd_ds::environment=cb;ds_set_environment(pd_ds::env);
  static const retro_input_descriptor d[]={
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP,"Up"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN,"Down"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT,"Left"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT,"Right"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A,"DS A"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B,"DS B"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_X,"DS X"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_Y,"DS Y"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START,"DS Start"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT,"DS Select / Stylus"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L,"DS L"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R,"DS R"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_L2,"Companion"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_R2,"Screen Auto Bottom Top"},
    {0,0,0,0,nullptr}};
  cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,const_cast<retro_input_descriptor*>(d));}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb){pd_ds::video=cb;ds_set_video_refresh(pd_ds::present);}
RETRO_API void retro_set_input_state(retro_input_state_t cb){pd_ds::input=cb;ds_set_input_state(pd_ds::filtered_input);}
RETRO_API void retro_get_system_info(retro_system_info* info){ds_get_system_info(info);info->library_name="PokeDaisy DS";info->library_version="0.4-one-screen";}
RETRO_API void retro_get_system_av_info(retro_system_av_info* info){
  ds_get_system_av_info(info);
  info->geometry={512,384,512+4*248,384+4*184,4.f/3.f};
}
RETRO_API bool retro_load_game(const retro_game_info* info){
  pd_ds::clear();pd_ds::width=512;pd_ds::height=384;
  pd_ds::perfOn=std::getenv("PD_DS_PERF")!=nullptr;
  pd_ds::ownSkip=false;pd_ds::reset_skip();
  if(const char* budget=std::getenv("PD_DS_FRAME_BUDGET")){
    const double ms=std::atof(budget);
    if(ms>0 && ms<=17)pd_ds::frameBudget=ms;
  }
  if(!ds_load_game(info))return false;
  // Read for the hash and the companion's art: a game too big for memory (or not a file) leaves the companion off
  // rather than throwing out of a C entry point.
  std::vector<uint8_t> rom;
  try{
    std::ifstream f(info->path?info->path:"",std::ios::binary|std::ios::ate);
    if(f){auto size=f.tellg();if(size>0){rom.resize(size_t(size));f.seekg(0);
      if(!f.read(reinterpret_cast<char*>(rom.data()),size))rom.clear();}}
  }catch(const std::exception&){rom.clear();rom.shrink_to_fit();
    log(RETRO_LOG_WARN,"PokeDaisy DS: couldn't read the ROM for the companion\n");}
  DsDexArt dexArt;
  if(rom.size()>=512) {
    // Preserve the existing exact-ROM guard: offsets and art are rev-specific.
    uint8_t hash[20];ds::crypto::sha1(rom.data(),rom.size(),hash);
    char dump[41];for(int i=0;i<20;++i)std::sprintf(dump+2*i,"%02x",hash[i]);
    const char* hex=ds_canonical_sha1(dump);
    pd_ds::supported=!std::strcmp(hex,pd_ds_data::sha1);
    pd_ds::companionGame=ds_companion_game(hex);
    pd_ds::soulSilver=!std::strcmp(hex,pd_ds_data::soulSilverSha1);
    pd_ds::battleStyle=ds_battle_style(hex);
    // Capsule: a French cart (or any other the checksums don't name) is matched by its header.
    bool frenchSilver=false;
    const DsGame byHeader=ds_select_cart(reinterpret_cast<const char*>(rom.data()+12),rom[30],&frenchSilver);
    if(pd_ds::companionGame==DsGame::Unsupported&&byHeader!=DsGame::Unsupported){
      pd_ds::companionGame=byHeader;pd_ds::soulSilver=frenchSilver;
      // The battle strip, by family. Platinum's copies the cart's own command art when it matches the US layout and
      // draws its plates otherwise: either way the commands stay on the one screen.
      pd_ds::battleStyle=byHeader==DsGame::Johto?ds_battle_style(frenchSilver?pd_ds_data::soulSilverSha1:pd_ds_data::heartGoldSha1):
        byHeader==DsGame::Black||byHeader==DsGame::White?ds_battle_style(pd_ds_data::blackSha1):
        byHeader==DsGame::Black2||byHeader==DsGame::White2?ds_battle_style(pd_ds_data::black2Sha1):
        &ds_platinum_battle_style;
      pd_ds::game=pk_nds_field_game(reinterpret_cast<const char*>(rom.data()+12),rom[30]);
    }
    ds_battle_set_base_pp({});
    if(pd_ds::battleStyle)
      pd_ds::game=pk_nds_field_game(reinterpret_cast<const char*>(rom.data()+12),rom[30]);
    if(pd_ds::companionGame!=DsGame::Unsupported)pd_ds::map=ds_map_art(rom,pd_ds::companionGame);
    if(pd_ds::companionGame!=DsGame::Unsupported){
      dexArt=ds_dex_art(rom,pd_ds::companionGame);
      pd_ds::icons=ds_party_icons(rom,pd_ds::companionGame);
      pd_ds::hudItemIcons=ds_hud_item_icons(rom,pd_ds::companionGame);
      ds_battle_set_base_pp(ds_move_base_pp(rom,pd_ds::companionGame));
    }
  }
  const auto old_layout=opt("pokedaisy_ds_layout");
  pd_ds::shell.init(dir(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY),
    old_layout=="top"?1:old_layout=="bottom"?2:old_layout=="stacked"?4:0,opt("pokedaisy_ds_hud")=="enabled");
  pd_ds::shell.set_game(pd_ds::companionGame,pd_ds::soulSilver);
  pd_ds::set_wild_rates();
  pd_ds::shell.dexArt=std::move(dexArt);
  log(RETRO_LOG_INFO,"PokeDaisy DS: companion %s\n",pd_ds::companionGame!=DsGame::Unsupported?"enabled":"unavailable for this ROM");
  pd_ds::wide::panel="?";pd_ds::wide::mx=pd_ds::wide::my=pd_ds::wide::myT=pd_ds::wide::myB=0;
  pd_ds::wide::read_panel(true);
  return true;
}
RETRO_API void retro_reset(){
  pd_ds::clear_battle();pd_ds::reset_skip();pd_ds::noticeFrames=0;
  ds_reset();pd_ds::clear_hud_focus();pd_ds::stageReady=false;pd_ds::clear_field_actions();pd_ds::flyJob={};pd_ds::flyPicker=pd_ds::heldQuickConfirm=false;pd_ds::companion=pd_ds::quickOpen=false;pd_ds::shell.picker=-1;pd_ds::shell.summary=false;pd_ds::shell.details=false;pd_ds::shell.dexEntry=0;pd_ds::shell.guideLoadedPage=-1;pd_ui_guide_reset(&pd_ds::shell.frame);pd_ds::shell.placeRow=pd_ds::shell.mapAction=-1;pd_ds::screen_override=0;pd_ds::snapshot={};pd_ds::repelFull=0;pd_ds::tick=0;pd_ds::scene.reset();pd_ds::arrangement={};pd_ds::manualTouch=pd_ds::fieldValid=pd_ds::fieldMenuFlow=false;pd_ds::companionHold=0;
}
RETRO_API bool retro_unserialize(const void* data,size_t n){
  pd_ds::clear_battle();
  // A DSperate state carries the cart's save memory: loading one would put the state's (maybe older) save over the
  // battery save the frontend then writes to its .srm. States never carry the save here, as the Linux host's
  // (load_state_keeping_save) and the GB / GBA core's don't: the save the console held is put back, in the same buffer
  // the frontend was given (retro_get_memory_data), still dirty if it was.
  std::vector<uint8_t> battery;bool dirty=false;uint8_t* frontendSave=nullptr;
  if(nds&&nds->cart){battery=nds->cart->sram();dirty=nds->cart->sram_dirty();frontendSave=nds->cart->sram().data();}
  bool ok=ds_unserialize(data,n);
  if(ok&&nds&&nds->cart&&!battery.empty()){
    auto& sram=nds->cart->sram();
    if(sram.size()==battery.size())std::memcpy(sram.data(),battery.data(),battery.size());else sram=battery;
    if(!dirty)nds->cart->clear_sram_dirty();
    if(sram.data()!=frontendSave)log(RETRO_LOG_WARN,"PokeDaisy DS: the save buffer moved on a state load\n");
  }
  if(ok){pd_ds::reset_skip();pd_ds::noticeFrames=0;pd_ds::shell.battle={};pd_ds::shell.update_battle();pd_ds::stageReady=false;pd_ds::restoreGrace=4;pd_ds::set_wild_rates();}
  if(ok){pd_ds::clear_hud_focus();pd_ds::clear_field_actions();pd_ds::flyJob={};pd_ds::flyPicker=pd_ds::heldQuickConfirm=false;pd_ds::companion=pd_ds::quickOpen=false;pd_ds::shell.picker=-1;pd_ds::shell.summary=false;pd_ds::shell.details=false;pd_ds::shell.dexEntry=0;pd_ds::shell.guideLoadedPage=-1;pd_ui_guide_reset(&pd_ds::shell.frame);pd_ds::shell.placeRow=pd_ds::shell.mapAction=-1;pd_ds::screen_override=0;pd_ds::snapshot={};pd_ds::repelFull=0;pd_ds::tick=0;pd_ds::scene.reset();pd_ds::arrangement={};pd_ds::manualTouch=pd_ds::fieldValid=pd_ds::fieldMenuFlow=false;pd_ds::companionHold=0;}
  return ok;
}
RETRO_API void retro_unload_game(){pd_ds::wide::stop();pd_ds::wide::mx=pd_ds::wide::my=pd_ds::wide::myT=pd_ds::wide::myB=0;pd_ds::wide::outW=512;pd_ds::wide::outH=384;ds_unload_game();pd_ds::clear();}
RETRO_API void retro_run(){
  if(!nds)return;
  input_poll_cb();
  if(pd_ds::tick%30==0)pd_ds::wide::read_panel(false);   // the phone turned
  const unsigned previous=pd_ds::old_buttons;
  const bool menuWasOpen=pd_ds::companion || pd_ds::quickOpen || pd_ds::hudCursor>=0;
  pd_ds::controls();
  const int field=pk_nds_field_state(pd_ds::game,pd_ds::read32,nullptr,0);
  if(pd_ds::shell.frame.battleWasOn && !(field&PK_NDS_FIELD_BATTLE)){
    pd_ds::shell.battle={};
    pd_ds::shell.update_battle();
  }
  pd_ds::update_battle(menuWasOpen?0:pd_ds::buttons&~previous,field);
  if(pd_ds::companion || pd_ds::quickOpen || pd_ds::hudCursor>=0){
    if(pd_ds::quickOpen)pd_ds::draw_quick();
    else if(pd_ds::companion)pd_ds::draw_companion();
    else pd_ds::present(nullptr,0,0,0);
    static const int16_t silence[548*2]={};audio_batch_cb(silence,547);
  }else {
    pd_ds::update_fly();
    pd_ds::swallowMenuInput=menuWasOpen;
    if(pd_ds::field_free(field))
      pk_nds_field_state(pd_ds::game,pd_ds::read32,pd_ds::write32,pd_ds::shell.settings.wildOff);
    constexpr unsigned directions=(1u<<RETRO_DEVICE_ID_JOYPAD_UP)|(1u<<RETRO_DEVICE_ID_JOYPAD_DOWN)|
      (1u<<RETRO_DEVICE_ID_JOYPAD_LEFT)|(1u<<RETRO_DEVICE_ID_JOYPAD_RIGHT);
    pd_ds::autoRunThisFrame=pd_ds::shell.autoRun && !pd_ds::manualTouch &&
      !pd_ds::pcJob && ds_auto_run(field,pd_ds::buttons&directions,pd_ds::buttons&(1u<<RETRO_DEVICE_ID_JOYPAD_A));
    if(pd_ds::ownSkip && fs_mode!=Skip::Off){
      // Last frame's work against the budget; see ownSkip.
      pd_ds::frameDebt=std::clamp(pd_ds::frameDebt+pd_ds::frameWork-pd_ds::frameBudget,0.0,pd_ds::maxDebt);
      pd_ds::skipRun=pd_ds::frameDebt>pd_ds::skipDebt && pd_ds::skipRun<2?pd_ds::skipRun+1:0;
      fs_behind=pd_ds::skipRun>0;
      pd_ds::frameWork=pd_ds::frameBudget;   // a frame that never presents neither adds nor pays
    }
    const double r0=pd_ds::perfOn||pd_ds::ownSkip?pd_ds::perf_now():0;
    pd_ds::runStart=r0;
    ds_run();
    pd_ds::runStart=0;
    if(pd_ds::perfOn){
      pd_ds::perfRun+=pd_ds::perf_now()-r0;
      if(nds->gpu.will_skip_frame())++pd_ds::perfSkipped;
      pd_ds::perf_report();
    }
    pd_ds::update_pc();
    pd_ds::update_fly_call();
    pd_ds::swallowMenuInput=false;
    ++pd_ds::tick;
  }
  // Once a frame, whichever way it was drawn (the quick menu, the companion, a frame DSperate skipped).
  if(pd_ds::noticeFrames)--pd_ds::noticeFrames;
}
}
