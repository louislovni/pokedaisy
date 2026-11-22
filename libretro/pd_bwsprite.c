/* BATTLE SPRITES: Black / White's animated battle pictures, decoded from the player's cart (pd_bwsprite.h). The Nitro
 * formats as the games use them: the parts sheet a linear 4bpp bitmap (RAHC +0x14 = 1) addressed by 2D-mapped tile
 * numbers (KBEC mapping 4), every OAM affine and double-size, the cells animated by SRT / translation frames. */
#include "pd_bwsprite.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the cart's file system ---- */
static FILE *cart;
static uint32_t narcBase, narcCount, narcFat, narcImg; /* a/0/0/4: where its members are, from the cart's start */

static uint32_t rd32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static int16_t rs16(const uint8_t *b) { return (int16_t)rd16(b); }

static bool cart_read(uint32_t off, void *out, size_t n) {
  return cart && fseek(cart, (long)off, SEEK_SET) == 0 && fread(out, 1, n, cart) == n;
}

/* The file id of [path] ("a/0/0/4") in the FNT at [fnt], -1 if none. */
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
        if (n == len && !memcmp(name, p, len)) {
          if (slash)
            return -1;
          return file;
        }
        file++;
      }
    }
    if (!found || !slash)
      return -1;
    p = slash + 1;
  }
  return -1;
}

void bws_close(void);
static void cache_clear(void);

bool bws_open(const char *path) {
  bws_close();
  if (!path || !*path)
    return false;
  cart = fopen(path, "rb");
  uint8_t h[0x50];
  if (!cart || !cart_read(0, h, sizeof h))
    goto fail;
  // Black / White / Black 2 / White 2, any language: IRB? IRA? IRE? IRD?
  if (h[0xC] != 'I' || h[0xD] != 'R' || !strchr("BAED", h[0xE]))
    goto fail;
  uint32_t fnt = rd32(h + 0x40), fat = rd32(h + 0x48);
  int id = fnt_find(fnt, "a/0/0/4");
  uint8_t fe[8], nh[0x1C];
  if (id < 0 || !cart_read(fat + (uint32_t)id * 8, fe, 8))
    goto fail;
  narcBase = rd32(fe);
  if (!cart_read(narcBase, nh, sizeof nh) || memcmp(nh, "NARC", 4) || memcmp(nh + 0x10, "BTAF", 4))
    goto fail;
  narcCount = rd16(nh + 0x18);
  narcFat = narcBase + 0x1C;
  uint32_t fntb = narcBase + 0x10 + rd32(nh + 0x14);
  uint8_t fb[8];
  if (!cart_read(fntb, fb, 8))
    goto fail;
  narcImg = fntb + rd32(fb + 4) + 8;
  if (narcCount < 650 * 20)
    goto fail;
  return true;
fail:
  bws_close();
  return false;
}

void bws_close(void) {
  if (cart)
    fclose(cart);
  cart = NULL;
  narcCount = 0;
  cache_clear();
}

bool bws_ready(void) { return cart && narcCount; }

/* LZ10 / LZ11 (or a raw member), into a new buffer. */
static uint8_t *unlz(const uint8_t *b, size_t n, size_t *outLen) {
  if (n < 4)
    return NULL;
  if (b[0] != 0x10 && b[0] != 0x11) {
    uint8_t *o = malloc(n);
    if (o) {
      memcpy(o, b, n);
      *outLen = n;
    }
    return o;
  }
  size_t len = b[1] | b[2] << 8 | (size_t)b[3] << 16, i = 4, w = 0;
  if (!len && n >= 8) {
    len = rd32(b + 4);
    i = 8;
  }
  if (!len || len > (8u << 20))
    return NULL;
  uint8_t *o = malloc(len);
  if (!o)
    return NULL;
  bool lz11 = b[0] == 0x11;
  while (w < len && i < n) {
    uint8_t flags = b[i++];
    for (int k = 0; k < 8 && w < len; k++) {
      if (!(flags & (0x80 >> k))) {
        if (i >= n)
          goto bad;
        o[w++] = b[i++];
        continue;
      }
      size_t l, d;
      if (!lz11) {
        if (i + 1 >= n)
          goto bad;
        l = (b[i] >> 4) + 3;
        d = (((size_t)b[i] & 0xF) << 8 | b[i + 1]) + 1;
        i += 2;
      } else {
        if (i + 1 >= n)
          goto bad;
        int ind = b[i] >> 4;
        if (ind == 0) {
          if (i + 2 >= n)
            goto bad;
          l = (((size_t)b[i] & 0xF) << 4 | b[i + 1] >> 4) + 0x11;
          d = (((size_t)b[i + 1] & 0xF) << 8 | b[i + 2]) + 1;
          i += 3;
        } else if (ind == 1) {
          if (i + 3 >= n)
            goto bad;
          l = (((size_t)b[i] & 0xF) << 12 | (size_t)b[i + 1] << 4 | b[i + 2] >> 4) + 0x111;
          d = (((size_t)b[i + 2] & 0xF) << 8 | b[i + 3]) + 1;
          i += 4;
        } else {
          l = (size_t)ind + 1;
          d = (((size_t)b[i] & 0xF) << 8 | b[i + 1]) + 1;
          i += 2;
        }
      }
      if (d > w)
        goto bad;
      for (size_t c = 0; c < l && w < len; c++, w++)
        o[w] = o[w - d];
    }
  }
  if (w < len)
    goto bad;
  *outLen = len;
  return o;
bad:
  free(o);
  return NULL;
}

