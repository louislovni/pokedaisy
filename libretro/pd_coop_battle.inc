/* Co-op joined battles in the libretro core (Capsule's iPhone / PC builds; the app's is CoopLinkFlow.kt + CoopLinkBattle.kt +
 * CoopBattles.kt): a battle that starts near the partner is offered to them, and when they join it runs as Emerald's own link
 * battle in both games (the app's C bridge, pk_coop_link*.c, built for English and French Emerald: pk_coop_fr_names.h). The
 * messages are the app's (CoopBattleWire.kt), byte for byte, so an iPhone and an Android play together. Included by
 * pd_libretro.c after pd_coop.inc (its partner presence gives the nearby rule). Wild, two-trainer and one-trainer battles;
 * story battles (gym leaders) are not offered from here yet.
 *
 * The frontend carries the messages (types 0x0200-0x02FF) over its co-op link, on the thread that runs the frames:
 *   retro_pd_coop_battle_in(type, bytes, n)  a message from the partner
 *   retro_pd_coop_battle_out(&type, out, cap) the next one to send (its size; 0 = none), called until 0
 *   retro_pd_coop_battle_link(connected)     the link came up / went down
 * GPL-3.0, see ../NOTICE. */

#ifndef PD_ARMGBA

#include "pk_coop.h"
#include "pk_coop_after.h"
#include "pk_coop_field.h"
#include "pk_call.h"
#include "pd_save.h"
#include "pd_coop_story_spots.h"
#include <time.h>

/* The French cart's twins (pk_coop_fr_names.h renames every external name of pk_coop*.c: pk_coop_x -> pk_coopfr_x). */
int pk_coopfr_install(struct mCore *core);
const char *pk_coopfr_refused(void);
int pk_coopfr_link_install(struct mCore *core, pk_coop_call_fn call);
const char *pk_coopfr_link_refused(void);
int pk_coopfr_link_take(struct mCore *core, uint8_t *out, int cap);
int pk_coopfr_link_give(struct mCore *core, const uint8_t *block, int size);
int pk_coopfr_link_state(struct mCore *core);
void pk_coopfr_link_tick(struct mCore *core);
int pk_coopfr_link_bad_blocks(uint8_t *out, int cap);
void pk_coopfr_link_leave(struct mCore *core, int blackout);
int pk_coopfr_link_result(struct mCore *core, uint8_t *out, int cap);
void pk_coopfr_link_result_clear(struct mCore *core);
void pk_coopfr_link_reset(struct mCore *core);
int pk_coopfr_link_caught(struct mCore *core, uint8_t mon[100]);
int pk_coopfr_link_host_hold(struct mCore *core);
int pk_coopfr_link_host_go(struct mCore *core, const struct pk_coop_link_setup *partner);
int pk_coopfr_link_partner_go(struct mCore *core, const struct pk_coop_link_setup *setup);
int pk_coopfr_on_free_field(struct mCore *core);
int pk_coopfr_join_kind(struct mCore *core);
void pk_coopfr_join_trainers(struct mCore *core, int out[2]);
int pk_coopfr_partner_free(struct mCore *core);
int pk_coopfr_give_mon(struct mCore *core, const uint8_t mon[100]);
int pk_coopfr_save_game(struct mCore *core);
int pk_coopfr_white_out(struct mCore *core);
int pk_coopfr_story_arm(struct mCore *core, pk_coop_call_fn call, uint16_t trainerId);
int pk_coopfr_story_state(struct mCore *core);
int pk_coopfr_story_go(struct mCore *core, struct pk_coop_link_setup *setup);
int pk_coopfr_story_cancel(struct mCore *core);
void pk_coopfr_story_disarm(struct mCore *core);
int pk_coopfr_release_script(struct mCore *core);
void pk_coopfr_after_set_call(pk_coop_call_fn call);
void pk_coopfr_after_set_cap(pk_coop_cap_fn cap);

static const struct coop_api {
  int (*install)(struct mCore *);
  const char *(*refused)(void);
  int (*link_install)(struct mCore *, pk_coop_call_fn);
  const char *(*link_refused)(void);
  int (*take)(struct mCore *, uint8_t *, int);
  int (*give)(struct mCore *, const uint8_t *, int);
  int (*state)(struct mCore *);
  void (*tick)(struct mCore *);
  int (*bad_blocks)(uint8_t *, int);
  void (*leave)(struct mCore *, int);
  int (*result)(struct mCore *, uint8_t *, int);
  void (*result_clear)(struct mCore *);
  void (*reset)(struct mCore *);
  int (*caught)(struct mCore *, uint8_t *);
  int (*host_hold)(struct mCore *);
  int (*host_go)(struct mCore *, const struct pk_coop_link_setup *);
  int (*partner_go)(struct mCore *, const struct pk_coop_link_setup *);
  int (*free_field)(struct mCore *);
  int (*join_kind)(struct mCore *);
  void (*join_trainers)(struct mCore *, int *);
  int (*partner_free)(struct mCore *);
  int (*give_mon)(struct mCore *, const uint8_t *);
  int (*save_game)(struct mCore *);
  int (*white_out)(struct mCore *);
  void (*set_call)(pk_coop_call_fn);
  void (*set_cap)(pk_coop_cap_fn);
  int (*story_arm)(struct mCore *, pk_coop_call_fn, uint16_t);
  int (*story_state)(struct mCore *);
  int (*story_go)(struct mCore *, struct pk_coop_link_setup *);
  int (*story_cancel)(struct mCore *);
  void (*story_disarm)(struct mCore *);
  int (*release_script)(struct mCore *);
  uint32_t battleMoves; /* gBattleMoves (the PP check of a partner's Pokémon) */
} COOP_EN = {pk_coop_install, pk_coop_refused, pk_coop_link_install, pk_coop_link_refused, pk_coop_link_take,
             pk_coop_link_give, pk_coop_link_state, pk_coop_link_tick, pk_coop_link_bad_blocks, pk_coop_link_leave,
             pk_coop_link_result, pk_coop_link_result_clear, pk_coop_link_reset, pk_coop_link_caught, pk_coop_link_host_hold,
             pk_coop_link_host_go, pk_coop_link_partner_go, pk_coop_on_free_field, pk_coop_join_kind, pk_coop_join_trainers,
             pk_coop_partner_free, pk_coop_give_mon, pk_coop_save_game, pk_coop_white_out, pk_coop_after_set_call,
             pk_coop_after_set_cap, pk_coop_story_arm, pk_coop_story_state, pk_coop_story_go, pk_coop_story_cancel,
             pk_coop_story_disarm, pk_coop_release_script, 0x0831C898u},
  COOP_FR = {pk_coopfr_install, pk_coopfr_refused, pk_coopfr_link_install, pk_coopfr_link_refused, pk_coopfr_link_take,
             pk_coopfr_link_give, pk_coopfr_link_state, pk_coopfr_link_tick, pk_coopfr_link_bad_blocks, pk_coopfr_link_leave,
             pk_coopfr_link_result, pk_coopfr_link_result_clear, pk_coopfr_link_reset, pk_coopfr_link_caught,
             pk_coopfr_link_host_hold, pk_coopfr_link_host_go, pk_coopfr_link_partner_go, pk_coopfr_on_free_field,
             pk_coopfr_join_kind, pk_coopfr_join_trainers, pk_coopfr_partner_free, pk_coopfr_give_mon, pk_coopfr_save_game,
             pk_coopfr_white_out, pk_coopfr_after_set_call, pk_coopfr_after_set_cap, pk_coopfr_story_arm,
             pk_coopfr_story_state, pk_coopfr_story_go, pk_coopfr_story_cancel, pk_coopfr_story_disarm,
             pk_coopfr_release_script, 0x08324408u};

/* Retail English and French Emerald only (their SHA-1s: the bridge's addresses are those carts'). */
static const struct coop_api *cb_api(void) {
  if (!core || !PD_BE_IS_GBA())
    return NULL;
  if (!strcmp(game.sha1, "f3ae088181bf583e55daf962a92bb46f4f1d07b7"))
    return &COOP_EN;
  if (!strcmp(game.sha1, "ca666651374d89ca439007bed54d839eb7bd14d0"))
    return &COOP_FR;
  return NULL;
}

static uint32_t cb_call(struct mCore *c, uint32_t fn, const uint32_t args[4]) {
  uint32_t r0;
  return pk_call(c, fn, args, &r0) ? r0 : 0xFFFFFFFFu;
}
static void cb_cap(long steps) { pk_call_set_cap(steps); }

