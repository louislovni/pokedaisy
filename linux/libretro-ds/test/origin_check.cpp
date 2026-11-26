// Origin HeartGold's companion against the player's own cart and the Android app's RAM fixture (OriginHeartGoldTest.kt).
// Not in ctest: it needs the ROM. ds-origin-check ROM MAINRAM [OUT_DIR]
#include "companion.h"
#include "map_art.h"
#include "party_view.h"
#include "bag_view.h"
#include "dex_view.h"
#include "map_view.h"
extern "C" {
#include "pd_ui.h"
#include "pd_tables.h"
}
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
static std::vector<uint8_t> load(const char* path){
  std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};
}
static void ppm(const std::string& path,const std::vector<uint32_t>& px,int w,int h){
  FILE* f=std::fopen(path.c_str(),"wb");if(!f)return;
  std::fprintf(f,"P6\n%d %d\n255\n",w,h);
  for(uint32_t p:px){std::fputc(p>>16&255,f);std::fputc(p>>8&255,f);std::fputc(p&255,f);}
  std::fclose(f);
}
int main(int argc,char** argv){
  assert(argc>=3);
  const auto rom=load(argv[1]),ram=load(argv[2]);
  const std::string out=argc>3?argv[3]:"";
  assert(ds_origin_cart(rom));
  static DsOrigin tables;
  assert(ds_origin_read(rom,tables));
  ds_select_cart("IPKJ",0,nullptr);
  ds_select_origin(&tables);
  const auto G=DsGame::Johto;
  for(int id:{1,700,887,1025,1028})std::printf("species %d %s types %d/%d\n",id,ds_species(id,G),ds_species_type(id,0,G),ds_species_type(id,1,G));
  assert(!std::strcmp(ds_species(1,G),"Bulbasaur")&&!std::strcmp(ds_species(887,G),"Dragapult")&&!std::strcmp(ds_species(1025,G),"Pecharunt"));
  assert(ds_species_type(700,0,G)==9&&ds_species_type(887,0,G)==16&&ds_species_type(887,1,G)==7);
  assert(!std::strcmp(ds_move(33,G),"Tackle")&&ds_move_type(52,G)==10&&ds_move_power(52,G)==40);
  assert(!std::strcmp(ds_item(780,G),"Rash Mint"));
  assert(tables.machines[0]==264&&tables.machines[93]==19&&tables.machines[100]==473&&tables.machines[137]==815);
  assert(tables.form_species(6,2)==1028&&tables.form_species(6,0)==6);
  assert(tables.wildRates.size()>300);
  std::printf("move 920 %s, egg icon %d\n",ds_move(920,G),ds_egg_icon(G));
  const auto s=ds_read_companion(ram.data(),ram.size(),G);
  assert(s.valid);
  std::printf("trainer %s money %d map %d (%d,%d) badges %d\n",s.trainer.c_str(),s.money,s.map,s.mapX,s.mapY,s.badges);
  for(const auto& m:s.party)std::printf("  %s Lv%d %d/%d item %d moves %s %s\n",ds_species(m.species,G),m.level,m.hp,m.maxHp,m.heldItem,
    ds_move(m.moves[0],G),ds_move(m.moves[1],G));
  const int species[]={155,700,25,1028,445,887},levels[]={12,20,9,30,40,35};
  assert(s.party.size()==6);
  for(int i=0;i<6;++i)assert(s.party[i].species==species[i]&&s.party[i].level==levels[i]);
  assert(s.party[1].heldItem==234&&s.money==3000&&s.map==49);
  int bagSeen=0;for(const auto& i:s.bag){std::printf("  bag %d x%d pocket %d %s\n",i.id,i.count,i.pocket,ds_item(i.id,G));++bagSeen;}
  assert(bagSeen==11);
  assert(s.dexValid&&s.seenCount==5&&s.caughtCount==5&&s.dexCaught[887]&&s.dexSeen.size()==1026);
  const auto icons=ds_party_icons(rom,G);
  int drawn=0;for(const auto& i:icons)drawn+=!i.empty();
  std::printf("icons %d of %zu\n",drawn,icons.size());
  assert(icons.size()==1442&&!icons[887].empty()&&!icons[1028].empty()&&!icons[1441].empty());
  const auto art=ds_dex_art(rom,G);
  assert(art.details.size()==1026&&art.regional.empty());
  std::printf("dex 887: %s / %s, %u dm %u hg growth %d\n",art.details[887].category.c_str(),art.details[887].description.c_str(),
    art.details[887].heightDm,art.details[887].weightHg,art.details[887].growth);
  const auto page=ds_dex_page(art,G,887);
  std::printf("page: side %d, %zu level moves, %zu machines, %zu evolutions\n",page.side,page.moves.size(),page.machines.size(),
    ds_dex_evolutions(art,887).size());
  assert(page.side==80&&!page.moves.empty()&&!page.machines.empty());
  const auto pp=ds_move_base_pp(rom,G);assert(pp.size()>900&&pp[33]==35);
  const auto map=ds_map_art(rom,G);
  if(!out.empty()){
    pd_type_fairy9=1;
    std::vector<uint32_t> px(256*192*4);pd_canvas c;
    auto shot=[&](const char* name,auto draw){
      std::fill(px.begin(),px.end(),0);pd_canvas_init(&c,341,256,px.data());draw();ppm(out+"/"+name+".ppm",px,341,256);
    };
    shot("party",[&]{ds_party_view(&c,s,icons,0,false,0);});
    shot("summary",[&]{ds_party_view(&c,s,icons,1,true,1,&art);});
    shot("bag",[&]{ds_bag_view(&c,s,0,0,false);});
    shot("dex",[&]{ds_dex_view(&c,s,0,0,-1,&icons,nullptr,nullptr);});
    shot("entry",[&]{ds_dex_entry(&c,s,887,887,icons,&art.details[887],page,ds_dex_evolutions(art,887),{},0,5,DsDexSection::Info,0);});
    shot("moves",[&]{ds_dex_entry(&c,s,887,887,icons,&art.details[887],page,ds_dex_evolutions(art,887),{},0,5,DsDexSection::Moves,0);});
    auto m=map;ds_map_update(m,s);
    shot("map",[&]{ds_map_view(&c,s,m,s.mapX,s.mapY);});
  }
  std::puts("PASS Origin HeartGold companion");
}
