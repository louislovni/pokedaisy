/* FF SOUND: a Gen 3 game's music and sounds heard at 1x while the game runs fast (pd_ffsound.h).
 *
 * The m4a sound engine runs off the game's VBlank interrupt (m4aSoundMain), from song data in the ROM. A second mGBA
 * core loaded from the live game's state, its main loop parked on a `b .` with interrupts still on, goes on playing what
 * the game was playing, by itself, at the speed it's run at: once a frame of real time. Nothing in it can change song
 * (its main loop is stopped), so the live game tells it:
 *  - the background music changed (another song, stopped, paused): the copy is made again from the live game;
 *  - a sound effect or a cry started on another player (its song header changed, or its clock went back to 0): the
 *    copy's MPlayStart is called with the same player and header (a cry's header is built in RAM: copied over first).
 * GPL-3.0 (see NOTICE). */
#include "pd_ffsound.h"

#include <mgba/core/core.h>
#include <mgba/core/blip_buf.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/vfs.h>
#include <stdlib.h>
#include <string.h>

#include "pk_call.h"

#define ROM_BASE 0x08000000u
#define SOUND_INFO_PTR 0x03007FF0u
#define MUSIC_PLAYER_HEAD 0x24u /* SoundInfo.musicPlayerHead */
#define MP_HEADER 0x00u         /* MusicPlayerInfo.songHeader */
#define MP_STATUS 0x04u         /* low 16 bits: tracks playing; 0x80000000: paused */
#define MP_CLOCK 0x0Cu          /* ticks since MPlayStart */
#define MP_NEXT 0x3Cu
#define MAX_PLAYERS 12
#define RAM_HEADER 0x34 /* a cry's song: struct PokemonCrySong (its header and two tracks' commands) */
#define QUEUE 16
#define SAMPLE_RATE 32768 /* mGBA's libretro core's */

static struct {
  struct mCore *core;
  void *video;
  void *state;
  size_t stateSize;
  uint32_t mplayStart; /* MPlayStart (Thumb), 0: no m4a engine found */
  uint32_t spin;       /* a `b .` in the ROM */
  bool synced;         /* the copy plays what the live game did at its last sync */
  uint32_t bgmKey[3];  /* the BGM player's header, playing, paused at the sync */
  struct {
    uint32_t at, header, clock;
  } seen[MAX_PLAYERS];
  int nSeen;
  struct {
    uint32_t player, header;
    uint8_t ram[RAM_HEADER];
    bool inRam;
  } queue[QUEUE];
  int nQueue;
} ff;

/* ---- the ROM's m4a engine (as the Android app's M4aSongs: m4aSongNumStart by its code, its first `bl` is MPlayStart) */
struct sig {
  const char *pattern;
};
static const struct sig SIGS[] = {
    {"00b50004074a0849400b401883885900c918890089180a680168101c00f0a2fb"},
    {"00b5..4b0004400bc018828853009b18..4a9b000168d058........01bc0047"},
    {"00b5..4a03045b0b9b1899884a005218..4992008858..4a416b914202d11968........01bc0047"},
    {"000430b5000c002916d0..4d2b685a1c12d02a0000210c3206e00c3213000c3b1b6801315c1c07d08342f6d1"
     "4b005b189b000433eb1802e0..4bc000c31899884a005218..4992008858..4a416b914202d11968........30bc01bc0047"},
    {"00b50004..4a..49400b401883885900c918890089180a680168802000064140101c"},
};

static int hexv(char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; }