/* ---- the wire (CoopBattleWire.kt) ---- */
enum {
  CB_STATUS = 0x0200, CB_OFFER, CB_ANSWER, CB_CANCEL, CB_RESULT_ACK = 0x020F, CB_LINK_BLOCK, CB_LINK_READY, CB_CAUGHT_MON,
  CB_STORY_STATE
};
enum { CB_TIMEOUT, CB_DECLINED, CB_ENGINE, CB_LINK, CB_BUSY, CB_CLOSED };
#define CB_MON 100
#define CB_NAME 8
#define CB_MAX_PAYLOAD 0x4000
#define CB_TRAINERS 855

struct cb_msg {
  int type;
  uint32_t id;
  /* Status */
  bool free, ready;
  int story;
  /* Offer */
  int kind, trainerA, trainerB, species;
  uint8_t name[CB_NAME];
  int gender;
  uint32_t otId;
  /* Answer */
  bool accept;
  int count;
  uint8_t team[3 * CB_MON];
  int slots[3];
  /* Cancel */
  int reason;
  /* CaughtMon */
  int by;
  uint8_t mon[CB_MON];
  /* LinkBlock */
  const uint8_t *block;
  int blockLen;
};

static void put16(uint8_t *p, int v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static int get16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool cb_valid_name(const uint8_t *n) {
  int end = -1;
  for (int i = 0; i < CB_NAME && end < 0; i++)
    if (n[i] == 0xFF)
      end = i;
  if (end < 0)
    return false;
  for (int i = 0; i < end; i++)
    if (n[i] > 0xF4)
      return false;
  return true;
}

static bool cb_mon_ok(const uint8_t *mon);

/* The message in [p] (decoded fields point into it for a link block); false for anything malformed. */
static bool cb_decode(int type, const uint8_t *p, int n, struct cb_msg *m) {
  memset(m, 0, sizeof *m);
  if (type < CB_STATUS || type > CB_STORY_STATE || n < 4)
    return false;
  if (type > CB_CANCEL && type < CB_RESULT_ACK)
    return false;
  m->type = type;
  m->id = get32(p);
  if ((m->id == 0) != (type == CB_STATUS))
    return false;
  const uint8_t *b = p + 4;
  int left = n - 4;
  switch (type) {
  case CB_STATUS:
    if (left != 3 || b[0] > 3 || get16(b + 1) > 0x3FF)
      return false;
    m->free = b[0] & 1, m->ready = (b[0] & 2) != 0, m->story = get16(b + 1);
    return true;
  case CB_OFFER:
    if (left != 7 + CB_NAME + 1 + 4)
      return false;
    m->kind = b[0], m->trainerA = get16(b + 1), m->trainerB = get16(b + 3), m->species = get16(b + 5);
    memcpy(m->name, b + 7, CB_NAME);
    m->gender = b[15], m->otId = get32(b + 16);
    if (m->kind < 1 || m->kind > 4 || m->trainerA >= CB_TRAINERS || m->trainerB >= CB_TRAINERS)
      return false;
    if ((m->kind == 1 && (m->trainerA || m->trainerB)) || (m->kind == 2 && (!m->trainerA || !m->trainerB)) ||
        (m->kind == 3 && (!m->trainerA || m->trainerB)) || (m->kind == 4 && !m->trainerA))
      return false;
    return m->gender <= 1 && cb_valid_name(m->name);
  case CB_ANSWER:
    if (left < 1)
      return false;
    if (b[0] == 0)
      return left == 1;
    if (b[0] != 1 || left < 2)
      return false;
    m->accept = true, m->count = b[1];
    if (m->count < 1 || m->count > 3 || left != 2 + m->count * CB_MON + CB_NAME + 1 + 4 + m->count)
      return false;
    memcpy(m->team, b + 2, (size_t)m->count * CB_MON);
    for (int i = 0; i < m->count; i++)
      if (!cb_mon_ok(m->team + i * CB_MON))
        return false;
    b += 2 + m->count * CB_MON;
    memcpy(m->name, b, CB_NAME);
    m->gender = b[8], m->otId = get32(b + 9);
    for (int i = 0; i < m->count; i++) {
      m->slots[i] = b[13 + i];
      if (m->slots[i] > 5)
        return false;
      for (int k = 0; k < i; k++)
        if (m->slots[k] == m->slots[i])
          return false;
    }
    return m->gender <= 1 && cb_valid_name(m->name);
  case CB_CANCEL:
    m->reason = left == 1 ? b[0] : -1;
    return left == 1 && b[0] <= CB_CLOSED;
  case CB_RESULT_ACK:
  case CB_LINK_READY:
    return left == 0;
  case CB_LINK_BLOCK:
    if (left < 4 || get16(b + 2) != left - 4 || left - 4 > CB_MAX_PAYLOAD)
      return false;
    m->block = b, m->blockLen = left;
    return true;
  case CB_CAUGHT_MON:
    if (left < 1)
      return false;
    m->by = b[0];
    if (m->by == 1)
      return left == 1;
    if (m->by != 2 || left != 1 + CB_MON)
      return false;
    memcpy(m->mon, b + 1, CB_MON);
    return cb_mon_ok(m->mon);
  case CB_STORY_STATE:
    m->story = left == 1 ? b[0] : -1;
    return left == 1 && b[0] <= 6;
  }
  return false;
}

/* ---- the outbox (frontend side) and the inbox ---- */
#define CB_OUT_SLOTS 256
#define CB_OUT_BYTES (1 << 20)
static struct {
  uint8_t buf[CB_OUT_BYTES];
  int head, tail, used;  /* byte ring of [u16 type][u16 len][bytes] records */
  int count;
} cbOut;

static bool cb_connected;

static bool cb_send_raw(int type, const uint8_t *p, int n) {
  if (!cb_connected || n < 0 || n > CB_MAX_PAYLOAD + 8)
    return false;
  int need = 4 + n;
  if (cbOut.used + need > CB_OUT_BYTES || cbOut.count >= CB_OUT_SLOTS * 64)
    return false;
  uint8_t hdr[4];
  put16(hdr, type);
  put16(hdr + 2, n);
  for (int i = 0; i < 4; i++)
    cbOut.buf[(cbOut.head + i) % CB_OUT_BYTES] = hdr[i];
  for (int i = 0; i < n; i++)
    cbOut.buf[(cbOut.head + 4 + i) % CB_OUT_BYTES] = p[i];
  cbOut.head = (cbOut.head + need) % CB_OUT_BYTES;
  cbOut.used += need;
  cbOut.count++;
  return true;
}

RETRO_API int retro_pd_coop_battle_out(int *type, uint8_t *out, int cap) {
  if (!cbOut.count || !type || !out)
    return 0;
  uint8_t hdr[4];
  for (int i = 0; i < 4; i++)
    hdr[i] = cbOut.buf[(cbOut.tail + i) % CB_OUT_BYTES];
  int n = get16(hdr + 2);
  if (n > cap)
    return -1;
  *type = get16(hdr);
  for (int i = 0; i < n; i++)
    out[i] = cbOut.buf[(cbOut.tail + 4 + i) % CB_OUT_BYTES];
  cbOut.tail = (cbOut.tail + 4 + n) % CB_OUT_BYTES;
  cbOut.used -= 4 + n;
  cbOut.count--;
  return n;
}

static bool cb_send_id(int type, uint32_t id, const uint8_t *body, int n) {
  uint8_t p[4 + 2 + 3 * CB_MON + CB_NAME + 1 + 4 + 3 + 16];
  if (n > (int)sizeof p - 4) {
    uint8_t *big = malloc((size_t)n + 4);
    if (!big)
      return false;
    put32(big, id);
    memcpy(big + 4, body, (size_t)n);
    bool ok = cb_send_raw(type, big, n + 4);
    free(big);
    return ok;
  }
  put32(p, id);
  if (n)
    memcpy(p + 4, body, (size_t)n);
  return cb_send_raw(type, p, n + 4);
}

static bool cb_send_cancel(uint32_t id, int reason) {
  uint8_t r = (uint8_t)reason;
  return cb_send_id(CB_CANCEL, id, &r, 1);
}

/* ---- this player ---- */
#define CB_PARTY 0x020244ECu
#define CB_PARTY_COUNT 0x020244E9u
#define CB_ENEMY_PARTY 0x02024744u
#define CB_SAVEBLOCK2_PTR 0x03005D90u

struct cb_identity {
  uint8_t name[CB_NAME];
  int gender;
  uint32_t otId;
};
static void cb_identity(struct cb_identity *who) {
  memset(who->name, 0xFF, CB_NAME);
  who->gender = 0, who->otId = 0;
  uint32_t sb2 = 0;
  uint8_t raw[14];
  if (!pd_lr_read_memory(core, CB_SAVEBLOCK2_PTR, &sb2, 4) || (sb2 >> 24) != 2 || !pd_lr_read_memory(core, sb2, raw, 14))
    return;
  memcpy(who->name, raw, 7); /* PLAYER_NAME_LENGTH + its end */
  who->name[7] = 0xFF;
  who->gender = raw[8] == 1;
  who->otId = get32(raw + 10);
}

static int cb_party(uint8_t party[6 * CB_MON]) {
  uint8_t n = 0;
  if (!pd_lr_read_memory(core, CB_PARTY_COUNT, &n, 1) || !pd_lr_read_memory(core, CB_PARTY, party, 6 * CB_MON))
    return 0;
  return n > 6 ? 6 : n;
}

/* CoopBattle.check: the only gate for a partner's untrusted bytes (checksum, no egg, species / item / moves / PP in range,
 * level 1..100, HP no more than max HP). */
static bool cb_mon_ok(const uint8_t *mon) {
  struct pd_box_mon b;
  pd_box_mon_read(&b, mon);
  if (!b.checksumOk || pd_box_mon_bad_egg(&b) || pd_box_mon_egg(&b) || (mon[19] & 4))
    return false;
  int species = pd_box_mon_species(&b);
  if (species < 1 || species > 411 || (species >= 252 && species <= 276) || pd_box_mon_item(&b) > 376)
    return false;
  const struct coop_api *api = cb_api();
  int ppBonuses = b.data[8];
  for (int i = 0; i < 4; i++) {
    int move = pd_box_mon_move(&b, i);
    if (move >= 355)
      return false;
    int pp = b.data[12 + 8 + i];
    uint8_t base = 0;
    if (api && romData && api->battleMoves - 0x08000000u + (uint32_t)move * 12 + 4 < romLen)
      base = romData[api->battleMoves - 0x08000000u + (uint32_t)move * 12 + 4];
    int bonus = (ppBonuses >> (2 * i)) & 3;
    if (api && pp > base * (5 + bonus) / 5)
      return false;
  }
  int level = mon[0x54], hp = get16(mon + 0x56), maxHp = get16(mon + 0x58);
  return level >= 1 && level <= 100 && hp <= maxHp;
}

/* The first three Pokémon of the party that can fight (CoopBattle.ableSlots): their slots, and their bytes packed. */
static int cb_able(const uint8_t *party, int count, int slots[3], uint8_t *packed) {
  int n = 0;
  for (int i = 0; i < count && n < 3; i++) {
    const uint8_t *m = party + i * CB_MON;
    if (cb_mon_ok(m) && get16(m + 0x56) > 0) {
      slots[n] = i;
      if (packed)
        memcpy(packed + n * CB_MON, m, CB_MON);
      n++;
    }
  }
  return n;
}

/* ---- the flow (CoopLinkFlow.kt) ---- */
enum { CBP_IDLE, CBP_H_ASKING, CBP_H_WAIT, CBP_H_GO, CBP_P_PROMPT, CBP_P_START, CBP_RUNNING, CBP_AFTER, CBP_S_ARMED };
enum { CBS_IDLE, CBS_ARMED, CBS_PICKING, CBS_READY, CBS_CANCELLED, CBS_MISMATCH, CBS_GO };
#define CB_STORY 4
#define CB_STORY_ARM_MS 300000u
#define CB_STORY_LOOK_MS 100u
#define CB_ABORT_WAIT_MS 60000u
#define CB_PROMPT_MS 30000u
#define CB_START_WAIT_MS 15000u
#define CB_MAX_GO_TRIES 120
#define CB_STATUS_MS 1000u
#define CB_DROP_MS 60000u
#define CB_RANGE 8
#define CB_CAUGHT_RESEND_MS 2000u
#define CB_OPS_PATIENCE_MS 60000u

static struct {
  int phase;
  bool host;
  uint32_t id;
  int kind, trainerA, trainerB;
  struct cb_identity other;
  uint32_t phaseAt, askedAt;
  int tries;
  bool heard, readyEarly, startHandled, accepted;
  struct cb_msg answer;
  /* the partner's last STATUS */
  bool partnerFree, partnerReady;
  int partnerStory;
  /* story joins */
  int storyNow, storyDone, storySent, otherStory;
  uint32_t storySeenAt, lastStoryLook, armDownAt, token;
  bool storyGoSeen, storyDrop, abortRelease, blackoutOwed;
  uint32_t lastSpot;
  int sentStory;
  bool lbStory; /* the running link battle is a story one (a drop for good is a loss) */
  /* this side's STATUS beat */
  bool ready, sentFree, sentReady;
  uint32_t lastStatus, lastProbe;
  bool statusOnce;
  /* the prompt */
  struct cb_msg offer;
  bool cursorJoin;
  /* the end */
  bool wantSave;
  int ops[4], opCount, opAt;
  uint8_t giveMon[CB_MON];
  uint32_t opsSince;
  /* a partner's catch the host keeps until acknowledged (in memory) */
  uint32_t pendingId;
  uint8_t pendingMon[CB_MON];
  bool pending;
  uint32_t caughtSent;
  /* the partner's catch this game gives itself */
  bool lateGive;
  uint32_t lateId, unsavedId;
  uint8_t lateMon[CB_MON];
  bool unsaved;
  uint32_t givenIds[16];
  int givenCount;
} cb;

/* the block pump (CoopLinkBattle.kt) */
#define CB_RING 64
#define CB_BLOCK (PK_COOP_LINK_WIRE_HEADER + PK_COOP_LINK_BLOCK_MAX)
static struct {
  bool active, pumping, endedSeen, reported, cancelled;
  uint32_t id, downAt;
  bool down;
  int lastSeq, refusals, endFrames, settleFrames, flushFrames;
  uint8_t out[CB_RING][CB_BLOCK];
  int outLen[CB_RING], outHead, outCount;
  uint8_t in[CB_RING][CB_BLOCK];
  int inLen[CB_RING], inHead, inCount;
  uint32_t expected;
} lb;

static uint32_t cbNow; /* ms, the frontend's clock (retro_pd_coop_frame's) */
static char cbNotice[64];
static uint32_t cbNoticeUntil;

static void cb_notice(const char *s) {
  snprintf(cbNotice, sizeof cbNotice, "%s", s);
  cbNoticeUntil = cbNow + 4000;
  fprintf(stderr, "pokedaisy coop: %s\n", s);
}
static void cb_log(const char *s) { fprintf(stderr, "pokedaisy coop: %s\n", s); }

static void cb_ops_load(const int *ops, int n) {
  cb.opCount = n;
  memcpy(cb.ops, ops, sizeof(int) * (size_t)n);
  cb.opAt = 0;
  cb.opsSince = cbNow;
}
enum { CBO_GIVE, CBO_SAVE, CBO_WHITEOUT };

static void cb_reset(void) {
  cb.phase = CBP_IDLE;
  cb.storyGoSeen = cb.storyDrop = cb.abortRelease = false;
  cb.armDownAt = 0;
  cb.kind = 0;
  cb.host = false;
  cb.readyEarly = false;
  cb.accepted = false;
  cb.opCount = 0;
  cb.wantSave = false;
}

/* the pump */
static void lb_reset(void) {
  lb.outCount = lb.inCount = 0;
  lb.lastSeq = -1;
  lb.refusals = lb.endFrames = lb.settleFrames = lb.flushFrames = 0;
  lb.down = lb.endedSeen = lb.reported = lb.active = lb.pumping = lb.cancelled = false;
}
static void lb_expect(uint32_t id) {
  lb.inCount = 0;
  lb.expected = id;
}
static void lb_forget(void) {
  lb.expected = 0;
  lb.inCount = 0;
}
/* start: the engine call's answer (1 started, 0 refused, -1 not yet); only a 1 makes the battle run */
static int lb_start(uint32_t id, int r) {
  if (r != 1)
    return r;
  uint32_t keep = lb.expected;
  int inCount = lb.inCount, inHead = lb.inHead;
  lb_reset();
  /* blocks the other game sent before this one began are kept (they were expected) */
  if (keep == id)
    lb.inCount = inCount, lb.inHead = inHead;
  lb.id = id;
  lb.active = lb.pumping = true;
  lb.expected = 0;
  return 1;
}

static void lb_block_in(const struct cb_msg *m) {
  if (!lb.active && !(lb.expected && m->id == lb.expected))
    return;
  if (lb.active && m->id != lb.id)
    return;
  if (m->blockLen > CB_BLOCK || lb.inCount >= CB_RING)
    return;
  int seq = m->block[1];
  if (lb.lastSeq >= 0 && (((seq - lb.lastSeq) & 0xFF) == 0 || ((seq - lb.lastSeq) & 0xFF) > 127))
    return; /* seen already, or older */
  lb.lastSeq = seq;
  int at = (lb.inHead + lb.inCount) % CB_RING;
  memcpy(lb.in[at], m->block, (size_t)m->blockLen);
  lb.inLen[at] = m->blockLen;
  lb.inCount++;
}

static void cb_ended(const uint8_t *result, int caughtBy, const uint8_t *caught, bool left);

static void lb_report(const uint8_t *result, bool left) {
  const struct coop_api *api = cb_api();
  if (lb.reported || !api)
    return;
  lb.reported = true;
  uint8_t mon[CB_MON];
  int by = api->caught(core, mon);
  api->result_clear(core);
  api->reset(core);
  lb.active = lb.pumping = false;
  cb_ended(result, by, by == 1 || by == 2 ? mon : NULL, left);
}

static void lb_finish(bool blackout) {
  const struct coop_api *api = cb_api();
  if (!api)
    return;
  uint8_t mon[CB_MON];
  int by = api->caught(core, mon);
  api->leave(core, blackout);
  if (lb.endedSeen) {
    api->result_clear(core);
    api->reset(core);
  }
  lb.active = false;
  lb.settleFrames = 0;
  if (!lb.reported) {
    lb.reported = true;
    cb_ended(NULL, by, by == 1 || by == 2 ? mon : NULL, true);
  }
  lb.pumping = !api->free_field(core);
}

static void lb_fail_loud(const char *why) {
  cb_log(why);
  cb_notice("THE LINK DROPPED. THE BATTLE WAS UNDONE.");
  cb_send_cancel(lb.id, CB_ENGINE);
  lb_finish(false);
}

/* After EVERY emulated frame while the bridge needs its state pumped. */
static void lb_after_frame(void) {
  const struct coop_api *api = cb_api();
  if (!lb.pumping || !api)
    return;
  api->tick(core); /* the end's steps ride the state pump */
  int st = api->state(core);
  if (!lb.active) {
    if (++lb.settleFrames >= 1800 || api->free_field(core))
      lb.pumping = false;
    return;
  }
  if (st == PK_COOP_LINK_ERROR)
    return lb_fail_loud("the bridge reports the two games out of step");
  if (st == PK_COOP_LINK_ENDED && !lb.endedSeen) {
    lb.endedSeen = true;
    lb.endFrames = lb.flushFrames = 0;
  }
  for (;;) {
    if (lb.outCount >= CB_RING)
      return lb_fail_loud("outbox full");
    int at = (lb.outHead + lb.outCount) % CB_RING;
    int n = api->take(core, lb.out[at], CB_BLOCK);
    if (n <= 0)
      break;
    lb.outLen[at] = n;
    lb.outCount++;
  }
  /* flush */
  while (lb.outCount && !lb.down && cb_connected) {
    if (!cb_send_id(CB_LINK_BLOCK, lb.id, lb.out[lb.outHead], lb.outLen[lb.outHead]))
      break;
    lb.outHead = (lb.outHead + 1) % CB_RING;
    lb.outCount--;
  }
  /* give what came */
  while (lb.inCount) {
    if (api->give(core, lb.in[lb.inHead], lb.inLen[lb.inHead])) {
      lb.inHead = (lb.inHead + 1) % CB_RING;
      lb.inCount--;
      lb.refusals = 0;
    } else if (lb.endedSeen) {
      lb.inHead = (lb.inHead + 1) % CB_RING;
      lb.inCount--;
      lb.refusals = 0;
    } else {
      if (++lb.refusals >= 1800)
        return lb_fail_loud("the game refused a block 1800 frames running");
      break;
    }
  }
  if (lb.down && !lb.endedSeen) {
    if (cbNow - lb.downAt < CB_DROP_MS)
      return;
    cb_notice("THE LINK DROPPED. THE BATTLE WAS UNDONE.");
    cb_send_cancel(lb.id, CB_CLOSED);
    lb_finish(cb.lbStory); /* a story battle dropped for good is a loss: the game's blackout */
    return;
  }
  if (!lb.endedSeen)
    return;
  uint8_t r[PK_COOP_LINK_RESULT_SIZE];
  if (api->result(core, r, sizeof r) != PK_COOP_LINK_RESULT_SIZE) {
    if (!api->free_field(core)) {
      lb.endFrames = 0;
      return;
    }
    if (++lb.endFrames >= 3600) {
      cb_send_cancel(lb.id, CB_ENGINE);
      api->leave(core, 0);
      lb_report(NULL, true);
    }
    return;
  }
  if (lb.outCount && cb_connected && ++lb.flushFrames < 300)
    return;
  lb_report(r, false);
}

/* ---- the end: the changes this game needs, then the save ---- */
static void cb_ended(const uint8_t *r, int caughtBy, const uint8_t *caught, bool left) {
  (void)left;
  int list[4], n = 0;
  cb.wantSave = false;
  if (cb.host && caught) {
    if (caughtBy == 1 && cb_mon_ok(caught)) {
      memcpy(cb.giveMon, caught, CB_MON);
      list[n++] = CBO_GIVE;
      cb.wantSave = true;
    } else if (caughtBy == 2) {
      /* the partner's ball: kept until the partner says it has it, resent every few seconds and on a reconnect */
      cb.pending = true;
      cb.pendingId = cb.id;
      memcpy(cb.pendingMon, caught, CB_MON);
      cb.caughtSent = 0;
    }
  }
  if (r) {
    int trainers = r[4] | r[5] << 8 | r[6] | r[7] << 8;
    uint32_t paid = get32(r + 8);
    bool blackout = r[2] & 1;
    if (trainers || paid || blackout || (cb.kind == CB_STORY && r[1] == 1))
      cb.wantSave = true;
  }
  cb_ops_load(list, n);
  cb.phase = CBP_AFTER;
  cb.phaseAt = cbNow;
}

/* One op on a free-field frame; -1 retried, else done. */
static void cb_ops_step(void) {
  const struct coop_api *api = cb_api();
  if (!api || cb.opAt >= cb.opCount)
    return;
  if (cbNow - cb.opsSince > CB_OPS_PATIENCE_MS) {
    cb_notice("SOME CHANGES COULD NOT BE APPLIED.");
    cb.opCount = 0;
    return;
  }
  if (!api->free_field(core))
    return;
  api->set_call(cb_call);
  api->set_cap(cb_cap);
  int op = cb.ops[cb.opAt], r;
  if (op == CBO_GIVE) {
    r = api->give_mon(core, cb.giveMon);
    if (r == 2)
      cb_notice("NO ROOM FOR THE CAUGHT POKéMON.");
  } else if (op == CBO_WHITEOUT) {
    r = api->white_out(core);
  } else {
    r = api->save_game(core);
    if (r == 0 || r == -2)
      cb_notice("THE GAME COULD NOT BE SAVED.");
    if (r == 1 && cb.unsaved) {
      /* the partner's catch is in this game and saved: acknowledged, remembered as given */
      cb_send_id(CB_RESULT_ACK, cb.unsavedId, NULL, 0);
      if (cb.givenCount < 16)
        cb.givenIds[cb.givenCount++] = cb.unsavedId;
      cb.unsaved = false;
    }
  }
  if (r != -1) {
    cb.opAt++;
    cb.opsSince = cbNow;
  }
}

static void cb_after(void) {
  const struct coop_api *api = cb_api();
  if (cb.abortRelease && api) {
    /* a dropped story join: the held script is let go once the game stands on its field, then the blackout and the save */
    api->set_call(cb_call);
    int r = api->release_script(core);
    if (r < 0) {
      if (cbNow - cb.phaseAt > CB_ABORT_WAIT_MS) {
        cb_log("story join: the game never freed: the blackout waits for it");
        cb_reset();
        cb.blackoutOwed = true;
      }
      return;
    }
    cb.abortRelease = false;
    int list[2] = {CBO_WHITEOUT, CBO_SAVE};
    cb_ops_load(list, 2);
    return;
  }
  if (cb.wantSave) {
    cb.wantSave = false;
    if (cb.opCount < 4)
      cb.ops[cb.opCount++] = CBO_SAVE;
  }
  if (cb.opAt >= cb.opCount) {
    cb_reset();
    return;
  }
  cb_ops_step();
}

/* the host's catch of the partner's goes out until acknowledged */
static void cb_resend_caught(void) {
  if (!cb.pending || !cb_connected || (cb.caughtSent && cbNow - cb.caughtSent < CB_CAUGHT_RESEND_MS))
    return;
  cb.caughtSent = cbNow ? cbNow : 1;
  uint8_t body[1 + CB_MON];
  body[0] = 2;
  memcpy(body + 1, cb.pendingMon, CB_MON);
  cb_send_id(CB_CAUGHT_MON, cb.pendingId, body, sizeof body);
}

/* the partner: a catch of ours the host named, given on a free field */
static void cb_caught_message(const struct cb_msg *m) {
  if (m->by != 2)
    return;
  for (int i = 0; i < cb.givenCount; i++)
    if (cb.givenIds[i] == m->id) {
      cb_send_id(CB_RESULT_ACK, m->id, NULL, 0);
      return;
    }
  if ((cb.unsaved && cb.unsavedId == m->id) || (cb.lateGive && cb.lateId == m->id))
    return;
  struct cb_identity me;
  cb_identity(&me);
  struct pd_box_mon b;
  pd_box_mon_read(&b, m->mon);
  if (pd_box_mon_ot_id(&b) != me.otId) {
    cb_log("caught Pokemon refused: another OT ID");
    return;
  }
  cb.lateGive = true;
  cb.lateId = m->id;
  memcpy(cb.lateMon, m->mon, CB_MON);
}

static void cb_late_step(void) {
  const struct coop_api *api = cb_api();
  if (cb.opAt < cb.opCount) {
    cb_ops_step();
    return;
  }
  if (!cb.lateGive || !api || !api->free_field(core))
    return;
  api->set_call(cb_call);
  api->set_cap(cb_cap);
  int r = api->give_mon(core, cb.lateMon);
  if (r == -1)
    return;
  cb.lateGive = false;
  if (r == 2) {
    cb_notice("NO ROOM FOR THE CAUGHT POKéMON.");
    return;
  }
  cb.unsaved = true;
  cb.unsavedId = cb.lateId;
  int save = CBO_SAVE;
  cb_ops_load(&save, 1);
}

/* ---- the host's offer ---- */
static bool cb_nearby(void) {
  int32_t self[PK_PARTNER_SELF_N];
  if (!pk_partner_self(core, self) || !self[PK_PARTNER_SELF_VALID] || !sm.n)
    return false;
  const struct coop_update *last = &sm.buf[sm.n - 1].u;
  if (!last->valid || coop_diff(coopNow, sm.buf[sm.n - 1].arrived) > PD_COOP_STALE_MS)
    return false;
  if (self[PK_PARTNER_SELF_GROUP] != last->group || self[PK_PARTNER_SELF_NUM] != last->num)
    return false;
  int dx = self[PK_PARTNER_SELF_FX] / 16 - last->fx / 16, dy = self[PK_PARTNER_SELF_FY] / 16 - last->fy / 16;
  return abs(dx) + abs(dy) <= CB_RANGE;
}

/* The story's trainers (gym leaders, the League, rivals...: CoopStory.isStoryTrainer, from CoopStoryGen.kt's events and
 * trainer wins): one of them alone is never offered as an everyday battle, its badge / flag steps run by its own script. */
static bool cb_story_trainer(int id) {
  static const uint16_t ids[] = {30,  34,  261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 335, 519, 520,
                                 521, 522, 523, 524, 525, 526, 527, 528, 529, 530, 531, 532, 533, 534, 535, 536, 537,
                                 592, 593, 599, 600, 601, 602, 656, 657, 661, 662, 663, 664, 665, 666, 768, 769};
  for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++)
    if (ids[i] == id)
      return true;
  return false;
}