/* Member [i] of a/0/0/4, unpacked; NULL if empty. */
static uint8_t *member(uint32_t i, size_t *len) {
  uint8_t e[8];
  if (i >= narcCount || !cart_read(narcFat + i * 8, e, 8))
    return NULL;
  uint32_t a = rd32(e), b = rd32(e + 4);
  if (b <= a || b - a > (2u << 20))
    return NULL;
  uint8_t *raw = malloc(b - a);
  if (!raw || !cart_read(narcImg + a, raw, b - a)) {
    free(raw);
    return NULL;
  }
  uint8_t *out = unlz(raw, b - a, len);
  free(raw);
  return out;
}

/* The block tagged [tag] in a Nitro file (its 8-byte block header's start), or NULL. */
static const uint8_t *block(const uint8_t *b, size_t n, const char *tag) {
  for (size_t i = 0; i + 8 <= n; i++)
    if (!memcmp(b + i, tag, 4))
      return b + i;
  return NULL;
}

/* ---- one picture, decoded ---- */
struct oam { int16_t x, y; uint8_t w, h; uint16_t tile; bool hf, vf; };
struct cell { int first, count; };
struct frame { uint16_t index, dur; int16_t px, py; float rot, sx, sy; };
struct seq { int first, count, loop, total; };
struct node { uint16_t seq; int16_t x, y; };
struct mcell { int first, count; };

struct pic {
  int national;
  bool back, ok;
  uint8_t *sheet;
  int sheetW, sheetH; /* pixels */
  struct oam *oams;
  struct cell *cells;
  int nCells;
  struct frame *frames, *mframes;
  struct seq *seqs, mseq;
  int nSeqs;
  struct node *nodes;
  struct mcell *mcs;
  int nMcs;
  uint32_t pal[2][16];
  unsigned used;
};

#define PICS 6
static struct pic pics[PICS];
static unsigned useClock;

static void pic_free(struct pic *p) {
  free(p->sheet);
  free(p->oams);
  free(p->cells);
  free(p->frames);
  free(p->mframes);
  free(p->seqs);
  free(p->nodes);
  free(p->mcs);
  memset(p, 0, sizeof *p);
}
static void cache_clear(void) {
  for (int i = 0; i < PICS; i++)
    pic_free(&pics[i]);
}