static long find_sig(const uint8_t *rom, size_t len, const char *pat) {
  size_t n = strlen(pat) / 2;
  for (size_t i = 0; i + n <= len; i += 2) {
    size_t j;
    for (j = 0; j < n; j++) {
      const char a = pat[2 * j], b = pat[2 * j + 1];
      const uint8_t v = rom[i + j];
      if (a != '.' && (v >> 4) != hexv(a))
        break;
      if (b != '.' && (v & 15) != hexv(b))
        break;
    }
    if (j == n)
      return (long)i;
  }
  return -1;
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void locate(const uint8_t *rom, size_t len) {
  ff.mplayStart = ff.spin = 0;
  for (size_t s = 0; s < sizeof(SIGS) / sizeof(SIGS[0]) && !ff.mplayStart; s++) {
    long at = find_sig(rom, len, SIGS[s].pattern);
    if (at < 0)
      continue;
    // The function's first `bl` (a Thumb pair: 11110 high offset, 11111 low offset).
    for (long o = at; o + 4 <= at + 0x90 && (size_t)o + 4 <= len; o += 2) {
      const uint16_t hi = u16(rom + o), lo = u16(rom + o + 2);
      if ((hi & 0xF800) != 0xF000 || (lo & 0xF800) != 0xF800)
        continue;
      int32_t off = ((int32_t)(hi & 0x7FF) << 21 >> 9) | ((lo & 0x7FF) << 1);
      const uint32_t target = ROM_BASE + (uint32_t)o + 4 + (uint32_t)off;
      if (target >= ROM_BASE && target < ROM_BASE + len)
        ff.mplayStart = target | 1;
      break;
    }
  }
  for (size_t i = 0; i + 2 <= len && ff.mplayStart; i += 2)
    if (u16(rom + i) == 0xE7FE) {
      ff.spin = ROM_BASE + (uint32_t)i;
      break;
    }
  if (!ff.spin)
    ff.mplayStart = 0;
}

bool ffs_open(const void *rom, size_t len) {
  ffs_close();
  locate(rom, len);
  if (!ff.mplayStart)
    return false;
  struct VFile *vf = VFileFromConstMemory(rom, len);
  struct mCore *core = vf ? mCoreFindVF(vf) : NULL;
  if (!core) {
    if (vf)
      vf->close(vf);
    return false;
  }
  if (!core->init(core)) {
    vf->close(vf);
    return false;
  }
  mCoreInitConfig(core, NULL);
  unsigned w, h;
  core->desiredVideoDimensions(core, &w, &h);
  ff.video = calloc((size_t)w * h, sizeof(color_t));
  core->setVideoBuffer(core, ff.video, w);
  core->setAudioBufferSize(core, 2048);
  blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), SAMPLE_RATE);
  blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), SAMPLE_RATE);
  if (!core->loadROM(core, vf)) {
    core->deinit(core);
    free(ff.video);
    ff.video = NULL;
    return false;
  }
  core->reset(core);
  // Its picture is never looked at: drawn as seldom as mGBA allows.
  ((struct GBA *)core->board)->video.frameskip = 1 << 30;
  ff.core = core;
  return true;
}

void ffs_close(void) {
  if (ff.core)
    ff.core->deinit(ff.core);
  free(ff.video);
  free(ff.state);
  memset(&ff, 0, sizeof(ff));
}

bool ffs_ready(void) { return ff.core != NULL; }

void ffs_reset(void) {
  ff.synced = false;
  ff.nQueue = 0;
}

static uint32_t r32(struct mCore *c, uint32_t a) { return c->rawRead32(c, a, -1); }
static bool in_ram(uint32_t a) { return (a >= 0x02000000u && a < 0x02040000u) || (a >= 0x03000000u && a < 0x03008000u); }

/* The players, newest first (the BGM player, opened first, is the tail): their addresses; how many. */
static int players(struct mCore *c, uint32_t *out) {
  const uint32_t info = r32(c, SOUND_INFO_PTR);
  if (!in_ram(info))
    return 0;
  int n = 0;
  for (uint32_t p = r32(c, info + MUSIC_PLAYER_HEAD); p && n < MAX_PLAYERS; p = r32(c, p + MP_NEXT)) {
    if (!in_ram(p))
      return 0;
    out[n++] = p;
  }
  return n;
}

/* Parks the copy's main loop on the `b .` once it runs with VBlank's interrupt on (as the app's pkRenderPark). */
static bool park(struct mCore *c) {
  struct ARMCore *cpu = (struct ARMCore *)c->cpu;
  for (int i = 0; i < 3000000; i++) {
    if ((cpu->privilegeMode == MODE_SYSTEM || cpu->privilegeMode == MODE_USER) && !cpu->cpsr.i && !cpu->halted &&
        (c->busRead16(c, 0x04000208) & 1) && (c->busRead16(c, 0x04000200) & 1) && (c->busRead16(c, 0x04000004) & 8)) {
      cpu->gprs[ARM_PC] = (int32_t)ff.spin;
      _ARMSetMode(cpu, MODE_THUMB);
      ThumbWritePC(cpu);
      return true;
    }
    c->step(c);
  }
  return false;
}

static void bgm_key(struct mCore *live, uint32_t key[3]) {
  uint32_t p[MAX_PLAYERS];
  const int n = players(live, p);
  key[0] = key[1] = key[2] = 0;
  if (!n)
    return;
  const uint32_t status = r32(live, p[n - 1] + MP_STATUS);
  key[0] = r32(live, p[n - 1] + MP_HEADER);
  key[1] = (status & 0xFFFF) != 0;
  key[2] = (status & 0x80000000u) != 0;
}