static bool cb_send_offer(int k, int trainerA, int trainerB, int species);

static void cb_offer_battle(void) {
  const struct coop_api *api = cb_api();
  int trainers[2] = {0, 0};
  api->join_trainers(core, trainers);
  int k = api->join_kind(core);
  if (k == 4 || (k == 3 && cb_story_trainer(trainers[0])))
    k = 0; /* a story trainer: not offered as an everyday battle (a story join is offered where the players stand before it) */
  bool may = k && cb_connected && cb.partnerReady && cb.partnerFree && cb_nearby() && cb.ready;
  char t[128];
  snprintf(t, sizeof t, "battle start held: kind %d, connected %d, partner ready %d free %d, nearby %d, self ready %d", k,
           cb_connected, cb.partnerReady, cb.partnerFree, cb_nearby(), cb.ready);
  cb_log(t);
  if (!may)
    return;
  uint8_t foe[CB_MON];
  int species = 0;
  if (pd_lr_read_memory(core, CB_ENEMY_PARTY, foe, CB_MON)) {
    struct pd_box_mon b;
    pd_box_mon_read(&b, foe);
    species = pd_box_mon_species(&b);
  }
  cb_send_offer(k, trainers[0], trainers[1], species);
}

/* An offer out (kind 1-3 from a held battle start, 4 from a story spot): false if it could not be sent. */
static bool cb_send_offer(int k, int trainerA, int trainerB, int species) {
  struct cb_identity who;
  cb_identity(&who);
  int trainers[2] = {trainerA, trainerB};
  cb.kind = k, cb.trainerA = trainers[0], cb.trainerB = trainers[1];
  do
    cb.id = (uint32_t)rand() & 0x7FFFFFFF;
  while (!cb.id);
  cb.heard = cb.readyEarly = cb.accepted = false;
  cb.host = true;
  uint8_t body[7 + CB_NAME + 1 + 4];
  body[0] = (uint8_t)k;
  put16(body + 1, trainers[0]);
  put16(body + 3, trainers[1]);
  put16(body + 5, species);
  memcpy(body + 7, who.name, CB_NAME);
  body[15] = (uint8_t)who.gender;
  put32(body + 16, who.otId);
  if (!cb_send_id(CB_OFFER, cb.id, body, sizeof body)) {
    cb_reset();
    return false;
  }
  cb.phase = CBP_H_ASKING;
  cb.askedAt = cb.phaseAt = cbNow;
  return true;
}

