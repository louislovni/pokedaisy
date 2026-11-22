/* BATTLE SPRITES (Capsule's build): the animated battle pictures of Pokémon Black / White / Black 2 / White 2, read from
 * the player's own DS cart (never bundled) and drawn in a GBA game's battles in place of its still pictures.
 * The cart's a/0/0/4 holds 20 files a species (national number x 20): an NCGR sheet of parts, NCER cells, NANR cell
 * animations, an NMCR of multi-cells (parts placed) and the NMAR that plays them, for the front (+2..+7) and the back
 * (+11..+16), and the normal / shiny NCLR (+18 / +19). */
#ifndef PD_BWSPRITE_H
#define PD_BWSPRITE_H

#include <stdbool.h>
#include <stdint.h>

/* The cart at [path] (any language): false if it isn't one of the four or can't be read. Replaces the one before. */
bool bws_open(const char *path);
void bws_close(void);
bool bws_ready(void);

/* [national]'s picture ([back], [shiny]) at frame [t] of its idle animation, drawn into [dst] (XRGB, [stride] pixels a
 * row, clipped to [clip] x0 y0 x1 y1) with its ground point at ([ox], [oy]) and a DS pixel [scale] destination pixels
 * wide. False if the cart has no such picture. */
bool bws_draw(int national, bool back, bool shiny, int t, uint32_t *dst, int stride, const int clip[4], float ox, float oy,
              float scale);

/* How far up [national]'s picture reaches from its ground point at rest (DS pixels, <= 0); 0 if none. */
int bws_top(int national, bool back);

#endif
