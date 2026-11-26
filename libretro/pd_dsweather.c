/* DS WEATHER and SEASONS (pd_dsweather.h). The weather's sprites are HeartGold / SoulSilver's own, decoded from the
 * player's cart: a/0/6/3's sprite sets as the game loads them (its members 55-58 list each set's NCGR / NCLR / NCER / NANR:
 * rain 6 / 7 / 5, the heavy rain's slanted streaks 13 / 7 / 12, snow 20 / 21 / 19, the blizzard's flakes 28 / 21 / 27,
 * diamond dust 24 / 25 / 23, sand 41 / 42 / 40 and 34 / 35 / 33) and two of its scrolling layers (the blizzard's 21 / 36 / 38,
 * the sandstorm's 37 / 36 / 38: palette, characters, screen). Their movement is ours, after the game's. */
#include "pd_dsweather.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t rd32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }

/* ---- the cart: a/0/6/3, whole (about 64 KB) ---- */
static uint8_t *narc;
static size_t narcLen;
static uint32_t narcCount, narcFat, narcImg; /* offsets into narc */

static FILE *cart;
static bool cart_read(uint32_t off, void *out, size_t n) {
  return cart && fseek(cart, (long)off, SEEK_SET) == 0 && fread(out, 1, n, cart) == n;
}

/* The file id of [path] in the FNT at [fnt], -1 if none. */
static int fnt_find(uint32_t fnt, const char *path) {
  uint16_t dir = 0xF000;
  const char *p = path;
  while (*p) {
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    uint8_t ent[8];
    if (!cart_read(fnt + (dir & 0xFFF) * 8u, ent, 8))
      return -1;
    uint32_t o = fnt + rd32(ent);
    int file = rd16(ent + 4);
    bool found = false;
    for (;;) {
      uint8_t t;
      if (!cart_read(o, &t, 1) || t == 0)
        return -1;
      o++;
      size_t n = t & 0x7F;
      char name[128];
      if (!cart_read(o, name, n))
        return -1;
      o += (uint32_t)n;
      if (t & 0x80) {
        uint8_t id[2];
        if (!cart_read(o, id, 2))
          return -1;
        o += 2;
        if (n == len && !memcmp(name, p, len)) {
          dir = rd16(id);
          found = true;
          break;
        }
      } else {
        if (n == len && !memcmp(name, p, len))
          return slash ? -1 : file;
        file++;
      }
    }
    if (!found || !slash)
      return -1;
    p = slash + 1;
  }
  return -1;
}

static const uint8_t *member(uint32_t i, size_t *len) {
  if (!narc || i >= narcCount || narcFat + i * 8 + 8 > narcLen)
    return NULL;
  uint32_t a = rd32(narc + narcFat + i * 8), b = rd32(narc + narcFat + i * 8 + 4);
  if (b <= a || narcImg + b > narcLen)
    return NULL;
  *len = b - a;
  return narc + narcImg + a;
}

static const uint8_t *block(const uint8_t *b, size_t n, const char *tag) {
  for (size_t i = 0; i + 8 <= n; i++)
    if (!memcmp(b + i, tag, 4))
      return b + i;
  return NULL;
}

/* ---- pictures: 0 = see-through, else 0xFF000000 | RGB ---- */
struct img {
  int w, h;
  uint32_t *px;
};

static bool palette(uint32_t id, uint32_t out[256]) {
  size_t n;
  const uint8_t *f = member(id, &n);
  const uint8_t *t = f ? block(f, n, "TTLP") : NULL;
  if (!t)
    return false;
  memset(out, 0, 256 * sizeof *out);
  const uint8_t *d = t + 24, *end = f + n;
  for (int i = 0; i < 256 && d + 2 <= end; i++, d += 2) {
    unsigned v = rd16(d);
    out[i] = 0xFF000000u | ((v & 31) * 255 / 31) << 16 | (((v >> 5) & 31) * 255 / 31) << 8 | (((v >> 10) & 31) * 255 / 31);
  }
  return true;
}