static void cb_fail_start(int reason, bool tell) {
  char t[96];
  snprintf(t, sizeof t, "start failed: reason %d (%s)", reason, cb.host ? "host" : "partner");
  cb_log(t);
  if (tell)
    cb_send_cancel(cb.id, reason);
  if (reason != CB_DECLINED && reason != CB_LINK)
    cb_notice("THE BATTLE COULD NOT START TOGETHER.");
  lb_forget();
  cb_reset();
}

static void cb_armed(void) {
  cb.phase = CBP_S_ARMED;
  cb.phaseAt = cbNow;
  cb.storySent = -1;
  cb.otherStory = CBS_IDLE;
  cb.storyGoSeen = false;
  cb.tries = 0;
  cb_notice("YOU JOINED. TALK TO THE TRAINER.");
}

static void cb_accepted(void) {
  const struct coop_api *api = cb_api();
  const struct cb_msg *a = &cb.answer;
  memcpy(cb.other.name, a->name, CB_NAME);
  cb.other.gender = a->gender;
  cb.other.otId = a->otId;
  cb.phaseAt = cbNow;
  cb.tries = 0;
  if (!(api->install(core) && api->link_install(core, cb_call)))
    return cb_fail_start(CB_ENGINE, true);
  lb_expect(cb.id);
  if (cb.kind == CB_STORY) {
    /* a story join: the pick hook goes in; each game runs its own script up to the trainer's battle */
    if (!api->story_arm(core, cb_call, (uint16_t)cb.trainerA))
      return cb_fail_start(CB_ENGINE, true);
    cb_armed();
    return;
  }
  cb.phase = cb.readyEarly ? CBP_H_GO : CBP_H_WAIT;
  cb.readyEarly = false;
}

