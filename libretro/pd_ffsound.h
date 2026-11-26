/* FF SOUND: a Gen 3 game's music and sound effects at 1x while it runs fast, from a parked copy of the game (pd_ffsound.c). */
#ifndef PD_FFSOUND_H
#define PD_FFSOUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mCore;

/** Finds the ROM's m4a engine and makes the copy's core; false for a game without one (nothing plays). */
bool ffs_open(const void *rom, size_t len);
void ffs_close(void);
bool ffs_ready(void);
/** FF stopped (or the game jumped: a state load): the copy is made again before it next plays. */
void ffs_reset(void);
/** After each of the live game's frames while FF runs: what it started, passed on to the copy. */
void ffs_live_frame(struct mCore *live);
/** One frame of real time: the copy run a frame, its sound (stereo, 32768 Hz) into [out]; the frames written. */
int ffs_play(struct mCore *live, int16_t *out, int maxFrames);

#endif
