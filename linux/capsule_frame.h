/* Capsule's frame (POCKET_PURE builds, the host menu's CAPSULE FRAME): in place of black bars, the margins round the
 * game take a soft pastel gradient drawn from the game's own colours (as Capsule's controller skin on iPhone), and the
 * game sits in a dark rounded bezel lifted by a shade. The colours follow the picture slowly; the textures are tiny and
 * stretched with linear filtering, so it costs next to nothing a frame. Included by main.c only. */
#ifndef CAPSULE_FRAME_H
#define CAPSULE_FRAME_H

#include <math.h>

static struct {
  float r, g, b;             /* the picture's average colour (0..1), what the gradient eases towards */
  float nr, ng, nb;          /* the colour drawn now */
  bool seen;                 /* a colour has been read */
  unsigned frames;
  SDL_Texture *gradient;     /* 2x2: the four pastel stops, stretched over the screen */
  SDL_Texture *disc;         /* a smooth white disc: the bezel's and shade's corners */
  SDL_Texture *glow;         /* a white radial glow, lower right (as the iPhone card's) */
} capFrame = {0.55f, 0.6f, 0.58f, 0.55f, 0.6f, 0.58f, false, 0, NULL, NULL, NULL};

/* The frame's colour, from a 16 x 16 grid of the picture's pixels (near-black ones left out), every 30 frames. */
static void capsule_frame_sample(const void *data, unsigned w, unsigned h, size_t pitch, enum retro_pixel_format fmt) {
  if (!data || !w || !h || capFrame.frames++ % 30)
    return;
  long r = 0, g = 0, b = 0, n = 0;
  for (unsigned j = 0; j < 16; j++)
    for (unsigned i = 0; i < 16; i++) {
      unsigned x = (2 * i + 1) * w / 32, y = (2 * j + 1) * h / 32;
      const uint8_t *row = (const uint8_t *)data + y * pitch;
      int pr, pg, pb;
      if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
        uint32_t p = ((const uint32_t *)row)[x];
        pr = (p >> 16) & 0xFF, pg = (p >> 8) & 0xFF, pb = p & 0xFF;
      } else if (fmt == RETRO_PIXEL_FORMAT_RGB565) {
        uint16_t p = ((const uint16_t *)row)[x];
        pr = ((p >> 11) & 31) * 255 / 31, pg = ((p >> 5) & 63) * 255 / 63, pb = (p & 31) * 255 / 31;
      } else {
        uint16_t p = ((const uint16_t *)row)[x];
        pr = ((p >> 10) & 31) * 255 / 31, pg = ((p >> 5) & 31) * 255 / 31, pb = (p & 31) * 255 / 31;
      }
      if (pr + pg + pb < 60)
        continue;
      r += pr, g += pg, b += pb, n++;
    }
  if (!n)
    return;
  capFrame.r = r / (255.0f * n), capFrame.g = g / (255.0f * n), capFrame.b = b / (255.0f * n);
  if (!capFrame.seen) {
    capFrame.nr = capFrame.r, capFrame.ng = capFrame.g, capFrame.nb = capFrame.b;
    capFrame.seen = true;
  }
}

static void capsule_rgb_to_hsv(float r, float g, float b, float *h, float *s, float *v) {
  float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
  *v = mx;
  *s = mx > 0 ? d / mx : 0;
  if (d <= 0) {
    *h = 0;
    return;
  }
  float hh = mx == r ? (g - b) / d : mx == g ? 2 + (b - r) / d : 4 + (r - g) / d;
  hh /= 6;
  *h = hh < 0 ? hh + 1 : hh;
}

/* A pastel near hue [h] (the iPhone card's CardColors.stops), as 0xRRGGBB. */
static uint32_t capsule_pastel(float h, float s, float light) {
  h -= floorf(h);
  float v = 0.82f, c = v * s, x = c * (1 - fabsf(fmodf(h * 6, 2) - 1)), m = v - c, r, g, b;
  int seg = (int)(h * 6) % 6;
  r = seg == 0 || seg == 5 ? c : seg == 1 || seg == 4 ? x : 0;
  g = seg == 1 || seg == 2 ? c : seg == 0 || seg == 3 ? x : 0;
  b = seg == 3 || seg == 4 ? c : seg == 2 || seg == 5 ? x : 0;
  r += m, g += m, b += m;
  r += (1 - r) * light, g += (1 - g) * light, b += (1 - b) * light;
  return (uint32_t)(r * 255 + 0.5f) << 16 | (uint32_t)(g * 255 + 0.5f) << 8 | (uint32_t)(b * 255 + 0.5f);
}

static SDL_Texture *capsule_texture(SDL_Renderer *ren, int w, int h, const uint32_t *px) {
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); /* stretched smoothly (the game's own texture stays sharp) */
  SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  if (t) {
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  }
  return t;
}