static void cb_setup_other(struct pk_coop_link_setup *s) {
  memset(s, 0, sizeof *s);
  memcpy(s->partnerName, cb.other.name, CB_NAME);
  s->partnerName[7] = 0xFF;
  s->partnerGender = cb.other.gender ? 1 : 0;
  s->partnerOtId = cb.other.otId;
}

static void cb_host_go(void) {
  const struct coop_api *api = cb_api();
  if (!api->host_hold(core))
    return cb_fail_start(CB_ENGINE, true);
  struct pk_coop_link_setup s;
  cb_setup_other(&s);
  cb.lbStory = false;
  int r = lb.active || lb.pumping ? 0 : lb_start(cb.id, api->host_go(core, &s) ? 1 : 0);
  if (r == 1) {
    cb.phase = CBP_RUNNING;
    cb.phaseAt = cbNow;
  } else if (++cb.tries >= CB_MAX_GO_TRIES) {
    cb_fail_start(CB_ENGINE, true);
  }
}

/* ---- the partner ---- */
static void cb_offered(const struct cb_msg *m) {
  const struct coop_api *api = cb_api();
  /* two story offers crossed: the lower battle id yields and takes the other's prompt */
  if (cb.phase == CBP_H_ASKING && cb.kind == CB_STORY && m->kind == CB_STORY && m->id < cb.id) {
    cb_send_cancel(cb.id, CB_BUSY);
    cb_reset();
  }
  bool busy = cb.phase != CBP_IDLE || lb.pumping || !api || !api->partner_free(core);
  if (busy || !cb.ready) {
    cb_send_cancel(m->id, CB_BUSY);
    return;
  }
  cb.offer = *m;
  cb.id = m->id;
  cb.kind = m->kind;
  cb.phase = CBP_P_PROMPT;
  cb.phaseAt = cbNow;
  cb.cursorJoin = true;
}

