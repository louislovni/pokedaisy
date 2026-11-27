/* Co-op presence in the libretro core (Capsule's iPhone / PC builds; the app's is CoopPresence.kt): the two players see
 * each other's hero on the map. Included by pd_libretro.c (its `core`, `romData` and field state), mGBA only.
 *
 * The frontend owns the connection and calls, on the thread that runs the frames:
 *   retro_pd_coop_frame(now, out)      after each retro_run: 1 = `out` holds an update (PD_COOP_PRESENCE_SIZE bytes) to send
 *   retro_pd_coop_receive(msg, n, now) an update the partner sent
 *   retro_pd_coop_stop()               the link is gone: nothing drawn, nothing kept
 *   retro_pd_coop_available()          1 = the loaded game can (Emerald, FireRed / LeafGreen, any Western language, its
 *                                      heroes found)
 * `now` is the device's monotonic clock in ms (only differences count; 32 bits wrap).
 *
 * The wire format is the app's (companion/data/coop/Presence.kt's PresenceCodec, 20 bytes, little endian): u8 version 1,
 * u8 flags (bit 0 valid, bit 1 female), u8 map group, u8 map num, u16 fineX, u16 fineY, u8 facing (1 S / 2 N / 3 W / 4 E),
 * u8 mode (0 walk / 1 run / 2 Mach / 3 Acro / 4 surf), u8 step, u8 look id (0..8; this core sends 0 and draws the
 * partner's own Brendan / May whatever look they wear), u32 time ms, u32 reserved 0. Anything out of range is dropped
 * whole. The smoother is PresenceSmoother.kt's, line for line; the art PartnerArt.kt's plain hero (42 frames of 32x32
 * 4bpp: walking, running, Mach and Acro Bike 9 each, surfing 3, the surf blob 3, then the palette), found in this ROM by
 * its bytes (a probe, then the CRC: the same in every language). GPL-3.0, see ../NOTICE. */

#ifndef PD_ARMGBA

#ifndef RETRO_API
#define RETRO_API
#endif

#define PD_COOP_PRESENCE_SIZE 20
#define PD_COOP_SEND_MS 100u
#define PD_COOP_DELAY_MS 150
#define PD_COOP_STALE_MS 3000
#define PD_COOP_RING 64
#define PD_COOP_OFFSETS 64
#define PD_COOP_MAX_LERP 48
#define PD_COOP_MAX_FINE 32767
#define PD_COOP_MAX_LOOK 8

struct coop_update {
  uint32_t t;
  bool valid, female;
  int group, num, fx, fy, facing, mode, step, look;
};

/* ---- the codec (PresenceCodec) ---- */

static bool coop_in_range(const struct coop_update *u) {
  return u->group >= 0 && u->group <= 255 && u->num >= 0 && u->num <= 255 && u->fx >= 0 && u->fx <= PD_COOP_MAX_FINE &&
         u->fy >= 0 && u->fy <= PD_COOP_MAX_FINE && u->facing >= 1 && u->facing <= 4 && u->mode >= 0 && u->mode <= 4 &&
         u->step >= 0 && u->step <= 255 && u->look >= 0 && u->look <= PD_COOP_MAX_LOOK;
}

static void coop_encode(const struct coop_update *u, uint8_t *b) {
  memset(b, 0, PD_COOP_PRESENCE_SIZE);
  b[0] = 1;
  b[1] = (uint8_t)((u->valid ? 1 : 0) | (u->female ? 2 : 0));
  b[2] = (uint8_t)u->group;
  b[3] = (uint8_t)u->num;
  b[4] = (uint8_t)u->fx;
  b[5] = (uint8_t)(u->fx >> 8);
  b[6] = (uint8_t)u->fy;
  b[7] = (uint8_t)(u->fy >> 8);
  b[8] = (uint8_t)u->facing;
  b[9] = (uint8_t)u->mode;
  b[10] = (uint8_t)u->step;
  b[11] = (uint8_t)u->look;
  b[12] = (uint8_t)u->t;
  b[13] = (uint8_t)(u->t >> 8);
  b[14] = (uint8_t)(u->t >> 16);
  b[15] = (uint8_t)(u->t >> 24);
}

