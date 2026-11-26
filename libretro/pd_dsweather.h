/* DS WEATHER and SEASONS (Capsule's build): over a GBA game's field, the weather of Pokémon HeartGold / SoulSilver read
 * from the player's own cart (never bundled) in place of the game's own, and Black / White's seasons: petals in spring,
 * falling leaves in autumn, a little snow in winter (drawn here: the DS games have no such particles).
 * The cart's a/0/6/3 is the field weather's archive (pret/pokeheartgold's weather_manager.c): its sprite sets (an NCGR,
 * NCLR, NCER each: rain, the heavy rain's slanted streaks, snow, the blizzard's and diamond dust's flakes, sand) and the
 * blizzard's and sandstorm's scrolling layers (NCGR, NCLR, NSCR). */
#ifndef PD_DSWEATHER_H
#define PD_DSWEATHER_H

#include <stdbool.h>
#include <stdint.h>

/* The cart at [path]: false if it isn't HeartGold / SoulSilver (any language) or can't be read. */
bool dsw_open(const char *path);
void dsw_close(void);
bool dsw_ready(void);

enum { DSW_NONE = -1, DSW_RAIN, DSW_STORM, DSW_SNOW, DSW_SAND };
enum { SEASON_NONE = -1, SEASON_SPRING, SEASON_SUMMER, SEASON_AUTUMN, SEASON_WINTER };

/* Where it draws: [plot] puts colour [rgb] (0xRRGGBB) at canvas pixel (x, y) at opacity [a] (1..256); the area is [w] x [h]
 * canvas pixels, a DS pixel [scale] canvas pixels wide. */
struct dsw_target {
  void (*plot)(int x, int y, uint32_t rgb, int a);
  int w, h, scale;
  /* How far the map scrolled since the last frame drawn, in its pixels: the particles move the other way, so they keep
   * their place in the world while the player walks. */
  float dx, dy;
};

/* One frame of [kind]'s weather (needs the cart). Particles keep going from frame to frame; another kind starts afresh. */
void dsw_weather(int kind, const struct dsw_target *t);
/* One frame of [season]'s particles (the cart's snow in winter when it's there, else drawn ones). */
void dsw_season(int season, const struct dsw_target *t);
void dsw_reset(void);

/* The season today: [mode] 1 Black / White's (a new one each month: January spring, February summer, March autumn, April
 * winter, and again), 2 the calendar's (northern: March-May spring ...), 3-6 that season always; SEASON_NONE for others
 * (0 off; 7 and 8, by play time and by area, are the core's). */
int dsw_season_now(int mode);

#endif