static void cb_answer(bool accept) {
  const struct coop_api *api = cb_api();
  if (cb.phase != CBP_P_PROMPT || !api)
    return;
  const struct cb_msg *o = &cb.offer;
  uint8_t party[6 * CB_MON], packed[3 * CB_MON];
  int count = cb_party(party), slots[3];
  int n = accept ? cb_able(party, count, slots, packed) : 0;
  bool installed = n > 0 && api->install(core) && api->link_install(core, cb_call);
  bool armed = installed && (o->kind != CB_STORY || api->story_arm(core, cb_call, (uint16_t)o->trainerA));
  if (!armed) {
    uint8_t no = 0;
    cb_send_id(CB_ANSWER, o->id, &no, 1);
    cb_reset();
    return;
  }
  struct cb_identity who;
  cb_identity(&who);
  cb.id = o->id, cb.kind = o->kind, cb.trainerA = o->trainerA, cb.trainerB = o->trainerB;
  memcpy(cb.other.name, o->name, CB_NAME);
  cb.other.gender = o->gender, cb.other.otId = o->otId;
  cb.host = false;
  cb.phaseAt = cbNow;
  cb.tries = 0;
  uint8_t body[2 + 3 * CB_MON + CB_NAME + 1 + 4 + 3];
  int k = 0;
  body[k++] = 1;
  body[k++] = (uint8_t)n;
  memcpy(body + k, packed, (size_t)n * CB_MON);
  k += n * CB_MON;
  memcpy(body + k, who.name, CB_NAME);
  k += CB_NAME;
  body[k++] = (uint8_t)who.gender;
  put32(body + k, who.otId);
  k += 4;
  for (int i = 0; i < n; i++)
    body[k++] = (uint8_t)slots[i];
  if (!cb_send_id(CB_ANSWER, cb.id, body, k)) {
    if (cb.kind == CB_STORY)
      api->story_disarm(core);
    cb_reset();
    return;
  }
  lb_expect(cb.id);
  if (cb.kind == CB_STORY)
    cb_armed();
  else
    cb.phase = CBP_P_START;
}

static void cb_partner_start(void) {
  const struct coop_api *api = cb_api();
  struct pk_coop_link_setup s;
  cb_setup_other(&s);
  s.master = 0;
  s.kind = (uint8_t)cb.kind;
  s.trainerA = (uint16_t)cb.trainerA;
  s.trainerB = (uint16_t)cb.trainerB;
  cb.lbStory = false;
  int r = lb.active || lb.pumping ? 0 : lb_start(cb.id, api->partner_go(core, &s));
  if (r == 1) {
    cb_send_id(CB_LINK_READY, cb.id, NULL, 0);
    cb.phase = CBP_RUNNING;
    cb.phaseAt = cbNow;
  } else if (r == 0) {
    cb_fail_start(CB_ENGINE, true);
  } else if (cbNow - cb.phaseAt >= CB_START_WAIT_MS) {
    cb_fail_start(CB_TIMEOUT, true);
  }
}

/* ---- story joins (CoopLinkFlow's story part): both players stand before the same story trainer ---- */

/* The joinable story event this player stands at (CoopStorySpots.at: the player object's tile less MAP_OFFSET 7; a COORD on its
 * tile, an OBJECT beside it), its trainer, and whether that trainer's flag is set. 0 = none. */
static int cb_story_at(int *trainer, bool *beaten) {
  int32_t self[PK_PARTNER_SELF_N];
  *trainer = 0, *beaten = true;
  if (!pk_partner_self(core, self) || !self[PK_PARTNER_SELF_VALID])
    return 0;
  int tx = self[PK_PARTNER_SELF_FX] / 16 - 7, ty = self[PK_PARTNER_SELF_FY] / 16 - 7;
  for (size_t i = 0; i < sizeof CB_STORY_SPOTS / sizeof CB_STORY_SPOTS[0]; i++) {
    const struct cb_story_spot *e = &CB_STORY_SPOTS[i];
    if (e->group != self[PK_PARTNER_SELF_GROUP] || e->num != self[PK_PARTNER_SELF_NUM])
      continue;
    if (e->object ? abs(e->x - tx) + abs(e->y - ty) != 1 : (e->x != tx || e->y != ty))
      continue;
    *trainer = e->trainer;
    uint32_t sb1 = 0;
    uint8_t b = 0xFF;
    int flag = CB_TRAINER_FLAGS_START + e->trainer;
    if (pd_lr_read_memory(core, 0x03005D8Cu, &sb1, 4) && sb1 >= 0x02000000u && sb1 < 0x02040000u &&
        pd_lr_read_memory(core, sb1 + CB_SB1_FLAGS_OFF + flag / 8, &b, 1))
      *beaten = (b >> (flag % 8)) & 1;
    return e->id;
  }
  return 0;
}

static int cb_story_trainer_of(int eventId) {
  for (size_t i = 0; i < sizeof CB_STORY_SPOTS / sizeof CB_STORY_SPOTS[0]; i++)
    if (CB_STORY_SPOTS[i].id == eventId)
      return CB_STORY_SPOTS[i].trainer;
  return 0;
}

/* IDLE: both at the same story spot, neither has beaten its trainer: the one who stood there first offers (a tie: a per-device wait). */
static void cb_story_idle(void) {
  if (cbNow - cb.lastStoryLook < CB_STORY_LOOK_MS)
    return;
  cb.lastStoryLook = cbNow;
  int trainer;
  bool beaten;
  int mine = cb_story_at(&trainer, &beaten);
  if (mine != cb.storyDone)
    cb.storyDone = 0;
  bool offer = mine && mine == cb.partnerStory && trainer == cb_story_trainer_of(cb.partnerStory) && !beaten &&
               cb.partnerFree && cb.partnerReady && cb_connected && cb.ready && mine != cb.storyDone;
  if (!offer) {
    cb.storySeenAt = 0;
    return;
  }
  if (!cb.storySeenAt) {
    cb.storySeenAt = cbNow ? cbNow : 1;
    return;
  }
  if (cbNow - cb.storySeenAt < cb.token)
    return;
  if (cb_send_offer(CB_STORY, trainer, 0, 0))
    cb.storyDone = mine;
}

/* The pick hook goes out: with a team picked or the screen backed out of, the game's own battle starts; else it is just disarmed. */
static void cb_disarm_story(void) {
  const struct coop_api *api = cb_api();
  int st = api->story_state(core);
  if (st == CBS_READY || st == CBS_CANCELLED)
    api->story_cancel(core);
  else
    api->story_disarm(core);
}

static void cb_cancel_story(bool tell) {
  if (tell)
    cb_send_cancel(cb.id, CB_DECLINED);
  cb_disarm_story();
  cb_notice("THE TEAM BATTLE WAS CALLED OFF.");
  lb_forget();
  cb_reset();
}

/* A story join dropped for good before its battle began is a loss for both, never a solo story battle: the hook stays in until the
 * game reaches it, then goes out WITHOUT the game's own battle, the held script is let go and the game blacks the player out. */
static void cb_story_drop_tick(void) {
  const struct coop_api *api = cb_api();
  int st = api->story_state(core);
  if (st == CBS_READY || st == CBS_CANCELLED) {
    api->story_disarm(core);
    cb.opCount = cb.opAt = 0;
    cb.abortRelease = true;
    cb.storyDrop = false;
    cb.phase = CBP_AFTER;
    cb.phaseAt = cbNow;
  } else if (st == CBS_IDLE || st == CBS_MISMATCH) {
    cb_log("dropped story join: the hook is gone, nothing to black out");
    cb_reset();
  }
}

static void cb_story_tick(void) {
  const struct coop_api *api = cb_api();
  if (cb.storyDrop)
    return cb_story_drop_tick();
  if (cb.armDownAt) {
    if (cbNow - cb.armDownAt < CB_DROP_MS)
      return;
    cb_log("story join: the link was down 60 s");
    cb_notice("THE LINK DROPPED. THE BATTLE WAS UNDONE.");
    lb_forget();
    cb.armDownAt = 0;
    cb.storyDrop = true;
    return;
  }
  int st = api->story_state(core);
  if (st != cb.storySent) {
    cb.storySent = st;
    uint8_t b = (uint8_t)st;
    cb_send_id(CB_STORY_STATE, cb.id, &b, 1);
  }
  if (st == CBS_CANCELLED || st == CBS_MISMATCH || st == CBS_IDLE || cbNow - cb.phaseAt >= CB_STORY_ARM_MS)
    return cb_cancel_story(true);
  if (st != CBS_READY)
    return;
  bool go = cb.host ? cb.otherStory == CBS_READY : cb.storyGoSeen;
  if (!go)
    return;
  struct pk_coop_link_setup s;
  cb_setup_other(&s);
  s.master = cb.host ? 1 : 0;
  int r = lb.active || lb.pumping ? 0 : lb_start(cb.id, api->story_go(core, &s) ? 1 : 0);
  if (r == 1) {
    cb.lbStory = true;
    if (cb.host) {
      uint8_t b = CBS_GO;
      cb_send_id(CB_STORY_STATE, cb.id, &b, 1);
    }
    cb.phase = CBP_RUNNING;
    cb.phaseAt = cbNow;
  } else if (++cb.tries >= CB_MAX_GO_TRIES) {
    cb_cancel_story(true);
  }
}

