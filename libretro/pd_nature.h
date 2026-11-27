#ifndef PD_NATURE_H
#define PD_NATURE_H
/* NATURE (Capsule's build, retro_pd_nature): what the world does by itself, none of it the game's own - fireflies on a
 * summer night, a shooting star now and then on a clear night, an autumn morning's mist, a sunrise's pink and a sunset's
 * orange glow over the sky, splashes at the player's feet in DYNAMIC WEATHER's rain and footprints in
 * its snow (the device's clock and the SEASONS' season, else the calendar's); heard: the rain and thunder, birds on a
 * fine morning, crickets on a summer or autumn night, the wind in the snow (all made here: noise and tones). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nat_target {
  void (*plot)(int x, int y, uint32_t rgb, int a); /* colour [rgb] at (x, y) of the w x h picture, opacity [a] 1..256 */
  int w, h, scale;                                 /* a game pixel is [scale] picture pixels */
  float camX, camY;                                /* the map's scroll since the last frame, in picture pixels */
  float feetX, feetY;                              /* the player's feet in the picture */
};
void nat_set_on(bool on);
/* After each frame: [outdoors] on the field outside; [clear] and no weather; the SEASONS' [season] (SEASON_NONE: the
 * calendar's); [rain] 0 / 1 rain / 2 storm, [snow]: DYNAMIC WEATHER's. */
void nat_after_run(bool outdoors, bool clear, int season, int rain, bool snow);
void nat_draw(const struct nat_target *t);
/* A lightning flash: its thunder heard a moment later. */
void nat_thunder(void);
/* Mixed into the game's sound (stereo, 32768 Hz). */
void nat_mix(int16_t *buf, size_t frames);

#ifdef __cplusplus
}
#endif
#endif
