/* NATURE, shared by Capsule's GBA core (pd_libretro.c) and DS core (pd_ds_libretro.cpp's sky). */
#include "pd_nature.h"
#include "pd_dsweather.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void (*plot)(int x, int y, uint32_t rgb, int a);
static int CW = 240, CH = 160;
static float feetX, feetY;

static bool natureOn = true;
void nat_set_on(bool on) { natureOn = on; }
static int natureRain;                       // DYNAMIC WEATHER's rain heard: 0 none, 1 rain, 2 storm
static bool natureSnow;                      // DYNAMIC WEATHER's snow: the wind heard, footprints
static int glowK, glowRgb;                   // sunrise / sunset glow (eased 0..256) and its colour
static float birdsWant, cricketsWant, windWant; // what's heard: 0..1
static int flyK, starOk, mistK;              // fireflies / stars (eased 0..256) / mist strength
static uint32_t natureSeed = 0x2545F491u;
static uint32_t nature_rand(void) {
  natureSeed ^= natureSeed << 13;
  natureSeed ^= natureSeed >> 17;
  natureSeed ^= natureSeed << 5;
  return natureSeed;
}
static float nature_hour(void) {
  time_t now = time(NULL);
  struct tm tmv;
#ifdef _WIN32
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  return tmv.tm_hour + tmv.tm_min / 60.f;
}
static int nature_season(int season) {
  if (season >= 0)
    return season;
  time_t now = time(NULL);
  struct tm tmv;
#ifdef _WIN32
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  const int m = tmv.tm_mon; // Mar-May spring, Jun-Aug summer, Sep-Nov autumn, Dec-Feb winter
  return m >= 2 && m <= 4 ? SEASON_SPRING : m >= 5 && m <= 7 ? SEASON_SUMMER : m >= 8 && m <= 10 ? SEASON_AUTUMN : SEASON_WINTER;
}
void nat_after_run(bool outdoors, bool clear, int season, int rain, bool snow) {
  natureRain = natureOn && outdoors ? rain : 0;
  natureSnow = natureOn && outdoors && snow;
  const float h = nature_hour();
  const int s = nature_season(season);
  // Sunrise (5:15-7:15, at its height at 6:00) pink; sunset (17:15-19:30, at 18:30) orange.
  int glow = 0;
  if (natureOn && clear) {
    if (h >= 5.25f && h < 7.25f)
      glow = (int)(256 * (h < 6.f ? (h - 5.25f) / 0.75f : (7.25f - h) / 1.25f)), glowRgb = 0xFF8FB0;
    else if (h >= 17.25f && h < 19.5f)
      glow = (int)(256 * (h < 18.5f ? (h - 17.25f) / 1.25f : (19.5f - h) / 1.f)), glowRgb = 0xFF8A3A;
  }
  glowK += glow > glowK ? 2 : glow < glowK ? -4 : 0;
  if (glowK < 0)
    glowK = 0;
  // Heard: birds on a fine morning (5:30-10:30, fewer in winter), crickets on a summer or early autumn night (20:30-4:00),
  // the wind in the snow.
  birdsWant = natureOn && clear && h >= 5.5f && h < 10.5f ? (s == SEASON_WINTER ? 0.3f : 1.f) : 0.f;
  cricketsWant = natureOn && clear && (h >= 20.5f || h < 4.f) && (s == SEASON_SUMMER || s == SEASON_AUTUMN) ? 1.f : 0.f;
  windWant = natureSnow ? 1.f : natureRain == 2 ? 0.5f : 0.f;
  const bool night = h >= 21.5f || h < 4.5f;
  const int flyWant = natureOn && clear && night && s == SEASON_SUMMER ? 256 : 0;
  flyK += flyWant > flyK ? 4 : flyWant < flyK ? -8 : 0;
  starOk = natureOn && clear && (h >= 21.f || h < 5.f);
  // Mist: autumn mornings, thickest 6:30-7:30, gone by 9:30.
  int mist = 0;
  if (natureOn && clear && s == SEASON_AUTUMN && h >= 5.f && h < 9.5f)
    mist = h < 6.5f ? (int)((h - 5.f) / 1.5f * 256) : h < 7.5f ? 256 : (int)((9.5f - h) / 2.f * 256);
  mistK += mist > mistK ? 4 : mist < mistK ? -6 : 0;
  if (mistK < 0)
    mistK = 0;
}

static struct { float x, y, vx, vy, phase, speed; } flies[16];
static bool fliesPlaced;
static struct { float x, y, dx, dy; int life; } star;
static int starWait = 600;
static uint32_t natureTick;