static void cb_story_message(const struct cb_msg *m, int state) {
  if (m->id != cb.id || cb.phase != CBP_S_ARMED)
    return;
  if (state == CBS_GO) {
    if (!cb.host) {
      cb.storyGoSeen = true;
      cb.tries = 0;
    }
  } else {
    cb.otherStory = state;
  }
}

/* ---- messages and the link ---- */
static void cb_cancelled(int reason) {
  cb.heard = true;
  switch (cb.phase) {
  case CBP_H_ASKING:
    cb_reset();
    break;
  case CBP_H_WAIT:
  case CBP_H_GO:
    cb_notice("YOUR PARTNER LEFT. THE BATTLE WAS UNDONE.");
    lb_forget();
    cb_reset();
    break;
  case CBP_S_ARMED:
    if (!cb.storyDrop) { /* a dropped join stays dropped */
      cb_disarm_story();
      cb_notice("THE TEAM BATTLE WAS CALLED OFF.");
      lb_forget();
      cb_reset();
    }
    break;
  case CBP_P_PROMPT:
    cb_reset();
    break;
  case CBP_P_START:
    lb_forget();
    cb_notice("YOUR PARTNER LEFT. THE BATTLE WAS UNDONE.");
    cb_reset();
    break;
  case CBP_RUNNING:
    if (reason == CB_ENGINE || reason == CB_TIMEOUT || reason == CB_DECLINED || reason == CB_BUSY) {
      cb_notice("THE BATTLE COULD NOT START TOGETHER.");
      if (lb.active)
        lb_finish(false);
    } else {
      lb.cancelled = true;
      if (lb.active && !lb.endedSeen && !lb.down) {
        lb.down = true;
        lb.downAt = cbNow;
      }
    }
    break;
  }
}

RETRO_API void retro_pd_coop_battle_in(int type, const uint8_t *p, size_t n) {
  struct cb_msg m;
  if (!p || n > CB_MAX_PAYLOAD + 64 || !cb_api() || !cb_decode(type, p, (int)n, &m))
    return;
  switch (m.type) {
  case CB_STATUS:
    cb.partnerFree = m.free, cb.partnerReady = m.ready, cb.partnerStory = m.story;
    break;
  case CB_OFFER:
    cb_offered(&m);
    break;
  case CB_ANSWER:
    if (cb.phase == CBP_H_ASKING && m.id == cb.id) {
      cb.heard = true;
      if (m.accept) {
        cb.answer = m;
        cb.accepted = true;
      } else {
        cb_reset();
      }
    }
    break;
  case CB_CANCEL:
    if (m.id == cb.id)
      cb_cancelled(m.reason);
    else if (lb.active && m.id == lb.id)
      cb_cancelled(m.reason);
    break;
  case CB_LINK_READY:
    if (m.id == cb.id) {
      if (cb.phase == CBP_H_WAIT) {
        cb.phase = CBP_H_GO;
        cb.phaseAt = cbNow;
        cb.tries = 0;
      } else if (cb.phase == CBP_H_ASKING && cb.host) {
        cb.readyEarly = true;
      }
    }
    break;
  case CB_LINK_BLOCK:
    lb_block_in(&m);
    break;
  case CB_CAUGHT_MON:
    cb_caught_message(&m);
    break;
  case CB_RESULT_ACK:
    if (cb.pending && m.id == cb.pendingId)
      cb.pending = false;
    break;
  case CB_STORY_STATE:
    cb_story_message(&m, m.story);
    break;
  default:
    break;
  }
}

RETRO_API void retro_pd_coop_battle_link(int connected) {
  bool up = connected != 0;
  if (up == cb_connected)
    return;
  cb_connected = up;
  if (up)
    cb.caughtSent = 0;
  switch (cb.phase) {
  case CBP_H_ASKING:
    if (!up)
      cb_reset();
    break;
  case CBP_H_WAIT:
  case CBP_H_GO:
  case CBP_P_START:
    if (!up)
      cb_fail_start(CB_LINK, false);
    break;
  case CBP_P_PROMPT:
    if (!up)
      cb_reset();
    break;
  case CBP_S_ARMED:
    /* a story join waits a minute for a dead link, then counts as a loss (cb_story_tick) */
    if (!up && !cb.armDownAt)
      cb.armDownAt = cbNow ? cbNow : 1;
    else if (up)
      cb.armDownAt = 0;
    break;
  case CBP_RUNNING:
    if (!up && lb.active && !lb.endedSeen && !lb.down) {
      lb.down = true;
      lb.downAt = cbNow;
    } else if (up && lb.down && !lb.cancelled) {
      lb.down = false;
    }
    break;
  }
}

/* STATUS on change and every second: free = on the field / START menu, ready = co-op installs. */
static void cb_status(void) {
  const struct coop_api *api = cb_api();
  if (!cb_connected || !api) {
    cb.statusOnce = false;
    return;
  }
  bool busy = cb.phase == CBP_IDLE ? cb.blackoutOwed
             : cb.phase != CBP_P_PROMPT && cb.phase != CBP_S_ARMED && !(cb.phase == CBP_H_ASKING && cb.kind == CB_STORY);
  bool fr = !busy && api->partner_free(core);
  if (cbNow - cb.lastSpot >= 250u || !cb.statusOnce) {
    cb.lastSpot = cbNow;
    int trainer;
    bool beaten;
    int at = cb_story_at(&trainer, &beaten);
    cb.storyNow = beaten ? 0 : at;
  }
  if (fr && (!cb.statusOnce || cbNow - cb.lastProbe >= CB_STATUS_MS)) {
    cb.lastProbe = cbNow;
    cb.ready = api->install(core) && api->link_install(core, cb_call);
  }
  bool rdy = cb.ready && !busy;
  if (!cb.statusOnce || fr != cb.sentFree || rdy != cb.sentReady || cb.storyNow != cb.sentStory ||
      cbNow - cb.lastStatus >= CB_STATUS_MS) {
    cb.statusOnce = true;
    cb.lastStatus = cbNow, cb.sentFree = fr, cb.sentReady = rdy, cb.sentStory = cb.storyNow;
    uint8_t body[3] = {(uint8_t)((fr ? 1 : 0) | (rdy ? 2 : 0)), (uint8_t)cb.storyNow, (uint8_t)(cb.storyNow >> 8)};
    cb_send_id(CB_STATUS, 0, body, 3);
  }
}

/* The game is held while the partner is asked / starts (no frame runs). */
static bool cb_hold(void) { return (cb.phase == CBP_H_ASKING && cb.kind != CB_STORY) || cb.phase == CBP_H_WAIT; }

/* The overlay takes the keys while it asks. */
static bool cb_takes_keys(void) { return cb.phase == CBP_P_PROMPT || cb_hold(); }

static uint32_t cb_clock(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}

/* Every tick, before the frame (held or not). */
static void cb_before_frame(void) {
  const struct coop_api *api = cb_api();
  if (!api)
    return;
  cbNow = cb_clock();
  cb_status();
  switch (cb.phase) {
  case CBP_IDLE:
    cb_late_step();
    if (cb.blackoutOwed) {
      /* a story join that dropped while its game never freed: the blackout runs once it does */
      if (api->free_field(core)) {
        cb.blackoutOwed = false;
        int list[2] = {CBO_WHITEOUT, CBO_SAVE};
        cb_ops_load(list, 2);
        cb.phase = CBP_AFTER;
        cb.phaseAt = cbNow;
      }
      break;
    }
    if (!lb.pumping && api->host_hold(core)) {
      if (!cb.startHandled) {
        cb.startHandled = true;
        cb_offer_battle();
      }
    } else {
      cb.startHandled = false;
      if (!lb.pumping)
        cb_story_idle();
    }
    break;
  case CBP_S_ARMED:
    cb_story_tick();
    break;
  case CBP_H_ASKING:
    if (cb.accepted)
      cb_accepted();
    else if (cbNow - cb.askedAt >= CB_PROMPT_MS) {
      if (!cb.heard)
        cb_send_cancel(cb.id, CB_TIMEOUT);
      cb_reset();
    } else if (down(B) && cb.kind != CB_STORY) {
      cb_send_cancel(cb.id, CB_TIMEOUT);
      cb_reset();
    }
    break;
  case CBP_H_WAIT:
    if (cbNow - cb.phaseAt >= CB_START_WAIT_MS)
      cb_fail_start(CB_TIMEOUT, true);
    break;
  case CBP_H_GO:
    cb_host_go();
    break;
  case CBP_P_PROMPT:
    if (cbNow - cb.phaseAt >= CB_PROMPT_MS) {
      cb_reset();
    } else if (down(LEFT) || down(RIGHT) || down(UP) || down(DOWN)) {
      cb.cursorJoin = !cb.cursorJoin;
    } else if (down(A)) {
      cb_answer(cb.cursorJoin);
    } else if (down(B)) {
      cb_answer(false);
    }
    break;
  case CBP_P_START:
    cb_partner_start();
    break;
  case CBP_RUNNING:
    if (!lb.active && !lb.pumping && cb.phase == CBP_RUNNING) {
      cb.phase = CBP_AFTER;
      cb.phaseAt = cbNow;
    }
    break;
  case CBP_AFTER:
    cb_after();
    break;
  }
  cb_resend_caught();
}