static bool resync(struct mCore *live) {
  const size_t size = live->stateSize(live);
  if (size != ff.stateSize) {
    free(ff.state);
    ff.state = malloc(size);
    ff.stateSize = ff.state ? size : 0;
  }
  if (!ff.state || !live->saveState(live, ff.state) || !ff.core->loadState(ff.core, ff.state))
    return false;
  ((struct GBA *)ff.core->board)->video.frameskip = 1 << 30;
  // What the copy played before is gone: drop what it had queued and its leftover sound.
  blip_clear(ff.core->getAudioChannel(ff.core, 0));
  blip_clear(ff.core->getAudioChannel(ff.core, 1));
  ff.nQueue = 0;
  if (!park(ff.core))
    return false;
  bgm_key(live, ff.bgmKey);
  // The other players as they stand now: only what starts from here on is passed on.
  uint32_t p[MAX_PLAYERS];
  ff.nSeen = players(live, p);
  for (int i = 0; i < ff.nSeen; i++) {
    ff.seen[i].at = p[i];
    ff.seen[i].header = r32(live, p[i] + MP_HEADER);
    ff.seen[i].clock = r32(live, p[i] + MP_CLOCK);
  }
  return true;
}

void ffs_live_frame(struct mCore *live) {
  if (!ff.core || !ff.synced)
    return;
  uint32_t key[3];
  bgm_key(live, key);
  if (memcmp(key, ff.bgmKey, sizeof(key))) {
    ff.synced = false; // made again before the copy's next frame
    return;
  }
  uint32_t p[MAX_PLAYERS];
  const int n = players(live, p);
  if (n != ff.nSeen) {
    ff.synced = false;
    return;
  }
  for (int i = 0; i < n - 1; i++) { // not the BGM player (the tail)
    const uint32_t header = r32(live, p[i] + MP_HEADER), clock = r32(live, p[i] + MP_CLOCK);
    const uint32_t status = r32(live, p[i] + MP_STATUS);
    // MPlayStart sets the header and the clock to 0; the tracks count as playing only once the sound engine has run
    // them (the next VBlank, after this frame's end).
    const bool started = (header != ff.seen[i].header || clock < ff.seen[i].clock) && !(status & 0x80000000u);
    ff.seen[i].at = p[i];
    ff.seen[i].header = header;
    ff.seen[i].clock = clock;
    if (!started || ff.nQueue >= QUEUE)
      continue;
    const bool ram = in_ram(header);
    if (!ram && (header < ROM_BASE || header >= 0x0A000000u))
      continue;
    // Started again within the same batch of fast frames: the latest start replaces the queued one.
    int q;
    for (q = 0; q < ff.nQueue && ff.queue[q].player != p[i]; q++) {
    }
    if (q == ff.nQueue)
      ff.nQueue++;
    ff.queue[q].player = p[i];
    ff.queue[q].header = header;
    ff.queue[q].inRam = ram;
    if (ram)
      for (int k = 0; k < RAM_HEADER; k++)
        ff.queue[q].ram[k] = (uint8_t)live->rawRead8(live, header + k, -1);
  }
}

int ffs_play(struct mCore *live, int16_t *out, int maxFrames) {
  if (!ff.core)
    return 0;
  if (!ff.synced) {
    ff.synced = resync(live);
    if (!ff.synced)
      return 0;
  }
  struct mCore *c = ff.core;
  for (int q = 0; q < ff.nQueue; q++) {
    if (ff.queue[q].inRam)
      for (int k = 0; k < RAM_HEADER; k++)
        c->rawWrite8(c, ff.queue[q].header + k, -1, ff.queue[q].ram[k]);
    const uint32_t args[4] = {ff.queue[q].player, ff.queue[q].header, 0, 0};
    pk_call(c, ff.mplayStart, args, NULL);
  }
  ff.nQueue = 0;
  c->runFrame(c);
  blip_t *l = c->getAudioChannel(c, 0), *r = c->getAudioChannel(c, 1);
  int avail = blip_samples_avail(l);
  if (avail > maxFrames)
    avail = maxFrames;
  if (avail <= 0)
    return 0;
  blip_read_samples(l, out, avail, 1);
  blip_read_samples(r, out + 1, avail, 1);
  return avail;
}