static bool coop_decode(const uint8_t *b, size_t n, struct coop_update *u) {
  if (n != PD_COOP_PRESENCE_SIZE || b[0] != 1 || (b[1] & ~3u) || b[16] | b[17] | b[18] | b[19])
    return false;
  u->valid = b[1] & 1;
  u->female = (b[1] & 2) != 0;
  u->group = b[2];
  u->num = b[3];
  u->fx = b[4] | b[5] << 8;
  u->fy = b[6] | b[7] << 8;
  u->facing = b[8];
  u->mode = b[9];
  u->step = b[10];
  u->look = b[11];
  u->t = (uint32_t)b[12] | (uint32_t)b[13] << 8 | (uint32_t)b[14] << 16 | (uint32_t)b[15] << 24;
  return coop_in_range(u);
}

/* ---- the smoother (PresenceSmoother) ---- */

struct coop_entry {
  struct coop_update u;
  uint32_t arrived;
};

static struct {
  struct coop_entry buf[PD_COOP_RING];
  int n;
  int32_t offsets[PD_COOP_OFFSETS];
  int offCount, offNext;
  uint32_t lastT, lastNow;
  bool hasLastT;
} sm;

static inline int32_t coop_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static void sm_reset(void) {
  sm.n = 0;
  sm.offCount = sm.offNext = 0;
  sm.hasLastT = false;
}

static void sm_drop_front(int k) {
  if (k <= 0)
    return;
  memmove(sm.buf, sm.buf + k, sizeof(sm.buf[0]) * (size_t)(sm.n - k));
  sm.n -= k;
}

static void sm_push(const struct coop_update *u, uint32_t arrived) {
  // A sender silent past the stale time may come back on a restarted clock: start over rather than drop it forever.
  if (sm.n && coop_diff(arrived, sm.buf[sm.n - 1].arrived) > PD_COOP_STALE_MS)
    sm_reset();
  sm.offsets[sm.offNext] = coop_diff(arrived, u->t);
  sm.offNext = (sm.offNext + 1) % PD_COOP_OFFSETS;
  if (sm.offCount < PD_COOP_OFFSETS)
    sm.offCount++;
  if (sm.n && coop_diff(u->t, sm.buf[sm.n - 1].u.t) <= 0)
    return; // late or a duplicate
  if (sm.n == PD_COOP_RING)
    sm_drop_front(1);
  sm.buf[sm.n].u = *u;
  sm.buf[sm.n].arrived = arrived;
  sm.n++;
}

struct coop_place {
  int fx, fy, frame, hflip, blob, bob;
  bool female;
};

static int coop_dir(int facing) { return facing == 2 ? 1 : (facing == 3 || facing == 4) ? 2 : 0; }

static bool coop_snaps(const struct coop_update *a, const struct coop_update *b) {
  int dx = abs(b->fx - a->fx), dy = abs(b->fy - a->fy);
  return a->valid != b->valid || a->group != b->group || a->num != b->num || (dx > dy ? dx : dy) > PD_COOP_MAX_LERP;
}

static bool coop_first_half(int dx, int dy, int x, int y) {
  if (dx > 0)
    return x % 16 >= 1 && x % 16 <= 8;
  if (dx < 0)
    return x % 16 >= 8;
  if (dy > 0)
    return y % 16 >= 1 && y % 16 <= 8;
  if (dy < 0)
    return y % 16 >= 8;
  return false;
}