/* Something to draw over the game this frame. */
static bool cb_overlay(void) {
  return cb_api() && (cb.phase == CBP_P_PROMPT || cb.phase == CBP_H_ASKING || cb.phase == CBP_H_WAIT ||
                      (lb.active && lb.down && !lb.endedSeen) || (cb.phase == CBP_S_ARMED && cb.armDownAt) || (cbNotice[0] && (int32_t)(cbNoticeUntil - cbNow) > 0));
}

/* After every emulated frame: the block pump. */
static void cb_after_frame(void) { lb_after_frame(); }

/* An Emerald-charset name in plain capitals (the overlay's font). */
static void cb_name_text(const uint8_t *n, char *out, size_t cap) {
  size_t k = 0;
  for (int i = 0; i < CB_NAME && n[i] != 0xFF && k + 1 < cap; i++) {
    uint8_t c = n[i];
    out[k++] = c >= 0xBB && c <= 0xD4 ? (char)('A' + c - 0xBB) : c >= 0xD5 && c <= 0xEE ? (char)('A' + c - 0xD5)
             : c >= 0xA1 && c <= 0xAA ? (char)('0' + c - 0xA1) : c == 0 ? ' ' : c == 0xAD ? '.' : c == 0xAE ? '-' : '?';
  }
  out[k] = 0;
}

static void coop_partner_name(char *out, size_t cap) {
  out[0] = 0;
  if (cb.phase == CBP_P_PROMPT)
    cb_name_text(cb.offer.name, out, cap);
  else if (cb.phase >= CBP_H_WAIT && cb.other.name[0] != 0xFF)
    cb_name_text(cb.other.name, out, cap);
}

/* The prompt, the wait and the notices, over the game (canvas pixels, at the game's scale). */
static void cb_draw(void) {
  if (!cb_api())
    return;
  int s = canvasScale > 0 ? canvasScale : 1;
  char l1[96] = "", l2[96] = "";
  char partner[40];
  coop_partner_name(partner, sizeof partner);
  if (cb.phase == CBP_P_PROMPT) {
    int left = (int)((CB_PROMPT_MS - (cbNow - cb.phaseAt) + 999) / 1000);
    if (cb.kind == CB_STORY)
      snprintf(l1, sizeof l1, "%s WANTS A TEAM BATTLE (%d)", partner[0] ? partner : "?", left < 0 ? 0 : left);
    else
      snprintf(l1, sizeof l1, "%s IS IN A BATTLE (%d)", partner[0] ? partner : "?", left < 0 ? 0 : left);
    snprintf(l2, sizeof l2, "%s JOIN     %s NO", cb.cursorJoin ? ">" : " ", cb.cursorJoin ? " " : ">");
  } else if (cb.phase == CBP_H_ASKING || cb.phase == CBP_H_WAIT) {
    uint32_t total = cb.phase == CBP_H_ASKING ? CB_PROMPT_MS : CB_START_WAIT_MS;
    uint32_t since = cb.phase == CBP_H_ASKING ? cb.askedAt : cb.phaseAt;
    int left = (int)((total - (cbNow - since) + 999) / 1000);
    snprintf(l1, sizeof l1, "ASKING YOUR PARTNER (%d)", left < 0 ? 0 : left);
    if (cb.phase == CBP_H_ASKING && cb.kind != CB_STORY)
      snprintf(l2, sizeof l2, "B: CANCEL");
  } else if ((lb.active && lb.down && !lb.endedSeen) || (cb.phase == CBP_S_ARMED && cb.armDownAt && !cb.storyDrop)) {
    uint32_t at = cb.phase == CBP_S_ARMED ? cb.armDownAt : lb.downAt;
    int left = (int)((CB_DROP_MS - (cbNow - at) + 999) / 1000);
    snprintf(l1, sizeof l1, "WAITING FOR %s... (%d)", partner[0] ? partner : "?", left < 0 ? 0 : left);
  } else if (cbNotice[0] && (int32_t)(cbNoticeUntil - cbNow) > 0) {
    snprintf(l1, sizeof l1, "%s", cbNotice);
  } else {
    return;
  }
  struct pd_canvas small;
  int w = CW / s, h = CH / s;
  static uint32_t px[1024 * 1024 / 4];
  if (w * h > (int)(sizeof px / sizeof px[0]))
    return;
  pd_canvas_init(&small, w, h, px);
  int boxH = l2[0] ? 34 : 20, y = h - boxH - 6;
  /* drawn small, then scaled onto the canvas: the box and its text at the game's own pixel size */
  for (int i = 0; i < w * h; i++)
    px[i] = 0xFF00FF;
  pd_fill(&small, 6, y, w - 12, boxH, 0x203048);
  pd_fill(&small, 7, y + 1, w - 14, boxH - 2, 0x304870);
  pd_text_fit(&small, 12, y + 2, w - 24, l1, 0xFFFFFF, PD_NO_SHADOW);
  if (l2[0])
    pd_text_fit(&small, 12, y + 16, w - 24, l2, 0xF8E070, PD_NO_SHADOW);
  for (int yy = y; yy < y + boxH; yy++)
    for (int xx = 0; xx < w; xx++) {
      uint32_t c = px[yy * w + xx];
      if (c == 0xFF00FF)
        continue;
      for (int dy = 0; dy < s; dy++)
        for (int dx = 0; dx < s; dx++)
          cvs[(yy * s + dy) * CW + xx * s + dx] = c;
    }
}

/* The game closed or changed: nothing carries over (a running link battle was left by the frontend's stop). */
static void cb_game_changed(void) {
  memset(&cb, 0, sizeof cb);
  cb.token = (uint32_t)rand() % 1000u; /* two players reaching a story spot together: the shorter wait offers */
  cb.storySent = -1;
  lb_reset();
  lb.expected = 0;
  cbOut.head = cbOut.tail = cbOut.used = cbOut.count = 0;
}

#ifdef PD_COOP_TEST
/* The headless two-game test's RAM access (never in a shipped build). */
RETRO_API int retro_pd_test_read(uint32_t a, uint8_t *out, int n) { return core && pd_lr_read_memory(core, a, out, (size_t)n); }
RETRO_API void retro_pd_test_write(uint32_t a, const uint8_t *in, int n) {
  for (int i = 0; core && i < n; i++)
    core->busWrite8(core, a + (uint32_t)i, in[i]);
}
RETRO_API int retro_pd_test_phase(void) {
  const struct coop_api *api = cb_api();
  uint8_t r[PK_COOP_LINK_RESULT_SIZE];
  int st = api ? api->state(core) : 0, res = api && api->result(core, r, sizeof r) == PK_COOP_LINK_RESULT_SIZE;
  return cb.phase | (lb.active ? 0x100 : 0) | (lb.pumping ? 0x200 : 0) | (lb.endedSeen ? 0x400 : 0) | (res ? 0x800 : 0) |
         st << 12 | (api && api->free_field(core) ? 0x10000 : 0) | lb.endFrames << 20;
}
#endif

#else
static void cb_before_frame(void) {}
static void cb_after_frame(void) {}
static void cb_draw(void) {}
static bool cb_hold(void) { return false; }
static bool cb_takes_keys(void) { return false; }
static bool cb_overlay(void) { return false; }
static void cb_game_changed(void) {}
RETRO_API void retro_pd_coop_battle_in(int type, const uint8_t *p, size_t n) { (void)type, (void)p, (void)n; }
RETRO_API int retro_pd_coop_battle_out(int *type, uint8_t *out, int cap) { (void)type, (void)out, (void)cap; return 0; }
RETRO_API void retro_pd_coop_battle_link(int connected) { (void)connected; }
#endif