static const uint8_t SIZES[3][4][2] = {{{8, 8}, {16, 16}, {32, 32}, {64, 64}},
                                       {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
                                       {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};

static bool parse_ncer(struct pic *p, const uint8_t *f, size_t n) {
  const uint8_t *k = block(f, n, "KBEC");
  if (!k || k + 0x18 > f + n)
    return false;
  int count = rd16(k + 8);
  int ext = rd16(k + 10) == 1 ? 16 : 8;
  const uint8_t *data = k + 8 + rd32(k + 12);
  const uint8_t *end = f + n;
  if (data + count * ext > end)
    return false;
  const uint8_t *base = data + count * ext;
  int total = 0;
  for (int i = 0; i < count; i++)
    total += rd16(data + i * ext);
  p->cells = calloc((size_t)count, sizeof *p->cells);
  p->oams = calloc((size_t)(total ? total : 1), sizeof *p->oams);
  if (!p->cells || !p->oams)
    return false;
  int at = 0;
  for (int i = 0; i < count; i++) {
    const uint8_t *e = data + i * ext;
    int c = rd16(e);
    uint32_t off = rd32(e + 4);
    p->cells[i].first = at;
    for (int j = 0; j < c; j++) {
      const uint8_t *a = base + off + j * 6;
      if (a + 6 > end)
        return false;
      uint16_t a0 = rd16(a), a1 = rd16(a + 2), a2 = rd16(a + 4);
      int shape = (a0 >> 14) & 3;
      if (shape == 3)
        continue;
      struct oam *o = &p->oams[at++];
      o->w = SIZES[shape][(a1 >> 14) & 3][0];
      o->h = SIZES[shape][(a1 >> 14) & 3][1];
      int y = a0 & 0xFF, x = a1 & 0x1FF;
      y = y >= 128 ? y - 256 : y;
      x = x >= 256 ? x - 512 : x;
      if ((a0 & 0x300) == 0x300) { // affine, double size: the picture sits in the middle of its box
        x += o->w / 2;
        y += o->h / 2;
      }
      o->x = (int16_t)x;
      o->y = (int16_t)y;
      o->tile = a2 & 0x3FF;
      o->hf = !(a0 & 0x100) && (a1 & 0x1000);
      o->vf = !(a0 & 0x100) && (a1 & 0x2000);
    }
    p->cells[i].count = at - p->cells[i].first;
  }
  p->nCells = count;
  return true;
}

/* An NANR / NMAR (KNBA): every sequence's frames. [one]: only the first sequence (into mseq). */
static bool parse_anim(struct pic *p, const uint8_t *f, size_t n, bool multi) {
  const uint8_t *k = block(f, n, "KNBA");
  if (!k || k + 0x20 > f + n)
    return false;
  const uint8_t *end = f + n;
  int count = rd16(k + 8);
  const uint8_t *seqs = k + 8 + rd32(k + 12), *frames = k + 8 + rd32(k + 16), *cont = k + 8 + rd32(k + 20);
  if (count < 1 || seqs + count * 16 > end)
    return false;
  int total = 0;
  for (int i = 0; i < (multi ? 1 : count); i++)
    total += rd16(seqs + i * 16);
  struct frame *fr = calloc((size_t)(total ? total : 1), sizeof *fr);
  struct seq *sq = multi ? &p->mseq : calloc((size_t)count, sizeof *sq);
  if (!fr || !sq) {
    free(fr);
    if (!multi)
      free(sq);
    return false;
  }
  int at = 0;
  for (int i = 0; i < (multi ? 1 : count); i++) {
    const uint8_t *s = seqs + i * 16;
    int nf = rd16(s), et = rd16(s + 4);
    uint32_t fo = rd32(s + 12);
    sq[i].first = at;
    sq[i].count = nf;
    sq[i].loop = rd16(s + 2);
    sq[i].total = 0;
    for (int j = 0; j < nf; j++) {
      const uint8_t *e = frames + fo + j * 8;
      if (e + 8 > end)
        goto bad;
      const uint8_t *c = cont + rd32(e);
      struct frame *x = &fr[at++];
      x->dur = rd16(e + 4);
      x->sx = x->sy = 1;
      if (c + (et == 1 ? 16 : et == 2 ? 8 : 2) > end)
        goto bad;
      x->index = rd16(c);
      if (et == 1) {
        x->rot = (float)rd16(c + 2) * 6.2831853f / 65536.0f;
        x->sx = (float)(int32_t)rd32(c + 4) / 4096.0f;
        x->sy = (float)(int32_t)rd32(c + 8) / 4096.0f;
        x->px = rs16(c + 12);
        x->py = rs16(c + 14);
      } else if (et == 2) {
        x->px = rs16(c + 4);
        x->py = rs16(c + 6);
      }
      sq[i].total += x->dur;
    }
  }
  if (multi) {
    p->mframes = fr;
  } else {
    p->frames = fr;
    p->seqs = sq;
    p->nSeqs = count;
  }
  return true;
bad:
  free(fr);
  if (!multi)
    free(sq);
  return false;
}

static bool parse_nmcr(struct pic *p, const uint8_t *f, size_t n) {
  const uint8_t *k = block(f, n, "KBCM");
  if (!k || k + 0x18 > f + n)
    return false;
  const uint8_t *end = f + n;
  int count = rd16(k + 8);
  const uint8_t *arr = k + 8 + rd32(k + 12), *hier = k + 8 + rd32(k + 16);
  if (arr + count * 8 > end)
    return false;
  int total = 0;
  for (int i = 0; i < count; i++)
    total += rd16(arr + i * 8);
  p->mcs = calloc((size_t)(count ? count : 1), sizeof *p->mcs);
  p->nodes = calloc((size_t)(total ? total : 1), sizeof *p->nodes);
  if (!p->mcs || !p->nodes)
    return false;
  int at = 0;
  for (int i = 0; i < count; i++) {
    const uint8_t *e = arr + i * 8;
    int nn = rd16(e);
    uint32_t ho = rd32(e + 4);
    p->mcs[i].first = at;
    p->mcs[i].count = nn;
    for (int j = 0; j < nn; j++) {
      const uint8_t *h = hier + ho + j * 8;
      if (h + 8 > end)
        return false;
      p->nodes[at].seq = rd16(h);
      p->nodes[at].x = rs16(h + 2);
      p->nodes[at].y = rs16(h + 4);
      at++;
    }
  }
  p->nMcs = count;
  return true;
}

static bool parse_nclr(uint32_t out[16], const uint8_t *f, size_t n) {
  const uint8_t *t = block(f, n, "TTLP");
  if (!t || t + 0x18 + 32 > f + n)
    return false;
  const uint8_t *d = t + 0x18;
  for (int i = 0; i < 16; i++) {
    uint16_t c = rd16(d + 2 * i);
    uint32_t r = (c & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = ((c >> 10) & 31) * 255 / 31;
    out[i] = r << 16 | g << 8 | b;
  }
  return true;
}

static bool load(struct pic *p, int national, bool back) {
  p->national = national;
  p->back = back;
  uint32_t m = (uint32_t)national * 20 + (back ? 9 : 0);
  size_t n[7] = {0};
  uint8_t *f[7] = {member(m + 2, &n[0]), member(m + 4, &n[1]), member(m + 5, &n[2]), member(m + 6, &n[3]),
                   member(m + 7, &n[4]), member((uint32_t)national * 20 + 18, &n[5]),
                   member((uint32_t)national * 20 + 19, &n[6])};
  bool ok = f[0] && f[1] && f[2] && f[3] && f[4] && f[5];
  if (ok) {
    const uint8_t *c = block(f[0], n[0], "RAHC");
    ok = c && c + 0x20 <= f[0] + n[0];
    if (ok) {
      p->sheetH = rd16(c + 8) * 8;
      p->sheetW = rd16(c + 10) * 8;
      uint32_t size = rd32(c + 0x18), off = rd32(c + 0x1C);
      ok = c + 8 + off + size <= f[0] + n[0] && p->sheetW > 0 && size >= (uint32_t)(p->sheetW * p->sheetH / 2);
      if (ok && (p->sheet = malloc(size)))
        memcpy(p->sheet, c + 8 + off, size);
      ok = ok && p->sheet;
    }
  }
  ok = ok && parse_ncer(p, f[1], n[1]) && parse_anim(p, f[2], n[2], false) && parse_nmcr(p, f[3], n[3]) &&
       parse_anim(p, f[4], n[4], true) && parse_nclr(p->pal[0], f[5], n[5]);
  if (ok && !(f[6] && parse_nclr(p->pal[1], f[6], n[6])))
    memcpy(p->pal[1], p->pal[0], sizeof p->pal[0]);
  for (int i = 0; i < 7; i++)
    free(f[i]);
  p->ok = ok;
  return ok;
}

static struct pic *get(int national, bool back) {
  struct pic *free_ = NULL;
  for (int i = 0; i < PICS; i++) {
    if (pics[i].national == national && pics[i].back == back) {
      pics[i].used = ++useClock;
      return pics[i].ok ? &pics[i] : NULL;
    }
    if (!free_ || pics[i].used < free_->used)
      free_ = &pics[i];
  }
  pic_free(free_);
  load(free_, national, back);
  free_->used = ++useClock;
  return free_->ok ? free_ : NULL;
}

/* The frame of [s] at time [t] (frames), looping from its loop frame. */
static const struct frame *frame_at(const struct frame *fr, const struct seq *s, int t) {
  if (s->count <= 0)
    return NULL;
  if (s->total <= 0)
    return &fr[s->first];
  int loopStart = 0;
  for (int i = 0; i < s->loop && i < s->count; i++)
    loopStart += fr[s->first + i].dur;
  if (t >= s->total) {
    int span = s->total - loopStart;
    t = loopStart + (span > 0 ? (t - loopStart) % span : 0);
  }
  for (int i = 0; i < s->count; i++) {
    const struct frame *f = &fr[s->first + i];
    if (t < f->dur)
      return f;
    t -= f->dur;
  }
  return &fr[s->first + s->count - 1];
}

bool bws_has(int national, bool back) {
  return bws_ready() && national >= 1 && national <= 649 && get(national, back) != NULL;
}

int bws_top(int national, bool back) {
  if (!bws_ready() || national < 1 || national > 649)
    return 0;
  struct pic *p = get(national, back);
  if (!p || !p->mframes || p->mseq.count <= 0)
    return 0;
  const struct frame *mf = &p->mframes[p->mseq.first];
  if (mf->index >= p->nMcs)
    return 0;
  const struct mcell *mc = &p->mcs[mf->index];
  int top = 0;
  for (int ni = 0; ni < mc->count; ni++) {
    const struct node *nd = &p->nodes[mc->first + ni];
    if (nd->seq >= p->nSeqs || p->seqs[nd->seq].count <= 0)
      continue;
    const struct frame *f = &p->frames[p->seqs[nd->seq].first];
    if (f->index >= p->nCells)
      continue;
    const struct cell *c = &p->cells[f->index];
    for (int oi = 0; oi < c->count; oi++) {
      int y = nd->y + f->py + mf->py + p->oams[c->first + oi].y;
      top = y < top ? y : top;
    }
  }
  return top;
}

bool bws_draw(int national, bool back, bool shiny, int t, uint32_t *dst, int stride, const int clip[4], float ox, float oy,
              float scale) {
  if (!bws_ready() || national < 1 || national > 649 || scale <= 0)
    return false;
  struct pic *p = get(national, back);
  if (!p)
    return false;
  const struct frame *mf = frame_at(p->mframes, &p->mseq, t);
  if (!mf || mf->index >= p->nMcs)
    return false;
  const uint32_t *pal = p->pal[shiny ? 1 : 0];
  const struct mcell *mc = &p->mcs[mf->index];
  // The nodes from the last to the first, each over the ones before: the first ends on top (as the DS draws them).
  for (int ni = mc->count - 1; ni >= 0; ni--) {
    const struct node *nd = &p->nodes[mc->first + ni];
    if (nd->seq >= p->nSeqs)
      continue;
    const struct frame *f = frame_at(p->frames, &p->seqs[nd->seq], t);
    if (!f || f->index >= p->nCells)
      continue;
    const struct cell *c = &p->cells[f->index];
    float ca = cosf(f->rot), sa = sinf(f->rot);
    float sx = fabsf(f->sx) < 0.01f ? 0.01f : f->sx, sy = fabsf(f->sy) < 0.01f ? 0.01f : f->sy;
    // The cell's origin, in DS pixels from the ground point, then on the destination.
    float cx = ox + (float)(nd->x + f->px + mf->px) * scale, cy = oy + (float)(nd->y + f->py + mf->py) * scale;
    for (int oi = c->count - 1; oi >= 0; oi--) {
      const struct oam *o = &p->oams[c->first + oi];
      // The OAM's box after the cell's rotation / scale: its corners, in DS pixels round the cell's origin.
      float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
      for (int k = 0; k < 4; k++) {
        float x = (float)(o->x + (k & 1 ? o->w : 0)) * sx, y = (float)(o->y + (k & 2 ? o->h : 0)) * sy;
        float rx = ca * x - sa * y, ry = sa * x + ca * y;
        minx = rx < minx ? rx : minx;
        maxx = rx > maxx ? rx : maxx;
        miny = ry < miny ? ry : miny;
        maxy = ry > maxy ? ry : maxy;
      }
      int x0 = (int)floorf(cx + minx * scale), x1 = (int)ceilf(cx + maxx * scale);
      int y0 = (int)floorf(cy + miny * scale), y1 = (int)ceilf(cy + maxy * scale);
      x0 = x0 < clip[0] ? clip[0] : x0;
      y0 = y0 < clip[1] ? clip[1] : y0;
      x1 = x1 > clip[2] ? clip[2] : x1;
      y1 = y1 > clip[3] ? clip[3] : y1;
      int across = p->sheetW / 8;
      int tileCol = o->tile % across, tileRow = o->tile / across;
      for (int y = y0; y < y1; y++) {
        float Y = ((float)y + 0.5f - cy) / scale;
        for (int x = x0; x < x1; x++) {
          float X = ((float)x + 0.5f - cx) / scale;
          float lx = (ca * X + sa * Y) / sx, ly = (-sa * X + ca * Y) / sy;
          int px = (int)floorf(lx) - o->x, py = (int)floorf(ly) - o->y;
          if (px < 0 || py < 0 || px >= o->w || py >= o->h)
            continue;
          if (o->hf)
            px = o->w - 1 - px;
          if (o->vf)
            py = o->h - 1 - py;
          int col = tileCol * 8 + px, row = tileRow * 8 + py;
          if (col >= p->sheetW || row >= p->sheetH)
            continue;
          uint8_t b = p->sheet[row * (p->sheetW / 2) + col / 2];
          int v = col & 1 ? b >> 4 : b & 15;
          if (v)
            dst[y * stride + x] = pal[v];
        }
      }
    }
  }
  return true;
}