static bool sm_at(uint32_t now, int group, int num, struct coop_place *out) {
  if (!sm.n)
    return false;
  struct coop_entry *newest = &sm.buf[sm.n - 1];
  if (coop_diff(now, newest->arrived) > PD_COOP_STALE_MS)
    return false;
  int32_t minOff = sm.offsets[0];
  for (int i = 1; i < sm.offCount; i++)
    if (sm.offsets[i] < minOff)
      minOff = sm.offsets[i];
  uint32_t target = now - (uint32_t)minOff - PD_COOP_DELAY_MS, t = target;
  if (sm.hasLastT) {
    // Never backwards; behind the target it runs at 2x until it rejoins it; past the newest update it holds there.
    int32_t dt = coop_diff(now, sm.lastNow);
    if (dt < 0)
      dt = 0;
    int32_t ahead = coop_diff(target, sm.lastT);
    t = ahead > 0 ? sm.lastT + (uint32_t)(2 * dt < ahead ? 2 * dt : ahead) : sm.lastT;
  }
  if (coop_diff(t, newest->u.t) > 0)
    t = newest->u.t;
  sm.lastT = t;
  sm.lastNow = now;
  sm.hasLastT = true;

  int bi = -1;
  for (int i = 0; i < sm.n; i++)
    if (coop_diff(sm.buf[i].u.t, t) >= 0) {
      bi = i;
      break;
    }
  struct coop_update shown;
  int x, y, dx = 0, dy = 0;
  if (bi < 0) {
    shown = newest->u;
  } else if (bi == 0) {
    shown = sm.buf[0].u;
  } else {
    struct coop_update a = sm.buf[bi - 1].u, b = sm.buf[bi].u;
    if (coop_snaps(&a, &b)) {
      shown = coop_diff(b.t, t) == 0 ? b : a;
    } else {
      int64_t span = coop_diff(b.t, a.t), into = coop_diff(t, a.t);
      dx = b.fx - a.fx;
      dy = b.fy - a.fy;
      shown = b;
      shown.fx = a.fx + (int)(dx * into / span);
      shown.fy = a.fy + (int)(dy * into / span);
    }
  }
  x = shown.fx;
  y = shown.fy;
  // Nothing behind the segment being shown is needed again.
  sm_drop_front(bi < 0 ? sm.n - 1 : (bi - 1 > 0 ? bi - 1 : 0));
  if (!shown.valid || shown.group != group || shown.num != num)
    return false;
  bool stepping = coop_first_half(dx, dy, x, y);
  int d = coop_dir(shown.facing);
  out->fx = x;
  out->fy = y;
  if (shown.mode == 4)
    out->frame = 36 + d;
  else
    out->frame = stepping ? shown.mode * 9 + 3 + d * 2 + (shown.step & 1) : shown.mode * 9 + d;
  out->hflip = shown.facing == 4;
  out->blob = shown.mode == 4 ? 39 + d : -1;
  static const int bob[4] = {0, 1, 2, 1};
  out->bob = shown.mode == 4 ? bob[(now / 266u) % 4] : 0;
  out->female = shown.female;
  return true;
}

/* ---- the art (PartnerArt, the plain hero) ---- */

struct coop_blob {
  int size;
  uint32_t crc;
  int probeAt;
  uint8_t probe[16];
};

