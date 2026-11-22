/* Capsule's look and weather (POCKET_PURE builds, the host menu's LOOK and WEATHER AND LIGHTS): what the iPhone draws
 * with its Metal shaders, here with SDL's renderer alone - so any PC (Direct3D, OpenGL or software) draws it.
 *  - LOOK: ORIGINAL; SMOOTH HD (the frame scaled up by EPX, then filtered); CRT (scanlines, darker corners); REMASTER
 *    (richer colours and contrast, a light vignette). LCD FILTER stays the host's own.
 *  - WEATHER AND LIGHTS, over the free field outdoors: the weather the game says (rain, storm, snow, fog, ash, sand, sun,
 *    under water) drawn finer than the game's own; the day's colours by the PC's clock (unless the game's DAY AND NIGHT
 *    tints it); at night darker edges, a lantern's light round the player, the houses' windows lit and a warm light
 *    before each door (the core's retro_pd_lights), fireflies on a clear night; a warm halo in a dark cave.
 * Everything over the frame is a few small textures made once (a soft disc, a dark vignette, tiling noise) drawn with
 * blend modes, and lines for the rain: next to nothing a frame. Included by main.c only. */
#ifndef CAPSULE_FX_H
#define CAPSULE_FX_H

#include <math.h>
#include <time.h>

enum { FX_ORIGINAL, FX_SMOOTH, FX_CRT, FX_REMASTER, FX_LOOKS };
static const char *const fxLookRows[FX_LOOKS] = {"LOOK  ORIGINAL", "LOOK  SMOOTH HD", "LOOK  CRT", "LOOK  REMASTER"};

#define FX_DROPS 260
#define FX_FLAKES 140
#define FX_FLIES 26
#define FX_LIGHTS 32

static struct {
  int (*scene)(int *, int);       /* retro_pd_scene: weather, map type, battle, field, flash level, tint r g b */
  int (*lights)(float *, int);    /* retro_pd_lights: x y w h kind, in the frame's 0..1 */
  /* SMOOTH HD / REMASTER: the frame redone on the CPU (it is small: the game's own pixels) */
  uint32_t *buf;
  size_t bufCap;
  SDL_Texture *tex;
  int texW, texH, outW, outH;
  bool texOn;                      /* this frame is drawn from tex, not the host's frameTexture */
  /* the textures drawn over the frame */
  SDL_Texture *disc, *dark, *noise, *clouds, *scan, *edge;
  int scanH;
  /* the weather, eased in and out as on the iPhone */
  int shown;
  float strength, flash;
  uint32_t nextFlash, lastTicks;
  float gr, gg, gb;                /* the day's grade now (1 = none) */
  float drops[FX_DROPS][3];        /* x, y (0..1), speed */
  float flakes[FX_FLAKES][4];      /* x, y, speed, phase */
  bool seeded;
} fx = {.shown = -1, .gr = 1, .gg = 1, .gb = 1};