/* A rounded rectangle in [color] at [alpha]: the disc's quarters at the corners, rectangles between. */
static void capsule_round_rect(SDL_Renderer *ren, SDL_Rect r, int radius, uint32_t color, uint8_t alpha) {
  if (radius * 2 > r.w)
    radius = r.w / 2;
  if (radius * 2 > r.h)
    radius = r.h / 2;
  uint8_t cr = color >> 16, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(ren, cr, cg, cb, alpha);
  SDL_Rect mid = {r.x + radius, r.y, r.w - 2 * radius, r.h};
  SDL_Rect left = {r.x, r.y + radius, radius, r.h - 2 * radius};
  SDL_Rect right = {r.x + r.w - radius, r.y + radius, radius, r.h - 2 * radius};
  SDL_RenderFillRect(ren, &mid);
  SDL_RenderFillRect(ren, &left);
  SDL_RenderFillRect(ren, &right);
  if (!capFrame.disc || radius <= 0)
    return;
  SDL_SetTextureColorMod(capFrame.disc, cr, cg, cb);
  SDL_SetTextureAlphaMod(capFrame.disc, alpha);
  const int half = 32;
  SDL_Rect src[4] = {{0, 0, half, half}, {half, 0, half, half}, {0, half, half, half}, {half, half, half, half}};
  SDL_Rect dst[4] = {{r.x, r.y, radius, radius}, {r.x + r.w - radius, r.y, radius, radius},
                     {r.x, r.y + r.h - radius, radius, radius}, {r.x + r.w - radius, r.y + r.h - radius, radius, radius}};
  for (int i = 0; i < 4; i++)
    SDL_RenderCopy(ren, capFrame.disc, &src[i], &dst[i]);
}

/* The frame round [game] on an [ow] x [oh] output: drawn before the game's picture. */
static void capsule_frame_draw(SDL_Renderer *ren, SDL_Rect game, int ow, int oh) {
  if (!capFrame.disc) {
    static uint32_t px[64 * 64];
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 64; x++) {
        float dx = x + 0.5f - 32, dy = y + 0.5f - 32, d = sqrtf(dx * dx + dy * dy);
        float a = d < 31.5f ? 1 : d < 32.5f ? 32.5f - d : 0; /* full to the edge, so the arcs meet the sides */
        px[y * 64 + x] = (uint32_t)(a * 255 + 0.5f) << 24 | 0xFFFFFF;
      }
    capFrame.disc = capsule_texture(ren, 64, 64, px);
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 64; x++) {
        float dx = x + 0.5f - 32, dy = y + 0.5f - 32, d = sqrtf(dx * dx + dy * dy) / 32;
        float a = d < 1 ? 0.34f * (1 - d) * (1 - d) : 0;
        px[y * 64 + x] = (uint32_t)(a * 255 + 0.5f) << 24 | 0xFFFFFF;
      }
    capFrame.glow = capsule_texture(ren, 64, 64, px);
  }
  /* Ease towards the picture's colour (a few seconds for a whole change). */
  capFrame.nr += (capFrame.r - capFrame.nr) * 0.03f;
  capFrame.ng += (capFrame.g - capFrame.ng) * 0.03f;
  capFrame.nb += (capFrame.b - capFrame.nb) * 0.03f;
  float h, s, v;
  capsule_rgb_to_hsv(capFrame.nr, capFrame.ng, capFrame.nb, &h, &s, &v);
  s = s < 0.18f ? 0.18f : s > 0.5f ? 0.5f : s;
  uint32_t stops[4] = {capsule_pastel(h - 0.04f, s, 0.3f), capsule_pastel(h + 0.03f, s, 0.5f),
                       capsule_pastel(h + 0.12f, s, 0.42f), capsule_pastel(h, s, 0.36f)};
  uint32_t grid[4] = {0xFF000000u | stops[0], 0xFF000000u | stops[1], 0xFF000000u | stops[3], 0xFF000000u | stops[2]};
  if (!capFrame.gradient)
    capFrame.gradient = capsule_texture(ren, 2, 2, grid);
  else
    SDL_UpdateTexture(capFrame.gradient, NULL, grid, 8);
  if (capFrame.gradient) {
    /* The texels' centres at the corners: the half texel round the edge cut off, so the corners are the stops. */
    SDL_Rect src = {0, 0, 2, 2};
    SDL_Rect dst = {-ow / 2, -oh / 2, ow * 2, oh * 2};
    SDL_RenderCopy(ren, capFrame.gradient, &src, &dst);
  }
  if (capFrame.glow) {
    int rad = (ow < oh ? ow : oh) * 6 / 10;
    SDL_Rect g = {ow * 8 / 10 - rad, oh * 3 / 4 - rad, 2 * rad, 2 * rad};
    SDL_RenderCopy(ren, capFrame.glow, NULL, &g);
  }
  /* The bezel: Capsule's near-black, a rim round the picture, rounded; under it a soft shade, a little lower. */
  int pad = game.h / 28 > 6 ? game.h / 28 : 6;
  int radius = pad * 3;
  SDL_Rect bezel = {game.x - pad, game.y - pad, game.w + 2 * pad, game.h + 2 * pad};
  /* Many faint layers, each a little smaller and lower: a soft shade with no visible steps. */
  for (int i = 12; i >= 1; i--) {
    int k = i * pad / 6;
    SDL_Rect shade = {bezel.x - k, bezel.y - k + pad * 2 / 3, bezel.w + 2 * k, bezel.h + 2 * k};
    capsule_round_rect(ren, shade, radius + k, 0x18241D, 6);
  }
  capsule_round_rect(ren, bezel, radius, 0x0B0A09, 255);
  SDL_Rect inner = {game.x - 2, game.y - 2, game.w + 4, game.h + 4};
  capsule_round_rect(ren, inner, 4, 0x2A2826, 255);
}

static void capsule_frame_free(void) {
  if (capFrame.gradient)
    SDL_DestroyTexture(capFrame.gradient);
  if (capFrame.disc)
    SDL_DestroyTexture(capFrame.disc);
  if (capFrame.glow)
    SDL_DestroyTexture(capFrame.glow);
  capFrame.gradient = capFrame.disc = capFrame.glow = NULL;
}

#endif