// Found in French Emerald (BPEF), the same bytes as English Emerald's (PlayerLookSigsGen's CRCs, CoopEmeraldGen's blob).
enum { CB_WALK, CB_RUN, CB_MACH, CB_ACRO, CB_SURF, CB_PAL, CB_COUNT };
static const struct coop_blob COOP_HERO[2][CB_COUNT] = {
    {
        {2304, 0x33EBBAD2u, 120, {0x9E, 0x99, 0x05, 0x00, 0xAA, 0xA9, 0x0F, 0x00, 0x00, 0xBF, 0x9B, 0xAE, 0x00, 0x5F, 0xAB, 0xEE}},
        {2304, 0x3A96F52Cu, 132, {0x00, 0xBF, 0x9A, 0xAA, 0x00, 0xBF, 0x9B, 0xAE, 0x40, 0x83, 0xAB, 0xEE, 0x40, 0x43, 0x11, 0x12}},
        {4608, 0x61D3FA2Cu, 288, {0x00, 0xBF, 0x9B, 0xAE, 0x40, 0x83, 0xAB, 0xEE, 0x40, 0x43, 0x11, 0x12, 0xF0, 0x34, 0x23, 0x28}},
        {13824, 0xD1A80D66u, 288, {0x00, 0xBF, 0x9B, 0xAE, 0x40, 0x83, 0xAB, 0xEE, 0x40, 0x43, 0x11, 0x12, 0xF0, 0x34, 0x23, 0x28}},
        {3072, 0xBCED7063u, 292, {0x00, 0xBF, 0x9B, 0xAA, 0x00, 0xBF, 0x9B, 0xAE, 0x40, 0xF3, 0xBB, 0xEE, 0x40, 0x43, 0x11, 0x12}},
        {32, 0x9975D62Au, 0, {0x0E, 0x53, 0x5F, 0x5B, 0x1F, 0x4B, 0x5B, 0x3A, 0x0F, 0x21, 0x27, 0x3D, 0xE5, 0x30, 0xA3, 0x28}},
    },
    {
        {2304, 0x6C9CB88Bu, 116, {0xAA, 0x85, 0x8B, 0x00, 0xE9, 0x9E, 0x08, 0x00, 0xEE, 0x89, 0xF4, 0x00, 0x00, 0x7F, 0x47, 0x48}},
        {2304, 0x39586DEFu, 124, {0xAA, 0x9E, 0x08, 0x00, 0x00, 0x4F, 0xA9, 0xAA, 0x00, 0x7F, 0x97, 0xEE, 0xF0, 0x27, 0x47, 0x48}},
        {4608, 0x9FEF69EDu, 284, {0x00, 0x00, 0x00, 0x00, 0xF0, 0x27, 0x47, 0x48, 0xF0, 0x74, 0x88, 0x72, 0x80, 0x84, 0x18, 0x41}},
        {13824, 0x6428FEA5u, 284, {0x00, 0x00, 0x00, 0x00, 0xF0, 0x27, 0x47, 0x48, 0xF0, 0x74, 0x88, 0x72, 0x80, 0x84, 0x18, 0x41}},
        {3072, 0x2010E029u, 288, {0x00, 0x4F, 0xA8, 0xAA, 0x00, 0x7F, 0xB7, 0xE9, 0xF0, 0x27, 0x47, 0x48, 0xF0, 0x74, 0x88, 0x72}},
        {32, 0x57800C55u, 0, {0x0E, 0x53, 0x7F, 0x67, 0x9B, 0x4A, 0x19, 0x3A, 0x6F, 0x29, 0x8C, 0x39, 0xE5, 0x20, 0xB4, 0x29}},
    },
};
static const struct coop_blob COOP_SURF_BLOB = {
    1536, 0xF2DF44CDu, 684, {0x00, 0x00, 0x00, 0xFF, 0x00, 0x60, 0x6F, 0x56, 0x60, 0xFF, 0x56, 0x55, 0xF6, 0x56, 0x55, 0x55}};