static const uint8_t *chars(uint32_t id, size_t *size) {
  size_t n;
  const uint8_t *f = member(id, &n);
  const uint8_t *c = f ? block(f, n, "RAHC") : NULL;
  if (!c || c + 32 > f + n)
    return NULL;
  const uint8_t *d = c + 8 + rd32(c + 0x1C);
  *size = rd32(c + 0x18);
  if (d < f || d + *size > f + n)
    return NULL;
  return d;
}

static int pix4(const uint8_t *d, size_t size, int tile, int x, int y) {
  size_t i = (size_t)tile * 32 + (size_t)y * 4 + (size_t)x / 2;
  return i < size ? (d[i] >> ((x & 1) * 4)) & 15 : 0;
}

static const uint8_t SIZES[3][4][2] = {{{8, 8}, {16, 16}, {32, 32}, {64, 64}},
                                       {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
                                       {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};

#define CELLS_MAX 24
struct set {
  struct img cell[CELLS_MAX];
  int n;
};

/* A sprite set's cells drawn whole (NCER over a 1D-mapped 4bpp sheet). */
static bool load_set(struct set *s, uint32_t charId, uint32_t palId, uint32_t cellId) {
  uint32_t pal[256];
  size_t dn, cn;
  const uint8_t *d = chars(charId, &dn);
  const uint8_t *f = member(cellId, &cn);
  const uint8_t *k = f ? block(f, cn, "KBEC") : NULL;
  if (!d || !k || !palette(palId, pal) || k + 0x18 > f + cn)
    return false;
  int count = rd16(k + 8), ext = rd16(k + 10) == 1 ? 16 : 8;
  const uint8_t *data = k + 8 + rd32(k + 12), *end = f + cn;
  if (data + count * ext > end)
    return false;
  const uint8_t *base = data + count * ext;
  s->n = 0;
  for (int i = 0; i < count && s->n < CELLS_MAX; i++) {
    const uint8_t *e = data + i * ext;
    int c = rd16(e);
    uint32_t off = rd32(e + 4);
    int x0 = 9999, y0 = 9999, x1 = -9999, y1 = -9999;
    for (int pass = 0; pass < 2; pass++) {
      struct img *im = &s->cell[s->n];
      if (pass == 1) {
        if (x1 <= x0 || y1 <= y0)
          break;
        im->w = x1 - x0;
        im->h = y1 - y0;
        im->px = calloc((size_t)im->w * im->h, sizeof *im->px);
        if (!im->px)
          return false;
      }
      for (int j = 0; j < c; j++) {
        const uint8_t *a = base + off + j * 6;
        if (a + 6 > end)
          return false;
        uint16_t a0 = rd16(a), a1 = rd16(a + 2), a2 = rd16(a + 4);
        int shape = (a0 >> 14) & 3;
        if (shape == 3)
          continue;
        int w = SIZES[shape][(a1 >> 14) & 3][0], h = SIZES[shape][(a1 >> 14) & 3][1];
        int y = a0 & 0xFF, x = a1 & 0x1FF;
        y = y >= 128 ? y - 256 : y;
        x = x >= 256 ? x - 512 : x;
        if (pass == 0) {
          if (x < x0) x0 = x;
          if (y < y0) y0 = y;
          if (x + w > x1) x1 = x + w;
          if (y + h > y1) y1 = y + h;
          continue;
        }
        bool hf = !(a0 & 0x100) && (a1 & 0x1000), vf = !(a0 & 0x100) && (a1 & 0x2000);
        int tile = a2 & 0x3FF, bank = a2 >> 12;
        for (int py = 0; py < h; py++)
          for (int px = 0; px < w; px++) {
            int v = pix4(d, dn, tile + (py / 8) * (w / 8) + px / 8, px & 7, py & 7);
            if (!v)
              continue;
            int dx = hf ? w - 1 - px : px, dy = vf ? h - 1 - py : py;
            im->px[(size_t)(y - y0 + dy) * im->w + (x - x0 + dx)] = pal[bank * 16 + v];
          }
      }
    }
    if (s->cell[s->n].px)
      s->n++;
  }
  return s->n > 0;
}

/* A scrolling layer: the screen's 32 x 32 tiles over 4bpp characters. */
static bool load_layer(struct img *im, uint32_t palId, uint32_t charId, uint32_t screenId) {
  uint32_t pal[256];
  size_t dn, sn;
  const uint8_t *d = chars(charId, &dn);
  const uint8_t *f = member(screenId, &sn);
  const uint8_t *s = f ? block(f, sn, "NRCS") : NULL;
  if (!d || !s || !palette(palId, pal))
    return false;
  int w = rd16(s + 8), h = rd16(s + 10);
  const uint8_t *map = s + 20;
  if (w <= 0 || h <= 0 || w > 512 || h > 512 || map + (size_t)(w / 8) * (h / 8) * 2 > f + sn)
    return false;
  im->w = w;
  im->h = h;
  im->px = calloc((size_t)w * h, sizeof *im->px);
  if (!im->px)
    return false;
  for (int ty = 0; ty < h / 8; ty++)
    for (int tx = 0; tx < w / 8; tx++) {
      uint16_t e = rd16(map + ((size_t)ty * (w / 8) + tx) * 2);
      int tile = e & 0x3FF, bank = e >> 12;
      bool hf = e & 0x400, vf = e & 0x800;
      for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
          int v = pix4(d, dn, tile, hf ? 7 - x : x, vf ? 7 - y : y);
          if (v)
            im->px[(size_t)(ty * 8 + y) * w + tx * 8 + x] = pal[bank * 16 + v];
        }
    }
  return true;
}

static struct set sRain, sStorm, sSnow, sFlakes, sDust, sSand, sGrit;
static struct img lBlizzard, lSand;
static bool ready;

static void free_set(struct set *s) {
  for (int i = 0; i < s->n; i++)
    free(s->cell[i].px);
  memset(s, 0, sizeof *s);
}

void dsw_close(void) {
  free_set(&sRain);
  free_set(&sStorm);
  free_set(&sSnow);
  free_set(&sFlakes);
  free_set(&sDust);
  free_set(&sSand);
  free_set(&sGrit);
  free(lBlizzard.px);
  free(lSand.px);
  memset(&lBlizzard, 0, sizeof lBlizzard);
  memset(&lSand, 0, sizeof lSand);
  free(narc);
  narc = NULL;
  narcLen = narcCount = 0;
  ready = false;
  dsw_reset();
}

bool dsw_ready(void) { return ready; }

bool dsw_open(const char *path) {
  dsw_close();
  if (!path || !*path)
    return false;
  cart = fopen(path, "rb");
  uint8_t h[0x50], fe[8];
  bool ok = false;
  // HeartGold / SoulSilver, any language: IPK? IPG?
  if (cart && cart_read(0, h, sizeof h) && h[0xC] == 'I' && h[0xD] == 'P' && (h[0xE] == 'K' || h[0xE] == 'G')) {
    int id = fnt_find(rd32(h + 0x40), "a/0/6/3");
    if (id >= 0 && cart_read(rd32(h + 0x48) + (uint32_t)id * 8, fe, 8) && rd32(fe + 4) > rd32(fe) &&
        rd32(fe + 4) - rd32(fe) < (1u << 20)) {
      narcLen = rd32(fe + 4) - rd32(fe);
      narc = malloc(narcLen);
      ok = narc && cart_read(rd32(fe), narc, narcLen);
    }
  }
  if (cart)
    fclose(cart);
  cart = NULL;
  if (ok && narcLen > 0x1C && !memcmp(narc, "NARC", 4) && !memcmp(narc + 0x10, "BTAF", 4)) {
    narcCount = rd16(narc + 0x18);
    narcFat = 0x1C;
    uint32_t fntb = 0x10 + rd32(narc + 0x14);
    ok = fntb + 8 <= narcLen;
    if (ok)
      narcImg = fntb + rd32(narc + fntb + 4) + 8;
  } else
    ok = false;
  ok = ok && narcCount >= 55 && load_set(&sRain, 6, 7, 5) && sRain.n >= 4 && load_set(&sStorm, 13, 7, 12) && sStorm.n >= 4 &&
       load_set(&sSnow, 20, 21, 19) && load_set(&sFlakes, 28, 21, 27) && load_set(&sDust, 24, 25, 23) &&
       load_set(&sSand, 41, 42, 40) && load_set(&sGrit, 34, 35, 33) && load_layer(&lBlizzard, 21, 36, 38) &&
       load_layer(&lSand, 37, 36, 38);
  if (!ok) {
    dsw_close();
    return false;
  }
  free(narc); // everything is decoded
  narc = NULL;
  ready = true;
  return true;
}

/* ---- drawing ---- */
static uint32_t seed = 0x9E3779B9u;
static float rnd(void) {
  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  return (float)(seed & 0xFFFFFF) / 16777216.0f;
}

static void blit(const struct dsw_target *t, const struct img *im, float fx, float fy, int alpha) {
  if (!im || !im->px)
    return;
  int s = t->scale, x0 = (int)floorf(fx) * s, y0 = (int)floorf(fy) * s;
  for (int y = 0; y < im->h; y++) {
    int cy = y0 + y * s;
    if (cy + s <= 0 || cy >= t->h)
      continue;
    for (int x = 0; x < im->w; x++) {
      uint32_t p = im->px[(size_t)y * im->w + x];
      if (!p)
        continue;
      int cx = x0 + x * s;
      if (cx + s <= 0 || cx >= t->w)
        continue;
      for (int j = 0; j < s; j++)
        for (int i = 0; i < s; i++)
          t->plot(cx + i, cy + j, p & 0xFFFFFF, alpha);
    }
  }
}

/* A small drawing of ours: rows of '.' (none) and palette letters. */
struct doodle {
  int w, h;
  const char *rows;
};
static void doodle(const struct dsw_target *t, const struct doodle *d, const uint32_t *pal, float fx, float fy, bool flip,
                   int alpha) {
  int s = t->scale, x0 = (int)floorf(fx) * s, y0 = (int)floorf(fy) * s;
  for (int y = 0; y < d->h; y++)
    for (int x = 0; x < d->w; x++) {
      char c = d->rows[y * d->w + (flip ? d->w - 1 - x : x)];
      if (c < 'a' || c > 'd')
        continue;
      int cx = x0 + x * s, cy = y0 + y * s;
      for (int j = 0; j < s; j++)
        for (int i = 0; i < s; i++)
          t->plot(cx + i, cy + j, pal[c - 'a'], alpha);
    }
}

#define PARTS 64
struct part {
  float x, y, vx, vy, phase;
  int kind, life, look;
};
static struct part parts[PARTS];
static int nParts, shownKind = -100;
static unsigned frame;
static float layerX, layerY;

void dsw_reset(void) {
  nParts = 0;
  shownKind = -100;
}

/* The canvas in DS pixels, a margin round it for things that come in from the edges. */
static void area(const struct dsw_target *t, float *w, float *h) {
  *w = (float)t->w / t->scale;
  *h = (float)t->h / t->scale;
}

static void start(int kind, int n, const struct dsw_target *t, void (*spawn)(struct part *, float, float, bool)) {
  float w, h;
  area(t, &w, &h);
  if (kind != shownKind) {
    shownKind = kind;
    nParts = n > PARTS ? PARTS : n;
    for (int i = 0; i < nParts; i++)
      spawn(&parts[i], w, h, true);
  }
}

/* -- rain: HeartGold's three streak lengths, falling a little to the left, each ending in a splash -- */
static void rain_spawn(struct part *p, float w, float h, bool anywhere) {
  p->kind = (int)(rnd() * 3);
  float speed = 6 + 3 * p->kind + rnd() * 2;
  p->vy = speed;
  p->vx = -speed / 4;
  p->x = rnd() * (w + h / 4);
  p->y = anywhere ? rnd() * h - 64 : -64 - rnd() * 40;
  p->life = (int)(h * (0.35f + 0.65f * rnd())); // where it lands
  p->look = 0;
}
static void storm_spawn(struct part *p, float w, float h, bool anywhere) {
  p->kind = (int)(rnd() * 3);
  float speed = 10 + 3 * p->kind + rnd() * 3;
  p->vy = speed;
  p->vx = -speed;
  p->x = rnd() * (w + h);
  p->y = anywhere ? rnd() * h - 96 : -96 - rnd() * 60;
  p->life = (int)(h * (0.4f + 0.6f * rnd()));
  p->look = 0;
}
static void falling(const struct dsw_target *t, const struct set *s, void (*spawn)(struct part *, float, float, bool)) {
  float w, h;
  area(t, &w, &h);
  for (int i = 0; i < nParts; i++) {
    struct part *p = &parts[i];
    const struct img *im = &s->cell[p->kind];
    if (p->look > 0) { // a splash where it landed
      blit(t, &s->cell[3], p->x, p->y, 256);
      if (--p->look == 0)
        spawn(p, w, h, false);
      continue;
    }
    p->x += p->vx;
    p->y += p->vy;
    if (p->y + im->h >= p->life) {
      p->look = 5;
      p->x = p->x - (float)s->cell[3].w / 2;
      p->y = (float)p->life - (float)s->cell[3].h / 2;
      continue;
    }
    blit(t, im, p->x, p->y, 256);
  }
}

/* -- snow: its flakes drifting down, swaying -- */
static void snow_spawn(struct part *p, float w, float h, bool anywhere) {
  p->kind = (int)(rnd() * 4);
  p->vy = 0.35f + 0.25f * p->kind + rnd() * 0.3f;
  p->vx = -0.2f + rnd() * 0.4f;
  p->phase = rnd() * 6.28f;
  p->x = rnd() * w;
  p->y = anywhere ? rnd() * h : -8 - rnd() * 20;
}
static void snowing(const struct dsw_target *t, const struct set *s, float wind, int alpha) {
  float w, h;
  area(t, &w, &h);
  for (int i = 0; i < nParts; i++) {
    struct part *p = &parts[i];
    p->phase += 0.04f;
    p->x += p->vx + wind + sinf(p->phase) * 0.35f;
    p->y += p->vy;
    if (p->y > h + 8 || p->x < -16 || p->x > w + 16) {
      snow_spawn(p, w, h, false);
      if (wind > 0.5f)
        p->x = rnd() * w - w * 0.5f;
    }
    blit(t, &s->cell[p->kind % s->n], p->x, p->y, alpha);
  }
}

/* -- sand: the sandstorm's layer blowing across, and its grit -- */
static void sand_spawn(struct part *p, float w, float h, bool anywhere) {
  p->kind = (int)(rnd() * 8);
  p->vx = 3 + rnd() * 4;
  p->vy = 0.5f + rnd() * 0.8f;
  p->phase = rnd() * 6.28f;
  p->x = anywhere ? rnd() * w : -16 - rnd() * 40;
  p->y = rnd() * h;
}
static void layer(const struct dsw_target *t, const struct img *im, float ox, float oy, int alpha) {
  if (!im->px)
    return;
  int s = t->scale;
  for (int cy = 0; cy < t->h; cy++) {
    int y = ((int)(cy / s + oy) % im->h + im->h) % im->h;
    const uint32_t *row = im->px + (size_t)y * im->w;
    for (int cx = 0; cx < t->w; cx++) {
      uint32_t p = row[((int)(cx / s + ox) % im->w + im->w) % im->w];
      if (p)
        t->plot(cx, cy, p & 0xFFFFFF, alpha);
    }
  }
}

void dsw_weather(int kind, const struct dsw_target *t) {
  if (!ready || !t || t->scale < 1)
    return;
  frame++;
  float w, h;
  area(t, &w, &h);
  int density = (int)(w * h / (256.0f * 192.0f) * 100); // per cent of the DS's screen
  if (density < 60)
    density = 60;
  switch (kind) {
  case DSW_RAIN:
    start(kind, 34 * density / 100, t, rain_spawn);
    falling(t, &sRain, rain_spawn);
    break;
  case DSW_STORM:
    start(kind, 16 * density / 100, t, storm_spawn);
    falling(t, &sStorm, storm_spawn);
    break;
  case DSW_SNOW:
    start(kind, 44 * density / 100, t, snow_spawn);
    snowing(t, &sSnow, 0, 256);
    break;
  case DSW_SAND: {
    start(kind, 56 * density / 100, t, sand_spawn);
    layerX -= 5.0f;
    layerY -= 1.0f;
    for (int y = 0; y < t->h; y++) // the air thick with sand
      for (int x = 0; x < t->w; x++)
        t->plot(x, y, 0xD8B070, 48);
    layer(t, &lSand, layerX, layerY, 190);
    for (int i = 0; i < nParts; i++) {
      struct part *p = &parts[i];
      p->phase += 0.1f;
      p->x += p->vx;
      p->y += p->vy + sinf(p->phase) * 0.6f;
      if (p->x > w + 16 || p->y > h + 16)
        sand_spawn(p, w, h, false);
      const struct set *s = p->kind < 5 ? &sSand : &sGrit;
      blit(t, &s->cell[p->kind % s->n], p->x, p->y, 256);
    }
    break;
  }
  default:
    shownKind = -100;
    break;
  }
}

/* ---- the seasons ---- */
/* Our own drawings (the DS games have no such particles): a petal turning over, a leaf tumbling. */
static const struct doodle PETAL[3] = {
    {7, 5, "..aab.."
           ".aaabb."
           "aaaabbb"
           ".aabbb."
           "..bb..."},
    {7, 4, ".aaab.."
           "aaaabbb"
           ".aabbbb"
           "...bb.."},
    {5, 4, ".aab."
           "aaabb"
           ".abbb"
           "..b.."},
};
static const struct doodle LEAF[4] = {
    {9, 7, ".......c."
           ".....aac."
           "...aaaab."
           "..aaaaab."
           ".aaaaabb."
           "aaaabbb.."
           ".bbbb...."},
    {7, 8, ".....c."
           "....ac."
           "...aab."
           "..aaab."
           ".aaaab."
           "aaaabb."
           "aabb..."
           ".b....."},
    {4, 8, "..c."
           ".ac."
           ".aa."
           "aaa."
           "aab."
           "aab."
           ".ab."
           ".b.."},
    {9, 5, "c........"
           ".caaaab.."
           "..aaaaabb"
           "...aabbb."
           "....bb..."},
};
static const struct doodle FLAKE = {3, 3, ".a."
                                          "aba"
                                          ".a."};

static int seasonShown = -100;
struct sparkle {
  float x, y;
  int t, look;
};
static struct sparkle sparkles[8];

static void season_spawn(struct part *p, float w, float h, bool anywhere) {
  p->kind = (int)(rnd() * 3);
  p->look = (int)(rnd() * 4);
  p->phase = rnd() * 6.28f;
  p->vx = 0.25f + rnd() * 0.35f;
  p->vy = 0.25f + rnd() * 0.3f;
  p->x = anywhere ? rnd() * w : rnd() * (w + h * 0.6f) - h * 0.6f;
  p->y = anywhere ? rnd() * h : -6 - rnd() * 30;
  p->life = 0;
}

void dsw_season(int season, const struct dsw_target *t) {
  if (!t || t->scale < 1 || season < 0)
    return;
  float w, h;
  area(t, &w, &h);
  frame++;
  int n = season == SEASON_WINTER ? 26 : season == SEASON_SUMMER ? 0 : 32;
  n = (int)(n * w * h / (240.0f * 160.0f));
  if (season != seasonShown) {
    seasonShown = season;
    shownKind = -100;
    memset(sparkles, 0, sizeof sparkles);
  }
  if (shownKind != 100 + season) {
    shownKind = 100 + season;
    nParts = n > PARTS ? PARTS : n;
    for (int i = 0; i < nParts; i++)
      season_spawn(&parts[i], w, h, true);
  }
  // Spring: petals in two pinks; autumn: leaves orange, red or gold, a darker side and a stem.
  static const uint32_t PETALS[2][4] = {{0xF890C0, 0xD86090, 0, 0}, {0xFFC8E0, 0xF080B0, 0, 0}};
  static const uint32_t LEAVES[3][4] = {
      {0xD84818, 0x902810, 0x582010, 0}, {0xB82818, 0x701808, 0x481008, 0}, {0xE89020, 0xA05010, 0x583010, 0}};
  static const uint32_t SNOW[4] = {0xF0F4FF, 0xFFFFFF, 0, 0};
  for (int i = 0; i < nParts; i++) {
    struct part *p = &parts[i];
    p->phase += season == SEASON_AUTUMN ? 0.05f : 0.07f;
    float sway = sinf(p->phase);
    if (season == SEASON_WINTER) {
      p->x += sway * 0.25f + 0.1f;
      p->y += 0.3f + 0.2f * p->kind;
    } else {
      // Carried on the wind, down and to the right, fluttering.
      p->x += p->vx + sway * (season == SEASON_AUTUMN ? 0.6f : 0.4f);
      p->y += p->vy + cosf(p->phase * 1.3f) * 0.15f;
    }
    if (p->y > h + 6 || p->x > w + 8)
      season_spawn(p, w, h, false);
    int step = (int)((p->phase + 6.28f) * 1.3f);
    if (season == SEASON_SPRING)
      doodle(t, &PETAL[(step + p->look) % 3], PETALS[p->kind & 1], p->x, p->y, sway < 0, 256);
    else if (season == SEASON_AUTUMN)
      doodle(t, &LEAF[(step + p->look) % 4], LEAVES[p->kind % 3], p->x, p->y, p->look & 1, 256);
    else if (season == SEASON_WINTER) {
      if (ready && sSnow.n)
        blit(t, &sSnow.cell[1 + p->kind % 3], p->x, p->y, 256); // the cart's flakes
      else
        doodle(t, &FLAKE, SNOW, p->x, p->y, false, 256);
    }
  }
  // Winter: now and then a glint of diamond dust in the air (the cart's sparkles).
  if (season == SEASON_WINTER && ready && sDust.n >= 4)
    for (int i = 0; i < 8; i++) {
      struct sparkle *s = &sparkles[i];
      if (s->t <= 0) {
        if (rnd() < 0.02f) {
          s->t = 30;
          s->x = rnd() * w;
          s->y = rnd() * h;
          s->look = (int)(rnd() * 4);
        }
        continue;
      }
      s->t--;
      int a = s->t > 15 ? (30 - s->t) * 17 : s->t * 17;
      blit(t, &sDust.cell[s->look], s->x, s->y, a < 1 ? 1 : a);
    }
}

int dsw_season_now(int mode) {
  if (mode >= 3 && mode <= 6)
    return mode - 3;
  if (mode != 1 && mode != 2)
    return SEASON_NONE;
  time_t now = time(NULL);
  struct tm tmv;
#ifdef _WIN32
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  int m = tmv.tm_mon; // 0 = January
  if (mode == 1)
    return m % 4;      // Black / White: January spring, February summer, March autumn, April winter ...
  if (m >= 2 && m <= 4)
    return SEASON_SPRING;
  if (m >= 5 && m <= 7)
    return SEASON_SUMMER;
  if (m >= 8 && m <= 10)
    return SEASON_AUTUMN;
  return SEASON_WINTER;
}