static float fx_rand(void) { return (float)rand() / (float)RAND_MAX; }
static float fx_smooth(float a, float b, float x) {
  float t = (x - a) / (b - a);
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  return t * t * (3 - 2 * t);
}
static float fx_hash(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

static void fx_core(int (*scene)(int *, int), int (*lights)(float *, int)) {
  fx.scene = scene;
  fx.lights = lights;
  fx.shown = -1;
  fx.strength = 0;
}

/* ---- the textures, made once per renderer ---- */
static SDL_Texture *fx_texture(SDL_Renderer *r, int w, int h, const uint32_t *px, SDL_BlendMode blend) {
  SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
  if (!t)
    return NULL;
  SDL_UpdateTexture(t, NULL, px, w * 4);
  SDL_SetTextureBlendMode(t, blend);
  SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
  return t;
}
/* Tiling value noise, a few octaves (0..1), on a 128 grid. */
static float fx_vnoise(float x, float y, int period) {
  int xi = (int)floorf(x), yi = (int)floorf(y);
  float fx_ = x - xi, fy = y - yi;
  float u = fx_ * fx_ * (3 - 2 * fx_), v = fy * fy * (3 - 2 * fy);
  int x0 = ((xi % period) + period) % period, y0 = ((yi % period) + period) % period;
  int x1 = (x0 + 1) % period, y1 = (y0 + 1) % period;
  float a = fx_hash(x0, y0), b = fx_hash(x1, y0), c = fx_hash(x0, y1), d = fx_hash(x1, y1);
  return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}
static void fx_make(SDL_Renderer *r) {
  if (fx.disc)
    return;
  enum { N = 128 };
  static uint32_t px[N * N];
  // A soft white disc (alpha falls off to the edge): glows, pools, flakes, fireflies.
  for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
      float dx = (x + 0.5f) / N * 2 - 1, dy = (y + 0.5f) / N * 2 - 1;
      float a = fx_smooth(1.0f, 0.0f, sqrtf(dx * dx + dy * dy));
      px[y * N + x] = (uint32_t)(a * a * 255) << 24 | 0xFFFFFF;
    }
  fx.disc = fx_texture(r, N, N, px, SDL_BLENDMODE_ADD);
  // The night's darker edges: black, more see-through in the middle.
  for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
      float dx = (x + 0.5f) / N - 0.5f, dy = (y + 0.5f) / N - 0.5f;
      float a = fx_smooth(0.2f, 0.75f, sqrtf(dx * dx * 1.6f + dy * dy)) * 0.5f;
      px[y * N + x] = (uint32_t)(a * 255) << 24;
    }
  fx.dark = fx_texture(r, N, N, px, SDL_BLENDMODE_BLEND);
  // CRT / REMASTER's corners: a lighter vignette.
  for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
      float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
      float k = u * (1 - u) * v * (1 - v) * 16;
      float a = 1 - powf(k < 0 ? 0 : k > 1 ? 1 : k, 0.3f);
      px[y * N + x] = (uint32_t)(a * 0.32f * 255) << 24;
    }
  fx.edge = fx_texture(r, N, N, px, SDL_BLENDMODE_BLEND);
  // Fog, sand, ash, caustics: tiling noise (white, its alpha the noise); clouds: their shadows' shapes.
  for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
      float n = 0, amp = 0.5f;
      for (int o = 0, per = 8; o < 4; o++, per *= 2, amp *= 0.5f)
        n += amp * fx_vnoise(x * per / (float)N, y * per / (float)N, per);
      n /= 0.9375f;
      px[y * N + x] = (uint32_t)(n * 255) << 24 | 0xFFFFFF;
    }
  fx.noise = fx_texture(r, N, N, px, SDL_BLENDMODE_BLEND);
  for (int i = 0; i < N * N; i++) {
    float n = (float)(px[i] >> 24) / 255;
    px[i] = (uint32_t)(fx_smooth(0.5f, 0.72f, n) * 255) << 24;
  }
  fx.clouds = fx_texture(r, N, N, px, SDL_BLENDMODE_BLEND);
}

/* CRT's scanlines for a frame [h] lines high: each line's lower half darker (stretched over the frame, nearest). */
static void fx_scanlines(SDL_Renderer *r, int h) {
  if (fx.scan && fx.scanH == h)
    return;
  if (fx.scan)
    SDL_DestroyTexture(fx.scan);
  fx.scan = NULL;
  uint32_t *px = malloc((size_t)h * 2 * 4);
  if (!px)
    return;
  for (int y = 0; y < h * 2; y++)
    px[y] = y & 1 ? 0xFFB4B4B4u : 0xFFFFFFFFu;
  fx.scan = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 1, h * 2);
  if (fx.scan) {
    SDL_UpdateTexture(fx.scan, NULL, px, 4);
    SDL_SetTextureBlendMode(fx.scan, SDL_BLENDMODE_MOD);
    SDL_SetTextureScaleMode(fx.scan, SDL_ScaleModeNearest);
  }
  fx.scanH = h;
  free(px);
}