static uint32_t coop_crc32(const uint8_t *p, size_t n) {
  static uint32_t table[256];
  if (!table[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++)
        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  uint32_t c = 0xFFFFFFFFu;
  while (n--)
    c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

/** Where [b] is in the ROM (its probe, then its CRC), or NULL. A plain search: memmem isn't in MinGW's C library. */
static const uint8_t *coop_find(const struct coop_blob *b) {
  if (!romData || romLen < 16)
    return NULL;
  const uint8_t *start = romData, *end = romData + romLen;
  for (const uint8_t *p = start; p + 16 <= end; p++) {
    p = memchr(p, b->probe[0], (size_t)(end - p - 15));
    if (!p)
      return NULL;
    if (memcmp(p, b->probe, 16))
      continue;
    const uint8_t *at = p - b->probeAt;
    if (at >= start && at + b->size <= end && coop_crc32(at, (size_t)b->size) == b->crc)
      return at;
  }
  return NULL;
}

/* FireRed / LeafGreen (any Western language, the same bytes): RED's and LEAF's sheets as pret/pokefirered has them - the
 * walking one (9 frames 16x32), the surfing-and-running one (surfing S / N / W, then the running nine), the bike (9 frames
 * 32x32) - their one palette (both heroes') and the surf blob (S, S, N, N, W, W). */
enum { CF_NORMAL, CF_SURF_RUN, CF_BIKE, CF_COUNT };
static const struct coop_blob COOP_FR_HERO[2][CF_COUNT] = {
    {
        {2304, 0x97E097A0u, 120, {0xBB, 0xBB, 0x08, 0x00, 0xBB, 0xBB, 0x8C, 0x00, 0x00, 0xCF, 0xBC, 0xBB, 0x00, 0xCF, 0xCC, 0x9C}},
        {3584, 0xF0F56B06u, 120, {0xBB, 0xBB, 0x08, 0x00, 0xBB, 0xBB, 0x8C, 0x00, 0x00, 0xCF, 0xBC, 0xBB, 0x00, 0xCF, 0xCC, 0x9C}},
        {4608, 0x6B1D97B2u, 284, {0x00, 0x00, 0x00, 0x00, 0x00, 0xCF, 0xCC, 0x9C, 0xF0, 0x81, 0xCA, 0xCA, 0xF0, 0xF3, 0x96, 0x99}},
    },
    {
        {2304, 0xB7EFD380u, 116, {0x99, 0xA9, 0x06, 0x00, 0x99, 0x99, 0x06, 0x00, 0x99, 0x99, 0x6A, 0x00, 0x60, 0xA9, 0x9A, 0xB9}},
        {3584, 0x56F35E89u, 116, {0x99, 0xA9, 0x06, 0x00, 0x99, 0x99, 0x06, 0x00, 0x99, 0x99, 0x6A, 0x00, 0x60, 0xA9, 0x9A, 0xB9}},
        {4608, 0xFD3EA998u, 280, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0xA9, 0xAC, 0xAC, 0xF0, 0xA6, 0xCA, 0xCC}},
    },
};
static const struct coop_blob COOP_FR_SURF_BLOB = {
    3072, 0xDDFE3A4Bu, 284, {0x00, 0x00, 0x00, 0xF9, 0x00, 0x00, 0xFF, 0x66, 0x00, 0x6F, 0x66, 0x77, 0xF0, 0x66, 0x76, 0x77}};
static const struct coop_blob COOP_FR_PAL = {
    32, 0x45DB95BEu, 0, {0xF0, 0x51, 0xF5, 0x21, 0x1F, 0x4B, 0x5B, 0x3A, 0x0F, 0x21, 0x08, 0x69, 0xE7, 0x3C, 0x8E, 0x62}};

/** FireRed / LeafGreen's game code (BPR? / BPG?, a Western language). */
static bool coop_kanto(void) {
  if (!romData || romLen < 0xB0)
    return false;
  const char *code = (const char *)romData + 0xAC;
  return (!memcmp(code, "BPR", 3) || !memcmp(code, "BPG", 3)) && code[3] && strchr("EFDIS", code[3]);
}

static uint8_t coopArt[2][PK_PARTNER_ART_BYTES];
static int coopArtState[2]; // 0 not tried, 1 built, -1 not in this ROM
static int coopShownArt = -1; // the gender whose art the native side holds

/** A 16x32 frame (2 x 4 tiles) centred in a 32x32 one (PlayerLook.widen). */
static void coop_widen(const uint8_t *f, uint8_t *out) {
  memset(out, 0, 512);
  for (int ty = 0; ty < 4; ty++)
    for (int tx = 0; tx < 2; tx++)
      memcpy(out + (ty * 4 + tx + 1) * 32, f + (ty * 2 + tx) * 32, 32);
}

/* PartnerArt's layout from FireRed's sheets: the bike in both bike places (FireRed has one), surfing widened. */
static bool coop_build_art_kanto(int female) {
  const uint8_t *part[CF_COUNT];
  for (int i = 0; i < CF_COUNT; i++)
    if (!(part[i] = coop_find(&COOP_FR_HERO[female][i])))
      return false;
  const uint8_t *blob = coop_find(&COOP_FR_SURF_BLOB), *pal = coop_find(&COOP_FR_PAL);
  if (!blob || !pal)
    return false;
  uint8_t *out = coopArt[female];
  int at = 0;
  for (int i = 0; i < 9; i++, at += 512)
    coop_widen(part[CF_NORMAL] + i * 256, out + at);
  for (int i = 0; i < 9; i++, at += 512)
    coop_widen(part[CF_SURF_RUN] + (3 + i) * 256, out + at);
  for (int s = 0; s < 2; s++)
    for (int i = 0; i < 9; i++, at += 512)
      memcpy(out + at, part[CF_BIKE] + i * 512, 512);
  for (int i = 0; i < 3; i++, at += 512)
    coop_widen(part[CF_SURF_RUN] + i * 256, out + at);
  for (int i = 0; i < 3; i++, at += 512)
    memcpy(out + at, blob + i * 2 * 512, 512);
  memcpy(out + at, pal, 32);
  coopArtState[female] = 1;
  return true;
}

static bool coop_build_art(int female) {
  if (coopArtState[female])
    return coopArtState[female] > 0;
  coopArtState[female] = -1;
  if (coop_kanto())
    return coop_build_art_kanto(female);
  const uint8_t *blob = coop_find(&COOP_SURF_BLOB);
  const uint8_t *part[CB_COUNT];
  for (int i = 0; i < CB_COUNT; i++)
    if (!(part[i] = coop_find(&COOP_HERO[female][i])))
      return false;
  if (!blob)
    return false;
  uint8_t *out = coopArt[female];
  int at = 0;
  // Walking and running: 16x32 frames, the first nine; the bikes: 32x32, the first nine; surfing: frames 0, 2, 4.
  for (int s = CB_WALK; s <= CB_RUN; s++)
    for (int i = 0; i < 9; i++, at += 512)
      coop_widen(part[s] + i * 256, out + at);
  for (int s = CB_MACH; s <= CB_ACRO; s++)
    for (int i = 0; i < 9; i++, at += 512)
      memcpy(out + at, part[s] + i * 512, 512);
  for (int i = 0; i < 6; i += 2, at += 512)
    memcpy(out + at, part[CB_SURF] + i * 512, 512);
  memcpy(out + at, blob, 1536);
  at += 1536;
  memcpy(out + at, part[CB_PAL], 32);
  coopArtState[female] = 1;
  return true;
}

/* ---- the presence ---- */

static struct {
  bool sentOnce, shown, haveField;
  uint32_t lastSend;
  int sentGroup, sentNum;
  int32_t lastField[PK_PARTNER_SELF_N];
} cp;
static int coopAvailable = -1; // -1 not worked out for this game
static uint32_t coopNow;      // the frontend's clock at its last retro_pd_coop_frame (the presence's own time)

static void coop_game_changed(void) {
  coopAvailable = -1;
  coopArtState[0] = coopArtState[1] = 0;
  coopShownArt = -1;
  sm_reset();
  memset(&cp, 0, sizeof cp);
  cp.shown = true; // a previous game's partner may still be asked for: place none on the first frame
}

RETRO_API int retro_pd_coop_available(void) {
  if (coopAvailable < 0) {
    coopAvailable = 0;
    if (core && PD_BE_IS_GBA() && fieldKnown && romData && romLen >= 0xB0) {
      // Emerald in a Western language (BPEE / BPEF / BPED / BPEI / BPES): the same maps and the same RAM the field reads use.
      const char *code = (const char *)romData + 0xAC;
      bool emerald = !memcmp(code, "BPE", 3) && strchr("EFDIS", code[3]) && code[3];
      // Or FireRed / LeafGreen (seeing each other only: the joined battles and trades are Emerald's).
      coopAvailable = (emerald || coop_kanto()) && coop_build_art(0) && coop_build_art(1);
    }
  }
  return coopAvailable;
}

/** The player's gender: SaveBlock2's playerGender (gSaveBlock2Ptr, Emerald's 0x03005D90, + 8; FireRed's the snapshot's). */
static bool coop_female(void) {
  if (coop_kanto())
    return snap.valid && snap.gender == 1;
  uint32_t sb2 = 0;
  uint8_t g = 0;
  if (!pd_lr_read_memory(core, 0x03005D90u, &sb2, 4) || (sb2 >> 24) != 2)
    return false;
  return pd_lr_read_memory(core, sb2 + 8, &g, 1) && g == 1;
}

RETRO_API void retro_pd_coop_receive(const uint8_t *msg, size_t n, uint32_t now) {
  struct coop_update u;
  if (!msg || !coop_decode(msg, n, &u))
    return;
  sm_push(&u, now);
}

RETRO_API void retro_pd_coop_stop(void) {
  sm_reset();
  if (core && PD_BE_IS_GBA()) {
    pk_partner_place(core, 0, 0, 0, 0, 0, -1, 0);
    pk_partner_set(NULL);
  }
  coopShownArt = -1;
  memset(&cp, 0, sizeof cp); // nothing placed now (cp.shown false)
}

RETRO_API int retro_pd_coop_frame(uint32_t now, uint8_t *out) {
  if (!retro_pd_coop_available())
    return 0;
  coopNow = now;
  bool field = (fieldFlags & PK_FIELD_KNOWN) && !(fieldFlags & PK_FIELD_BATTLE) && !snap.inBattle &&
               (!wideSupported || pk_wide_scene() == PK_WIDE_SCENE_FIELD);
  int32_t self[PK_PARTNER_SELF_N];
  bool haveSelf = pk_partner_self(core, self) != 0;
  int group = haveSelf ? self[PK_PARTNER_SELF_GROUP] : -1, num = haveSelf ? self[PK_PARTNER_SELF_NUM] : -1;
  int sent = 0;
  if (out && (!cp.sentOnce || (uint32_t)(now - cp.lastSend) >= PD_COOP_SEND_MS || group != cp.sentGroup || num != cp.sentNum)) {
    cp.sentOnce = true;
    cp.lastSend = now;
    cp.sentGroup = group;
    cp.sentNum = num;
    struct coop_update u = {.t = now, .female = coop_female(), .facing = 1};
    bool ok = haveSelf && self[PK_PARTNER_SELF_VALID] && self[PK_PARTNER_SELF_GROUP] >= 0 && self[PK_PARTNER_SELF_GROUP] <= 255 &&
              self[PK_PARTNER_SELF_NUM] >= 0 && self[PK_PARTNER_SELF_NUM] <= 255 && self[PK_PARTNER_SELF_FX] >= 0 &&
              self[PK_PARTNER_SELF_FX] <= PD_COOP_MAX_FINE && self[PK_PARTNER_SELF_FY] >= 0 && self[PK_PARTNER_SELF_FY] <= PD_COOP_MAX_FINE &&
              self[PK_PARTNER_SELF_FACING] >= 1 && self[PK_PARTNER_SELF_FACING] <= 4 && self[PK_PARTNER_SELF_MODE] >= 0 &&
              self[PK_PARTNER_SELF_MODE] <= 4;
    if (!ok) {
      cp.haveField = false;
    } else {
      // On the field it is remembered; in a battle or a menu the player stays at that last field place, walking.
      if (field) {
        memcpy(cp.lastField, self, sizeof self);
        cp.haveField = true;
      }
      const int32_t *f = field || !cp.haveField ? self : cp.lastField;
      u.valid = true;
      u.group = self[PK_PARTNER_SELF_GROUP];
      u.num = self[PK_PARTNER_SELF_NUM];
      u.fx = f[PK_PARTNER_SELF_FX];
      u.fy = f[PK_PARTNER_SELF_FY];
      u.facing = f[PK_PARTNER_SELF_FACING];
      u.mode = field ? f[PK_PARTNER_SELF_MODE] : 0;
      u.step = f[PK_PARTNER_SELF_STEP] & 0xFF;
    }
    coop_encode(&u, out);
    sent = 1;
  }
  struct coop_place p;
  if (field && sm_at(now, group, num, &p)) {
    int g = p.female ? 1 : 0;
    if (coopShownArt != g) {
      pk_partner_set(coopArt[g]);
      coopShownArt = g;
    }
    pk_partner_place(core, 1, p.fx, p.fy, p.frame, p.hflip, p.blob, p.bob);
    cp.shown = true;
  } else if (cp.shown) {
    pk_partner_place(core, 0, 0, 0, 0, 0, -1, 0);
    cp.shown = false;
  }
  return sent;
}

#endif