static void nature_glow(int cx, int cy, int r, uint32_t rgb, int a) {
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++) {
      const int d2 = x * x + y * y;
      if (d2 > r * r)
        continue;
      plot(cx + x, cy + y, rgb, a * (r * r - d2) / (r * r));
    }
}
static void nature_mist(int scale) {
  // A pale veil, thicker and thinner in slow drifting patches: 8-pixel blocks (in game pixels), each its own alpha.
  const float t = natureTick * 0.004f;
  const int b = 8 * scale;
  for (int by = 0; by < CH; by += b)
    for (int bx = 0; bx < CW; bx += b) {
      const float n = (sinf(bx * 0.004f / scale * 3 + t) + sinf(by * 0.005f / scale * 3 - t * 0.7f) +
                       sinf((bx + by) * 0.003f / scale * 3 + t * 0.4f)) / 6.f + 0.5f;
      const int a = (int)(mistK * (0.04f + 0.30f * n * n));
      if (a <= 2)
        continue;
      for (int y = by; y < by + b && y < CH; y++)
        for (int x = bx; x < bx + b && x < CW; x++)
          plot(x, y, 0xD8DEE4, a);
    }
}
/* The player's steps (the map scrolling: a tile walked) in the rain: a splash at the feet; in the snow: a footprint left on
 * the map (carried with it, fading in ~5 s), at the feet the core gives (nat_target). */
static struct { float x, y; int age, kind, side; } steps[24];
static float stepWalked;
static int stepSide;
static void nature_steps(float camX, float camY, int scale) {
  for (size_t i = 0; i < sizeof steps / sizeof *steps; i++)
    if (steps[i].age > 0)
      steps[i].x -= camX, steps[i].y -= camY;
  const float moved = fabsf(camX) + fabsf(camY);
  if ((natureRain || natureSnow) && moved > 0 && moved < 8.f * scale) {
    stepWalked += moved;
    if (stepWalked >= 8.f * scale) { // each half tile: a foot
      stepWalked = 0;
      size_t k = 0;
      for (size_t i = 1; i < sizeof steps / sizeof *steps; i++)
        if (steps[i].age == 0 || (steps[k].age && steps[i].age > steps[k].age))
          k = i;
      const float sideways = fabsf(camY) > fabsf(camX) ? 1.f : 0.f; // walking up/down: feet side by side
      stepSide ^= 1;
      steps[k].x = feetX + (stepSide ? 2.f : -2.f) * scale * sideways;
      steps[k].y = feetY + (stepSide ? 1.f : -1.f) * scale * (1.f - sideways);
      steps[k].age = 1;
      steps[k].kind = natureSnow ? 1 : 0;
      steps[k].side = stepSide;
    }
  } else if (!moved)
    stepWalked = 0;
  for (size_t i = 0; i < sizeof steps / sizeof *steps; i++) {
    if (!steps[i].age)
      continue;
    const int age = steps[i].age++;
    const int cx = (int)steps[i].x, cy = (int)steps[i].y;
    if (steps[i].kind == 0) { // a splash: a ring growing and going, four drops thrown out
      if (age > 16) {
        steps[i].age = 0;
        continue;
      }
      const float r = (1.5f + age * 0.35f) * scale;
      const int a = 200 - age * 12;
      for (int d = 0; d < 20; d++) {
        const float t = d * 6.2832f / 20;
        plot(cx + (int)(cosf(t) * r), cy + (int)(sinf(t) * r * 0.45f), 0xE8F2FF, a);
      }
      for (int d = 0; d < 4; d++) {
        const float t = (d * 2 + 1) * 0.785f, up = age * 0.6f * scale - age * age * 0.05f * scale;
        plot(cx + (int)(cosf(t) * age * 0.5f * scale), cy - (int)up, 0xF4F8FF, a);
      }
    } else { // a footprint: a small sole pressed in, pale, fading
      if (age > 300) {
        steps[i].age = 0;
        continue;
      }
      const int a = age < 240 ? 150 : 150 * (300 - age) / 60;
      for (int y = -scale; y <= scale; y++)
        for (int x = -scale / 2 - 1; x <= scale / 2; x++)
          plot(cx + x, cy + y, 0xF2F6FF, a);
    }
  }
}
/* Sunrise and sunset: the sky's colour over the picture's top, gone by its middle. */
static void nature_glow_sky(void) {
  const int rows = CH * 3 / 5;
  for (int y = 0; y < rows; y++) {
    const float f = 1.f - (float)y / rows;
    const int a = (int)(glowK * f * f * 0.40f);
    if (a <= 1)
      break;
    for (int x = 0; x < CW; x++)
      plot(x, y, (uint32_t)glowRgb, a);
  }
}
void nat_draw(const struct nat_target *t) {
  plot = t->plot;
  CW = t->w;
  CH = t->h;
  feetX = t->feetX;
  feetY = t->feetY;
  const float camX = t->camX, camY = t->camY;
  const int scale = t->scale;
  natureTick++;
  if (glowK > 0)
    nature_glow_sky();
  nature_steps(camX, camY, scale);
  if (mistK > 0)
    nature_mist(scale);
  if (flyK > 0) {
    if (!fliesPlaced) {
      for (size_t i = 0; i < sizeof flies / sizeof *flies; i++) {
        flies[i].x = (float)(nature_rand() % (unsigned)CW);
        flies[i].y = (float)(nature_rand() % (unsigned)CH);
        flies[i].phase = (nature_rand() % 628) / 100.f;
        flies[i].speed = 0.03f + (nature_rand() % 40) / 1000.f;
      }
      fliesPlaced = true;
    }
    for (size_t i = 0; i < sizeof flies / sizeof *flies; i++) {
      // A slow wander (a nudge now and then), carried with the map, round the picture's edges.
      if (nature_rand() % 40 == 0) {
        flies[i].vx = ((int)(nature_rand() % 200) - 100) / 400.f * scale;
        flies[i].vy = ((int)(nature_rand() % 200) - 100) / 500.f * scale;
      }
      flies[i].x += flies[i].vx - camX;
      flies[i].y += flies[i].vy - camY;
      if (flies[i].x < -8) flies[i].x += CW + 16;
      if (flies[i].x > CW + 8) flies[i].x -= CW + 16;
      if (flies[i].y < -8) flies[i].y += CH + 16;
      if (flies[i].y > CH + 8) flies[i].y -= CH + 16;
      flies[i].phase += flies[i].speed;
      const float glow = sinf(flies[i].phase);
      if (glow <= 0)
        continue;
      const int a = (int)(glow * flyK);
      nature_glow((int)flies[i].x, (int)flies[i].y, 4 * scale, 0xC8FF6A, a / 2);
      nature_glow((int)flies[i].x, (int)flies[i].y, scale * 3 / 2, 0xF8FFD0, a);
    }
  } else
    fliesPlaced = false;
  // A shooting star: every 20-70 s of a clear night, across the picture's top half in under a second.
  if (star.life > 0) {
    star.x += star.dx;
    star.y += star.dy;
    const int len = 26;
    for (int k = 0; k < len; k++) {
      const int a = (256 - k * 256 / len) * (star.life > 8 ? 256 : star.life * 32) / 256;
      const int px = (int)(star.x - star.dx * k / 3.f), py = (int)(star.y - star.dy * k / 3.f);
      for (int w = 0; w < scale; w++)
        for (int v = 0; v < scale; v++)
          plot(px + v, py + w, 0xFFFFF0, a);
    }
    star.life--;
  } else if (starOk && --starWait <= 0) {
    starWait = 1200 + (int)(nature_rand() % 3000u);
    star.x = (float)(nature_rand() % (unsigned)(CW * 2 / 3));
    star.y = (float)(nature_rand() % (unsigned)(CH / 4));
    star.dx = (2.5f + (nature_rand() % 100) / 50.f) * scale;
    star.dy = star.dx * (0.35f + (nature_rand() % 30) / 100.f);
    star.life = 26;
  } else if (!starOk)
    starWait = starWait < 600 ? 600 : starWait;
}