/* ---- the frame: SMOOTH HD and REMASTER redo it on the CPU (XRGB8888 frames) ---- */
static inline uint32_t fx_px(const uint8_t *data, size_t pitch, unsigned w, unsigned h, int x, int y) {
  x = x < 0 ? 0 : x >= (int)w ? (int)w - 1 : x;
  y = y < 0 ? 0 : y >= (int)h ? (int)h - 1 : y;
  return ((const uint32_t *)(data + (size_t)y * pitch))[x] & 0xFFFFFF;
}
static inline uint32_t fx_remaster(uint32_t p) {
  float r = (float)((p >> 16) & 255), g = (float)((p >> 8) & 255), b = (float)(p & 255);
  float l = r * 0.299f + g * 0.587f + b * 0.114f;
  r = (l + (r - l) * 1.18f - 127.5f) * 1.07f + 127.5f;
  g = (l + (g - l) * 1.18f - 127.5f) * 1.07f + 127.5f;
  b = (l + (b - l) * 1.18f - 127.5f) * 1.07f + 127.5f;
  int ri = r < 0 ? 0 : r > 255 ? 255 : (int)r, gi = g < 0 ? 0 : g > 255 ? 255 : (int)g, bi = b < 0 ? 0 : b > 255 ? 255 : (int)b;
  return 0xFF000000u | (uint32_t)ri << 16 | (uint32_t)gi << 8 | (uint32_t)bi;
}
/* The frame as it came; true if it is drawn from fx.tex this frame. */
static void fx_frame(SDL_Renderer *r, const void *data, unsigned w, unsigned h, size_t pitch, enum retro_pixel_format fmt,
                     int look) {
  fx.texOn = false;
  if ((look != FX_SMOOTH && look != FX_REMASTER) || fmt != RETRO_PIXEL_FORMAT_XRGB8888 || !w || !h)
    return;
  int s = look == FX_SMOOTH ? 2 : 1;
  int ow = (int)w * s, oh = (int)h * s;
  size_t need = (size_t)ow * oh;
  if (need > fx.bufCap) {
    uint32_t *b = realloc(fx.buf, need * 4);
    if (!b)
      return;
    fx.buf = b;
    fx.bufCap = need;
  }
  const uint8_t *d = data;
  if (s == 2) {
    // EPX (Scale2x): each pixel into four, its corners following the edges round it.
    for (int y = 0; y < (int)h; y++)
      for (int x = 0; x < (int)w; x++) {
        uint32_t P = fx_px(d, pitch, w, h, x, y), A = fx_px(d, pitch, w, h, x, y - 1), B = fx_px(d, pitch, w, h, x + 1, y);
        uint32_t C = fx_px(d, pitch, w, h, x - 1, y), D = fx_px(d, pitch, w, h, x, y + 1);
        uint32_t e0 = P, e1 = P, e2 = P, e3 = P;
        if (C == A && C != D && A != B)
          e0 = A;
        if (A == B && A != C && B != D)
          e1 = B;
        if (D == C && D != B && C != A)
          e2 = C;
        if (B == D && B != A && D != C)
          e3 = D;
        uint32_t *o = fx.buf + (size_t)(2 * y) * ow + 2 * x;
        o[0] = 0xFF000000u | e0;
        o[1] = 0xFF000000u | e1;
        o[ow] = 0xFF000000u | e2;
        o[ow + 1] = 0xFF000000u | e3;
      }
  } else {
    for (int y = 0; y < (int)h; y++) {
      const uint32_t *row = (const uint32_t *)(d + (size_t)y * pitch);
      for (int x = 0; x < (int)w; x++)
        fx.buf[(size_t)y * ow + x] = fx_remaster(row[x]);
    }
  }
  if (!fx.tex || fx.texW < ow || fx.texH < oh) {
    if (fx.tex)
      SDL_DestroyTexture(fx.tex);
    fx.texW = ow > fx.texW ? ow : fx.texW;
    fx.texH = oh > fx.texH ? oh : fx.texH;
    fx.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, fx.texW, fx.texH);
    if (!fx.tex)
      return;
  }
  SDL_Rect rect = {0, 0, ow, oh};
  SDL_UpdateTexture(fx.tex, &rect, fx.buf, ow * 4);
  // SMOOTH HD: filtered once more as it is stretched (soft, not blurred: EPX already made the edges).
  SDL_SetTextureScaleMode(fx.tex, look == FX_SMOOTH ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
  fx.outW = ow;
  fx.outH = oh;
  fx.texOn = true;
}

/* ---- the scene ---- */
struct fx_scene {
  int weather, mapType;
  bool battle, field;
  int cave;
  int tint[3];
};
static bool fx_read(struct fx_scene *s) {
  int v[8];
  if (!fx.scene || fx.scene(v, 8) < 8)
    return false;
  s->weather = v[0];
  s->mapType = v[1];
  s->battle = v[2] != 0;
  s->field = v[3] != 0;
  s->cave = v[4] < 0 ? 0 : v[4];
  s->tint[0] = v[5];
  s->tint[1] = v[6];
  s->tint[2] = v[7];
  return true;
}
static bool fx_outdoors(const struct fx_scene *s) {
  return s->mapType < 0 || s->mapType == 1 || s->mapType == 2 || s->mapType == 3 || s->mapType == 6;
}
static bool fx_drawn(int w) { return w >= 0 && w <= 14; }

/* The day's colours by the clock (as the iPhone, PokeDaisy's DayNight.kt): deep blue at night, pink at dawn, orange at
 * dusk, none by day. */
static void fx_day_grade(float out[3]) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  float hour = t ? (float)t->tm_hour + (float)t->tm_min / 60 : 12;
  static const float keys[8] = {0, 4.5f, 6, 7.5f, 16.5f, 18, 19.5f, 24};
  static const float cols[8][3] = {{0.5f, 0.56f, 0.86f}, {0.5f, 0.56f, 0.86f}, {0.96f, 0.86f, 0.86f}, {1, 1, 1},
                                   {1, 1, 1},           {1.0f, 0.8f, 0.66f},  {0.5f, 0.56f, 0.86f}, {0.5f, 0.56f, 0.86f}};
  for (int i = 0; i < 7; i++)
    if (hour >= keys[i] && hour <= keys[i + 1]) {
      float t2 = (hour - keys[i]) / (keys[i + 1] - keys[i]);
      for (int c = 0; c < 3; c++)
        out[c] = cols[i][c] + (cols[i + 1][c] - cols[i][c]) * t2;
      return;
    }
  out[0] = out[1] = out[2] = 1;
}

/* The frame, coloured by the time of day (its texture's colour mod): before it is drawn. */
static struct fx_scene fxScene;
static bool fxHave;
static void fx_before_frame(SDL_Texture *frame, bool on) {
  fxHave = on && fx_read(&fxScene);
  bool tinted = fxHave && (fxScene.tint[0] != 256 || fxScene.tint[1] != 256 || fxScene.tint[2] != 256);
  float target[3] = {1, 1, 1};
  if (fxHave && !tinted && fx_outdoors(&fxScene) && fxScene.field && !fxScene.battle)
    fx_day_grade(target);
  fx.gr += (target[0] - fx.gr) * 0.03f;
  fx.gg += (target[1] - fx.gg) * 0.03f;
  fx.gb += (target[2] - fx.gb) * 0.03f;
  SDL_Texture *t = fx.texOn ? fx.tex : frame;
  if (t)
    SDL_SetTextureColorMod(t, (Uint8)(fx.gr * 255), (Uint8)(fx.gg * 255), (Uint8)(fx.gb * 255));
}

/* Draws the frame (from fx.tex when the look redid it); false: the host draws its own. */
static bool fx_draw_frame(SDL_Renderer *r, SDL_Rect dst) {
  if (!fx.texOn || !fx.tex)
    return false;
  SDL_Rect src = {0, 0, fx.outW, fx.outH};
  SDL_RenderCopy(r, fx.tex, &src, &dst);
  return true;
}

static void fx_disc(SDL_Renderer *r, float cx, float cy, float rx, float ry, int cr, int cg, int cb, float a) {
  if (a <= 0.003f || !fx.disc)
    return;
  SDL_Rect d = {(int)(cx - rx), (int)(cy - ry), (int)(2 * rx), (int)(2 * ry)};
  SDL_SetTextureColorMod(fx.disc, (Uint8)cr, (Uint8)cg, (Uint8)cb);
  SDL_SetTextureAlphaMod(fx.disc, (Uint8)(a > 1 ? 255 : a * 255));
  SDL_RenderCopy(r, fx.disc, NULL, &d);
}
/* The tiling noise over [f], [tiles] across, scrolled by (ox, oy) tiles. */
static void fx_layer(SDL_Renderer *r, SDL_Texture *t, SDL_Rect f, float tiles, float ox, float oy, int cr, int cg, int cb,
                     float a) {
  if (!t || a <= 0.003f)
    return;
  SDL_SetTextureColorMod(t, (Uint8)cr, (Uint8)cg, (Uint8)cb);
  SDL_SetTextureAlphaMod(t, (Uint8)(a > 1 ? 255 : a * 255));
  float tw = (float)f.w / tiles;
  if (tw < 8)
    tw = 8;
  ox -= floorf(ox);
  oy -= floorf(oy);
  for (float y = f.y - oy * tw; y < f.y + f.h; y += tw)
    for (float x = f.x - ox * tw; x < f.x + f.w; x += tw) {
      SDL_Rect d = {(int)floorf(x), (int)floorf(y), (int)ceilf(tw) + 1, (int)ceilf(tw) + 1};
      SDL_RenderCopy(r, t, NULL, &d);
    }
}