/* The rain and thunder heard: made from noise at the core's rate (32768 Hz), mixed into the game's sound. */
static float rainLevel, thunderEnv, thunderRumble, rainLp[2], rainBp[2], dropLp[2], brown[2], thunderLp;
static int thunderIn = -1; // samples until the thunder of the last flash
void nat_thunder(void) {
  if (natureRain)
    thunderIn = 8000 + (int)(nature_rand() % 50000u); // 0.25-1.8 s after the flash
}
static float nature_noise(void) { return (int32_t)nature_rand() / 2147483648.f; }
/* Birds: phrases of 2-7 chirps (a quick rising or falling whistle, 40-120 ms), one bird or another (pitch, place);
 * crickets: two, a 4.2-4.7 kHz tone in pulses (three or four to a chirp, a chirp every ~0.6 s); the wind: low-passed noise
 * in slow gusts. */
static float birdsLevel, cricketsLevel, windLevel, windGust, windGustTo, windLp[2], windLp2[2];
static struct { float phase, f0, f1, pan, amp; int len, at, left, gap; } bird;
static struct { float phase, freq, pan; int period, at, pulses; } crickets[2] = {{0, 4300, 0.3f, 19000, 0, 3}, {0, 4650, 0.75f, 23500, 9000, 4}};
static void nature_life(float out[2]) {
  birdsLevel += (birdsWant - birdsLevel) * 0.00002f;
  cricketsLevel += (cricketsWant - cricketsLevel) * 0.00002f;
  windLevel += (windWant - windLevel) * 0.00002f;
  if (birdsLevel > 0.001f) {
    if (bird.at >= bird.len) { // between chirps
      if (bird.gap > 0)
        bird.gap--;
      else if (bird.left > 0 || nature_rand() % 60000u < 1u) {
        if (bird.left <= 0) { // a new phrase: who sings, how many
          bird.left = 2 + (int)(nature_rand() % 6u);
          bird.pan = (nature_rand() % 100) / 100.f;
          bird.amp = 0.5f + (nature_rand() % 50) / 100.f;
          bird.f0 = 2600.f + (float)(nature_rand() % 1800u);
        }
        bird.left--;
        const float spread = 600.f + (float)(nature_rand() % 1400u);
        bird.f1 = nature_rand() & 1 ? bird.f0 + spread : bird.f0 - spread * 0.5f;
        bird.len = 1300 + (int)(nature_rand() % 2600u);
        bird.at = 0;
        bird.gap = 1500 + (int)(nature_rand() % 5000u);
      }
    } else {
      const float t = (float)bird.at / bird.len;
      const float f = bird.f0 + (bird.f1 - bird.f0) * t * t;
      bird.phase += f * 6.2832f / 32768.f;
      if (bird.phase > 6.2832f)
        bird.phase -= 6.2832f;
      const float env = sinf(3.1416f * t) * bird.amp * birdsLevel * 1700.f;
      const float v = (sinf(bird.phase) + 0.18f * sinf(bird.phase * 2.f)) * env;
      out[0] += v * (1.f - bird.pan) * 1.4f;
      out[1] += v * bird.pan * 1.4f;
      bird.at++;
    }
  }
  if (cricketsLevel > 0.001f)
    for (int c = 0; c < 2; c++) {
      crickets[c].phase += crickets[c].freq * 6.2832f / 32768.f;
      if (crickets[c].phase > 6.2832f)
        crickets[c].phase -= 6.2832f;
      if (++crickets[c].at >= crickets[c].period)
        crickets[c].at = 0;
      const int pulse = crickets[c].at / 1100; // ~30 ms a pulse, half of it sounding
      if (pulse < crickets[c].pulses && crickets[c].at % 1100 < 600) {
        const float e = sinf(3.1416f * (crickets[c].at % 1100) / 600.f);
        const float v = sinf(crickets[c].phase) * e * cricketsLevel * 1100.f;
        out[0] += v * (1.f - crickets[c].pan);
        out[1] += v * crickets[c].pan;
      }
    }
  if (windLevel > 0.001f) {
    if (nature_rand() % 40000u == 0)
      windGustTo = 0.25f + (nature_rand() % 75) / 100.f;
    windGust += (windGustTo - windGust) * 0.00004f;
    for (int c = 0; c < 2; c++) {
      windLp[c] += (nature_noise() - windLp[c]) * (0.01f + windGust * 0.03f);
      windLp2[c] += (windLp[c] - windLp2[c]) * 0.05f;
      out[c] += windLp2[c] * windLevel * windGust * 26000.f;
    }
  }
}
void nat_mix(int16_t *buf, size_t frames) {
  const float want = natureRain == 2 ? 1.f : natureRain == 1 ? 0.6f : 0.f;
  const bool life = birdsWant > 0 || cricketsWant > 0 || windWant > 0 || birdsLevel > 0.001f || cricketsLevel > 0.001f ||
                    windLevel > 0.001f;
  if (want == 0.f && rainLevel < 0.001f && thunderEnv < 0.001f && thunderIn < 0 && !life)
    return;
  for (size_t i = 0; i < frames; i++) {
    rainLevel += (want - rainLevel) * 0.00003f; // ~1 s to come and go
    if (thunderIn >= 0 && --thunderIn < 0)
      thunderEnv = 0.7f + (nature_rand() % 30) / 100.f;
    thunderEnv *= 0.99996f;                     // ~2 s to die away
    thunderRumble += (nature_noise() - thunderRumble) * 0.0004f;
    float out[2];
    for (int c = 0; c < 2; c++) {
      const float n = nature_noise();
      rainLp[c] += (n - rainLp[c]) * 0.35f;
      rainBp[c] += (rainLp[c] - rainBp[c]) * 0.04f;
      float rain = (rainLp[c] - rainBp[c]) * 0.55f;
      if (nature_rand() % 1400 == 0)
        dropLp[c] += nature_noise() * 3.f; // a drop
      dropLp[c] *= 0.93f;
      rain += dropLp[c] * 0.25f;
      brown[c] = brown[c] * 0.996f + nature_noise() * 0.06f;
      thunderLp += (brown[c] - thunderLp) * 0.02f;
      const float thunder = thunderLp * thunderEnv * (0.55f + 2.2f * fabsf(thunderRumble));
      out[c] = rain * rainLevel * 8000.f + thunder * 30000.f;
    }
    if (life)
      nature_life(out);
    for (int c = 0; c < 2; c++) {
      const float v = buf[i * 2 + c] + out[c];
      buf[i * 2 + c] = (int16_t)(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v);
    }
  }
}