static void fx_seed(void) {
  if (fx.seeded)
    return;
  for (int i = 0; i < FX_DROPS; i++) {
    fx.drops[i][0] = fx_rand();
    fx.drops[i][1] = fx_rand();
    fx.drops[i][2] = 0.8f + fx_rand() * 0.6f;
  }
  for (int i = 0; i < FX_FLAKES; i++) {
    fx.flakes[i][0] = fx_rand();
    fx.flakes[i][1] = fx_rand();
    fx.flakes[i][2] = 0.5f + fx_rand();
    fx.flakes[i][3] = fx_rand() * 6.28f;
  }
  fx.seeded = true;
}

/* Over the drawn frame [f] (its [fw] x [fh] pixels): the weather and the night. [look]: CRT's and REMASTER's own layers. */
static void fx_overlay(SDL_Renderer *r, SDL_Rect f, int fw, int fh, bool on, int look) {
  fx_make(r);
  if (f.w <= 0 || f.h <= 0)
    return;
  uint32_t ticks = SDL_GetTicks();
  float dt = fx.lastTicks ? (float)(ticks - fx.lastTicks) / 1000.0f : 0.016f;
  dt = dt > 0.1f ? 0.1f : dt;
  fx.lastTicks = ticks;
  float time = (float)ticks / 1000.0f;
  SDL_RenderSetClipRect(r, &f);

  if (look == FX_CRT && fh > 0) {
    fx_scanlines(r, fh);
    if (fx.scan)
      SDL_RenderCopy(r, fx.scan, NULL, &f);
  }
  if ((look == FX_CRT || look == FX_REMASTER) && fx.edge) {
    SDL_SetTextureAlphaMod(fx.edge, look == FX_CRT ? 255 : 150);
    SDL_RenderCopy(r, fx.edge, NULL, &f);
  }

  struct fx_scene *s = &fxScene;
  bool live = on && fxHave && s->field && !s->battle;
  int wanted = live && fx_outdoors(s) && fx_drawn(s->weather) ? s->weather : -1;
  float ease = dt * 60;
  if (wanted != fx.shown) {
    fx.strength -= 0.05f * ease;
    if (fx.strength <= 0) {
      fx.strength = 0;
      fx.shown = wanted;
    }
  } else if (wanted >= 0) {
    fx.strength += 0.04f * ease;
    if (fx.strength > 1)
      fx.strength = 1;
  }
  float k = fx.strength;
  int w = fx.shown;
  float px = (float)f.h / 160.0f; // a GBA pixel, on screen
  fx_seed();

  if (w >= 0 && k > 0) {
    if (w == 3 || w == 5 || w == 13) { // rain; storms heavier, with lightning
      bool heavy = w != 3;
      int n = heavy ? FX_DROPS : FX_DROPS * 6 / 10;
      SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(r, 205, 215, 255, (Uint8)(k * (heavy ? 120 : 95)));
      float len = 0.05f, slant = 0.28f;
      for (int i = 0; i < n; i++) {
        float *d = fx.drops[i];
        d[1] += dt * d[2] * (heavy ? 1.7f : 1.3f);
        d[0] += dt * d[2] * slant * (heavy ? 1.7f : 1.3f) * f.h / f.w;
        if (d[1] > 1.05f) {
          d[1] -= 1.1f;
          d[0] = fx_rand();
        }
        if (d[0] > 1)
          d[0] -= 1;
        int x0 = f.x + (int)(d[0] * f.w), y0 = f.y + (int)(d[1] * f.h);
        int x1 = x0 - (int)(slant * len * f.h * d[2]), y1 = y0 - (int)(len * f.h * d[2]);
        SDL_RenderDrawLine(r, x0, y0, x1, y1);
        if (px >= 3)
          SDL_RenderDrawLine(r, x0 + 1, y0, x1 + 1, y1);
        if (d[1] > 0.3f && fx_hash(i, (int)(time * 4)) > 0.985f) // a splash now and then
          fx_disc(r, (float)x0, (float)y0, 2.5f * px, 1.0f * px, 120, 130, 160, k * 0.5f);
      }
      fx_layer(r, fx.noise, f, 2, time * 0.02f, 0, 40, 50, 70, k * (heavy ? 0.22f : 0.14f)); // a grey wet sky
      if (heavy && k > 0.5f && ticks >= fx.nextFlash) {
        fx.flash = 1;
        fx.nextFlash = ticks + 4000 + (uint32_t)(fx_rand() * 7000);
      }
    } else if (w == 4) { // snow
      for (int i = 0; i < FX_FLAKES; i++) {
        float *d = fx.flakes[i];
        d[1] += dt * 0.12f * d[2];
        d[3] += dt * 1.3f;
        if (d[1] > 1.03f) {
          d[1] -= 1.06f;
          d[0] = fx_rand();
        }
        float x = d[0] + sinf(d[3]) * 0.015f;
        x -= floorf(x);
        float rad = (0.8f + d[2]) * px;
        fx_disc(r, f.x + x * f.w, f.y + d[1] * f.h, rad, rad, 255, 255, 255, k * 0.75f);
      }
    } else if (w == 6 || w == 9) { // fog
      fx_layer(r, fx.noise, f, 1.3f, time * 0.012f, time * 0.004f, 225, 230, 240, k * 0.42f);
      fx_layer(r, fx.noise, f, 2.2f, -time * 0.02f, 0.3f, 225, 230, 240, k * 0.28f);
    } else if (w == 7) { // ash: a grey haze, flakes drifting down
      fx_layer(r, fx.noise, f, 1.6f, time * 0.01f, 0, 120, 118, 115, k * 0.25f);
      for (int i = 0; i < FX_FLAKES / 2; i++) {
        float *d = fx.flakes[i];
        d[1] += dt * 0.06f * d[2];
        d[3] += dt * 0.8f;
        if (d[1] > 1.03f) {
          d[1] -= 1.06f;
          d[0] = fx_rand();
        }
        float x = d[0] + sinf(d[3]) * 0.02f;
        x -= floorf(x);
        SDL_SetTextureBlendMode(fx.disc, SDL_BLENDMODE_BLEND);
        fx_disc(r, f.x + x * f.w, f.y + d[1] * f.h, 1.2f * px, 1.2f * px, 90, 88, 86, k * 0.8f);
        SDL_SetTextureBlendMode(fx.disc, SDL_BLENDMODE_ADD);
      }
    } else if (w == 8) { // sandstorm: tan, blowing fast
      SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(r, 200, 160, 100, (Uint8)(k * 60));
      SDL_RenderFillRect(r, &f);
      fx_layer(r, fx.noise, f, 1.5f, time * 0.35f, time * 0.03f, 215, 175, 115, k * 0.45f);
      fx_layer(r, fx.noise, f, 3.0f, time * 0.6f, 0.5f, 225, 185, 125, k * 0.3f);
    } else if (w == 12) { // drought: the sun's warm rays from above
      fx_disc(r, f.x + f.w * 0.15f, (float)f.y, f.w * 0.7f, f.h * 0.9f, 255, 200, 120, k * 0.28f);
    } else if (w == 10 || w == 14) { // under water: blue, light rippling
      SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(r, 20, 70, 160, (Uint8)(k * 70));
      SDL_RenderFillRect(r, &f);
      SDL_SetTextureBlendMode(fx.noise, SDL_BLENDMODE_ADD);
      fx_layer(r, fx.noise, f, 2.5f, time * 0.03f, time * 0.02f, 90, 140, 170, k * 0.22f);
      SDL_SetTextureBlendMode(fx.noise, SDL_BLENDMODE_BLEND);
    } else if (w == 11) { // shade
      SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(r, 10, 15, 30, (Uint8)(k * 60));
      SDL_RenderFillRect(r, &f);
    } else if (w <= 2) { // sun: clouds' shadows drifting over the ground
      fx_layer(r, fx.clouds, f, 0.9f, time * 0.008f, time * 0.003f, 0, 0, 20, k * 0.22f);
    }
  }
  fx.flash -= 0.07f * ease;
  if (fx.flash < 0)
    fx.flash = 0;
  if (fx.flash > 0 && k > 0) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    SDL_SetRenderDrawColor(r, 200, 210, 255, (Uint8)(fx.flash * k * 150));
    SDL_RenderFillRect(r, &f);
  }

  // The night: how dark the grade (the clock's, or DAY AND NIGHT's tint) has made it.
  float night = 0;
  if (live && fx_outdoors(s)) {
    float own = (1 - (fx.gr + fx.gg) / 2) / 0.45f;
    float blue = (float)(s->tint[0] + s->tint[1]) / 512.0f;
    float core = (1 - blue) / 0.45f;
    night = own > core ? own : core;
    night = night < 0 ? 0 : night > 1 ? 1 : night;
  }
  if (night > 0.01f) {
    float cx = f.x + f.w * 0.5f, cy = f.y + f.h * 0.5f;
    // Darker away from the player, a warm pool round them (a lantern).
    if (fx.dark) {
      SDL_SetTextureAlphaMod(fx.dark, (Uint8)(night * 255));
      SDL_RenderCopy(r, fx.dark, NULL, &f);
    }
    fx_disc(r, cx, cy, f.h * 0.42f, f.h * 0.42f, 255, 170, 80, night * 0.22f);
    // The real lights: each window lit from inside, a warm pool before each door.
    float lights[FX_LIGHTS * 5];
    int n = fx.lights ? fx.lights(lights, FX_LIGHTS) : 0;
    for (int i = 0; i < n; i++) {
      float *l = lights + 5 * i;
      float lx = f.x + l[0] * f.w, ly = f.y + l[1] * f.h, lw = l[2] * f.w, lh = l[3] * f.h;
      float flicker = 0.94f + 0.06f * sinf(time * 2.7f + i * 1.7f) * sinf(time * 1.3f + i);
      if (l[4] > 0.5f) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 255, 220, 140, (Uint8)(night * 0.88f * 255 * flicker));
        SDL_Rect g = {(int)lx, (int)ly, (int)(lw + 0.5f), (int)(lh + 0.5f)};
        SDL_RenderFillRect(r, &g);
        fx_disc(r, lx + lw / 2, ly + lh / 2, lw / 2 + 6 * px, lh / 2 + 6 * px, 255, 205, 115, night * 0.4f * flicker);
      } else {
        fx_disc(r, lx + lw / 2, ly + lh * 1.05f, lw * 1.3f, lw * 0.75f, 230, 150, 64, night * 0.32f * flicker);
        fx_disc(r, lx + lw / 2, ly + lh / 2, lw * 0.6f, lh * 0.6f, 230, 150, 64, night * 0.18f);
      }
    }
    // Fireflies on a clear night.
    if (w <= 2 || w < 0)
      for (int i = 0; i < FX_FLIES; i++) {
        float bx = fx_hash(i, 1), by = fx_hash(i, 2);
        float x = bx + sinf(time * 0.31f + i * 2.1f) * 0.05f, y = by + cosf(time * 0.27f + i * 1.3f) * 0.04f;
        float blink = 0.5f + 0.5f * sinf(time * 2.4f + i * 4.7f);
        fx_disc(r, f.x + x * f.w, f.y + y * f.h, 2.2f * px, 2.2f * px, 190, 255, 100, night * blink * 0.8f);
      }
  }
  if (live && s->cave > 0) {
    float c = (float)(s->cave > 4 ? 4 : s->cave) / 4;
    fx_disc(r, f.x + f.w * 0.5f, f.y + f.h * 0.5f, f.h * 0.34f, f.h * 0.34f, 255, 170, 70, c * 0.3f);
  }
  SDL_RenderSetClipRect(r, NULL);
}

#endif
