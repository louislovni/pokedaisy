/* Co-op trades from the QUICK MENU (Capsule: iPhone / PC; the Android app has none yet): two linked players on Emerald each give
 * one party Pokémon for one of the other's, both games held while they choose. Included by pd_coop_battle.inc (its link, its
 * checks and its free-field ops); the messages are Capsule's own (0x0220-0x0225; the Android app drops them).
 *
 *   initiator                         partner
 *   PICK  --- ASK (its Pokémon) --->  OFFERED (picks one of its own, or says no)
 *   CONFIRM <--- REPLY (its Pokémon)  REPLIED
 *   WAIT_DONE --- CONFIRM (yes) --->  writes the Pokémon in, saves: DONE (resent until ACK)
 *   writes the Pokémon in, saves: ACK
 * Each side checks the other's record (cb_mon_ok) and its own slot still holding what it offered. A link that drops after the
 * partner saved leaves the initiator's side pending (in memory): DONE resent on the next link completes it. GPL-3.0. */

enum { TR_ASK = 0x0220, TR_REPLY, TR_CONFIRM, TR_DONE, TR_ACK, TR_CANCEL };
enum { TRP_IDLE, TRP_PICK, TRP_ASKED, TRP_OFFERED, TRP_REPLIED, TRP_CONFIRM, TRP_WAIT_DONE };
#define TR_ANSWER_MS 60000u
#define TR_DONE_MS 30000u
#define TR_RESEND_MS 2000u

static struct {
  int phase;
  uint32_t id, phaseAt;
  int sel;
  bool host;
  int mySlot;
  uint8_t myMon[CB_MON];
  int theirSlot;
  uint8_t theirMon[CB_MON];
  uint8_t theirName[CB_NAME];
  /* the partner's side once saved: DONE until acknowledged */
  uint32_t doneId, doneSent;
  bool doneOwed;
  /* the initiator's side: what to commit when DONE comes (kept past a timeout) */
  bool pending;
  uint32_t pendingId;
  int pendingSlot;
  uint8_t pendingMine[CB_MON], pendingTheirs[CB_MON];
  uint32_t committed[8];
  int committedN;
  /* the save op's message after a commit */
  int afterSave; /* 0 none, TR_DONE or TR_ACK */
  uint32_t afterSaveId;
  int evolveSlot;
  int skip; /* ticks whose keys are the QUICK MENU's, not the window's */
} tr;

static bool tr_busy(void) { return tr.phase != TRP_IDLE; }
static bool tr_hold(void) { return tr.phase != TRP_IDLE; }

static void tr_reset(void) {
  tr.phase = TRP_IDLE;
  tr.sel = 0;
}

static bool tr_send(int type, uint32_t id, const uint8_t *body, int n) { return cb_send_id(type, id, body, n); }

static bool tr_same_mon(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 8); /* personality + OT ID */ }

/* This player's party slot still holds the Pokémon it offered (the games are held, but a link hiccup is no excuse). */
static bool tr_slot_holds(int slot, const uint8_t *mon) {
  uint8_t party[6 * CB_MON];
  int n = cb_party(party);
  return slot >= 0 && slot < n && tr_same_mon(party + slot * CB_MON, mon);
}

/* Why trading can't start now (NULL = it can). */
static const char *tr_why(void) {
  const struct coop_api *api = cb_api();
  if (!api)
    return "Emerald only";
  if (!cb_connected)
    return "No partner linked";
  if (cb.phase != CBP_IDLE || lb.pumping || tr.pending)
    return "Not now";
  if (!api->partner_free(core))
    return "Not now";
  if (!cb.partnerFree)
    return "Your partner is busy";
  return NULL;
}

static void tr_start(void) {
  if (tr_why())
    return;
  tr.phase = TRP_PICK;
  tr.sel = 0;
  tr.skip = 2;
  tr.host = true;
  tr.phaseAt = cbNow;
}

static void tr_cancel(bool tell, const char *notice) {
  if (tell)
    tr_send(TR_CANCEL, tr.id, NULL, 0);
  if (notice)
    cb_notice(notice);
  tr_reset();
}

/* A Pokémon's nickname (its level is mon[0x54]). */
static void tr_mon_name(const uint8_t *mon, char *out, size_t cap) {
  struct pd_mon m;
  snprintf(out, cap, "%s", pd_decode_party_mon(mon, &m) && m.species ? m.nickname : "-");
}
/* A Pokémon's line: nickname and level. */
static void tr_mon_line(const uint8_t *mon, char *out, size_t cap) {
  char name[40];
  tr_mon_name(mon, name, sizeof name);
  snprintf(out, cap, "%s Lv%d", name, mon[0x54]);
}

/* The commit: the other's Pokémon into this game's slot, then the save (DONE / ACK after it) and a trade evolution. */
static bool tr_commit(int slot, const uint8_t *mine, const uint8_t *theirs, int afterSave, uint32_t id) {
  const struct coop_api *api = cb_api();
  if (!api || !tr_slot_holds(slot, mine))
    return false;
  api->set_call(cb_call);
  api->set_cap(cb_cap);
  if (api->trade_mon(core, slot, theirs) != 1)
    return false;
  if (tr.committedN < 8)
    tr.committed[tr.committedN++] = id;
  tr.afterSave = afterSave;
  tr.afterSaveId = id;
  tr.evolveSlot = slot;
  int list[3] = {CBO_TRADE_SAVE, CBO_EVOLVE_TRADE, CBO_SAVE};
  cb_ops_load(list, 3);
  cb.phase = CBP_AFTER;
  cb.phaseAt = cbNow;
  return true;
}

/* The ops' save of a commit went through (or not: the message waits for a save that does). */
static void tr_saved(bool ok) {
  if (!ok || !tr.afterSave)
    return;
  if (tr.afterSave == TR_DONE) {
    tr.doneOwed = true;
    tr.doneId = tr.afterSaveId;
    tr.doneSent = 0;
  } else {
    tr_send(TR_ACK, tr.afterSaveId, NULL, 0);
  }
  tr.afterSave = 0;
  cb_notice("TRADE COMPLETE.");
}

static void tr_choose(void) {
  uint8_t party[6 * CB_MON];
  int n = cb_party(party);
  if (tr.sel < 0 || tr.sel >= n)
    return;
  const uint8_t *m = party + tr.sel * CB_MON;
  if (!cb_mon_ok(m)) {
    cb_notice("THAT POKéMON CAN'T BE TRADED.");
    return;
  }
  struct cb_identity me;
  cb_identity(&me);
  uint8_t body[CB_NAME + 1 + CB_MON];
  if (tr.phase == TRP_PICK) {
    do
      tr.id = (uint32_t)rand() & 0x7FFFFFFF;
    while (!tr.id);
    tr.mySlot = tr.sel;
    memcpy(tr.myMon, m, CB_MON);
    memcpy(body, me.name, CB_NAME);
    body[CB_NAME] = (uint8_t)tr.sel;
    memcpy(body + CB_NAME + 1, m, CB_MON);
    if (!tr_send(TR_ASK, tr.id, body, sizeof body))
      return tr_cancel(false, "THE TRADE COULD NOT START.");
    tr.phase = TRP_ASKED;
    tr.phaseAt = cbNow;
  } else if (tr.phase == TRP_OFFERED) {
    tr.mySlot = tr.sel;
    memcpy(tr.myMon, m, CB_MON);
    uint8_t reply[1 + CB_NAME + 1 + CB_MON];
    reply[0] = 1;
    memcpy(reply + 1, me.name, CB_NAME);
    reply[1 + CB_NAME] = (uint8_t)tr.sel;
    memcpy(reply + 2 + CB_NAME, m, CB_MON);
    if (!tr_send(TR_REPLY, tr.id, reply, sizeof reply))
      return tr_cancel(false, "THE LINK DROPPED. THE TRADE WAS CALLED OFF.");
    tr.phase = TRP_REPLIED;
    tr.phaseAt = cbNow;
  }
}

static void tr_in(int type, const uint8_t *p, size_t n) {
  if (n < 4)
    return;
  uint32_t id = get32(p);
  const uint8_t *b = p + 4;
  size_t left = n - 4;
  if (!id)
    return;
  switch (type) {
  case TR_ASK: {
    if (left != CB_NAME + 1 + CB_MON || !cb_valid_name(b) || b[CB_NAME] > 5 || !cb_mon_ok(b + CB_NAME + 1))
      return;
    const struct coop_api *api = cb_api();
    bool busy = tr_busy() || tr.pending || cb.phase != CBP_IDLE || lb.pumping || !api || !api->partner_free(core);
    if (busy) {
      tr_send(TR_CANCEL, id, NULL, 0);
      return;
    }
    tr.id = id;
    tr.host = false;
    memcpy(tr.theirName, b, CB_NAME);
    tr.theirSlot = b[CB_NAME];
    memcpy(tr.theirMon, b + CB_NAME + 1, CB_MON);
    tr.phase = TRP_OFFERED;
    tr.phaseAt = cbNow;
    tr.sel = 0;
    break;
  }
  case TR_REPLY:
    if (tr.phase != TRP_ASKED || id != tr.id || left < 1)
      return;
    if (b[0] == 0 && left == 1)
      return tr_cancel(false, "YOUR PARTNER SAID NO.");
    if (b[0] != 1 || left != 1 + CB_NAME + 1 + CB_MON || !cb_valid_name(b + 1) || b[1 + CB_NAME] > 5 ||
        !cb_mon_ok(b + 2 + CB_NAME))
      return tr_cancel(true, "THE TRADE WAS CALLED OFF.");
    memcpy(tr.theirName, b + 1, CB_NAME);
    tr.theirSlot = b[1 + CB_NAME];
    memcpy(tr.theirMon, b + 2 + CB_NAME, CB_MON);
    tr.phase = TRP_CONFIRM;
    tr.phaseAt = cbNow;
    tr.sel = 0;
    break;
  case TR_CONFIRM:
    if (tr.phase != TRP_REPLIED || id != tr.id || left != 1)
      return;
    if (!b[0])
      return tr_cancel(false, "YOUR PARTNER SAID NO.");
    if (!tr_commit(tr.mySlot, tr.myMon, tr.theirMon, TR_DONE, tr.id))
      return tr_cancel(true, "THE TRADE WAS CALLED OFF.");
    tr_reset();
    break;
  case TR_DONE:
    for (int i = 0; i < tr.committedN; i++)
      if (tr.committed[i] == id) {
        tr_send(TR_ACK, id, NULL, 0); /* the ACK was lost: again */
        return;
      }
    if (tr.pending && id == tr.pendingId) {
      if (tr_commit(tr.pendingSlot, tr.pendingMine, tr.pendingTheirs, TR_ACK, id))
        tr.pending = false;
      else
        cb_notice("THE TRADE COULD NOT BE FINISHED.");
      if (tr.phase == TRP_WAIT_DONE)
        tr_reset();
    }
    break;
  case TR_ACK:
    if (tr.doneOwed && id == tr.doneId)
      tr.doneOwed = false;
    break;
  case TR_CANCEL:
    if (id != tr.id)
      return;
    if (tr.phase == TRP_ASKED || tr.phase == TRP_OFFERED || tr.phase == TRP_CONFIRM || tr.phase == TRP_REPLIED)
      tr_cancel(false, tr.phase == TRP_ASKED ? "YOUR PARTNER IS BUSY." : "THE TRADE WAS CALLED OFF.");
    else if (tr.phase == TRP_WAIT_DONE) {
      tr.pending = false;
      tr_cancel(false, "THE TRADE WAS CALLED OFF.");
    }
    break;
  }
}

static void tr_link(bool up) {
  if (up) {
    tr.doneSent = 0;
    return;
  }
  if (tr.phase == TRP_PICK || tr.phase == TRP_ASKED || tr.phase == TRP_OFFERED || tr.phase == TRP_REPLIED ||
      tr.phase == TRP_CONFIRM)
    tr_cancel(false, "THE LINK DROPPED. THE TRADE WAS CALLED OFF.");
}

/* Every tick, before the frame. */
static void tr_tick(void) {
  if (tr.doneOwed && cb_connected && (!tr.doneSent || cbNow - tr.doneSent >= TR_RESEND_MS)) {
    tr.doneSent = cbNow ? cbNow : 1;
    tr_send(TR_DONE, tr.doneId, NULL, 0);
  }
  if (!tr.phase)
    return;
  if (tr.skip > 0) {
    tr.skip--;
    return;
  }
  uint8_t party[6 * CB_MON];
  int n = cb_party(party);
  int rows = tr.phase == TRP_OFFERED ? n + 1 : tr.phase == TRP_CONFIRM ? 2 : n;
  switch (tr.phase) {
  case TRP_PICK:
  case TRP_OFFERED:
  case TRP_CONFIRM:
    if (rows > 0 && down(UP))
      tr.sel = (tr.sel + rows - 1) % rows;
    if (rows > 0 && down(DOWN))
      tr.sel = (tr.sel + 1) % rows;
    if (tr.phase == TRP_OFFERED && cbNow - tr.phaseAt >= TR_ANSWER_MS) {
      uint8_t no = 0;
      tr_send(TR_REPLY, tr.id, &no, 1);
      tr_reset();
    } else if (tr.phase == TRP_CONFIRM && cbNow - tr.phaseAt >= TR_ANSWER_MS) {
      uint8_t no = 0;
      tr_send(TR_CONFIRM, tr.id, &no, 1);
      tr_reset();
    } else if (down(B)) {
      if (tr.phase == TRP_PICK) {
        tr_reset();
      } else {
        uint8_t no = 0;
        tr_send(tr.phase == TRP_OFFERED ? TR_REPLY : TR_CONFIRM, tr.id, &no, 1);
        tr_reset();
      }
    } else if (down(A)) {
      if (tr.phase == TRP_OFFERED && tr.sel == n) {
        uint8_t no = 0;
        tr_send(TR_REPLY, tr.id, &no, 1);
        tr_reset();
      } else if (tr.phase == TRP_CONFIRM) {
        uint8_t yes = tr.sel == 0;
        if (yes && !tr_slot_holds(tr.mySlot, tr.myMon)) {
          yes = 0;
          cb_notice("THE TRADE WAS CALLED OFF.");
        }
        tr_send(TR_CONFIRM, tr.id, &yes, 1);
        if (!yes) {
          tr_reset();
        } else {
          /* from here the partner commits: this side commits when its DONE comes, however late */
          tr.pending = true;
          tr.pendingId = tr.id;
          tr.pendingSlot = tr.mySlot;
          memcpy(tr.pendingMine, tr.myMon, CB_MON);
          memcpy(tr.pendingTheirs, tr.theirMon, CB_MON);
          tr.phase = TRP_WAIT_DONE;
          tr.phaseAt = cbNow;
        }
      } else {
        tr_choose();
      }
    }
    break;
  case TRP_ASKED:
    if (cbNow - tr.phaseAt >= TR_ANSWER_MS)
      tr_cancel(true, "NO ANSWER. YOUR PARTNER NEEDS CAPSULE TO TRADE.");
    else if (down(B))
      tr_cancel(true, NULL);
    break;
  case TRP_REPLIED:
    if (cbNow - tr.phaseAt >= TR_ANSWER_MS)
      tr_cancel(true, "THE TRADE WAS CALLED OFF.");
    break;
  case TRP_WAIT_DONE:
    if (cbNow - tr.phaseAt >= TR_DONE_MS) {
      /* the game goes on; the partner's DONE still completes the trade when it comes */
      cb_notice("THE TRADE WILL FINISH WHEN YOUR PARTNER IS BACK.");
      tr_reset();
    }
    break;
  }
}

/* The trade's window over the game (the small canvas, game pixels). */
static bool tr_draw_small(struct pd_canvas *c, int w, int h) {
  if (!tr.phase)
    return false;
  char partner[24], title[96] = "", foot[64] = "", line[64];
  cb_name_text(tr.theirName, partner, sizeof partner);
  uint8_t party[6 * CB_MON];
  int n = cb_party(party);
  const char *rows[8];
  char rowBuf[8][96];
  int rowsN = 0;
  bool list = false;
  switch (tr.phase) {
  case TRP_PICK:
    snprintf(title, sizeof title, "TRADE: PICK YOUR POKéMON");
    snprintf(foot, sizeof foot, "A: OFFER   B: BACK");
    list = true;
    break;
  case TRP_OFFERED:
    tr_mon_name(tr.theirMon, line, sizeof line);
    snprintf(title, sizeof title, "%s OFFERS %s Lv%d", partner, line, tr.theirMon[0x54]);
    snprintf(foot, sizeof foot, "A: GIVE THIS ONE   B: NO");
    list = true;
    break;
  case TRP_ASKED: {
    int left = (int)((TR_ANSWER_MS - (cbNow - tr.phaseAt) + 999) / 1000);
    snprintf(title, sizeof title, "WAITING FOR YOUR PARTNER (%d)", left < 0 ? 0 : left);
    snprintf(foot, sizeof foot, "B: CANCEL");
    break;
  }
  case TRP_REPLIED:
    snprintf(title, sizeof title, "WAITING FOR %s TO CONFIRM", partner);
    break;
  case TRP_CONFIRM: {
    char mine[48];
    tr_mon_name(tr.myMon, mine, sizeof mine);
    tr_mon_name(tr.theirMon, line, sizeof line);
    snprintf(title, sizeof title, "GIVE %s Lv%d", mine, tr.myMon[0x54]);
    snprintf(rowBuf[0], sizeof rowBuf[0], "FOR %s Lv%d?", line, tr.theirMon[0x54]);
    rows[rowsN++] = rowBuf[0];
    snprintf(foot, sizeof foot, "A: CHOOSE   B: NO");
    break;
  }
  case TRP_WAIT_DONE:
    snprintf(title, sizeof title, "TRADING...");
    break;
  }
  if (list) {
    for (int i = 0; i < n && rowsN < 6; i++) {
      tr_mon_line(party + i * CB_MON, rowBuf[rowsN], sizeof rowBuf[rowsN]);
      rows[rowsN] = rowBuf[rowsN];
      rowsN++;
    }
    if (tr.phase == TRP_OFFERED) {
      snprintf(rowBuf[rowsN], sizeof rowBuf[rowsN], "DON'T TRADE");
      rows[rowsN] = rowBuf[rowsN];
      rowsN++;
    }
  }
  int choices = tr.phase == TRP_CONFIRM ? 2 : 0;
  int lines = 1 + rowsN + choices + (foot[0] ? 1 : 0);
  int boxH = 6 + lines * 13, boxW = w - 24, x = 12, y = (h - boxH) / 2;
  if (y < 2)
    y = 2;
  pd_fill(c, x, y, boxW, boxH, 0x203048);
  pd_fill(c, x + 1, y + 1, boxW - 2, boxH - 2, 0x304870);
  int ty = y + 3;
  pd_text_fit(c, x + 6, ty, boxW - 12, title, 0xFFFFFF, PD_NO_SHADOW);
  ty += 13;
  for (int i = 0; i < rowsN; i++, ty += 13) {
    bool sel = list && i == tr.sel;
    if (sel)
      pd_fill(c, x + 4, ty - 1, boxW - 8, 12, 0x5070A8);
    pd_text_fit(c, x + 8, ty, boxW - 16, rows[i], sel ? 0xF8E070 : 0xFFFFFF, PD_NO_SHADOW);
  }
  if (choices) {
    const char *yn[2] = {"YES", "NO"};
    for (int i = 0; i < 2; i++, ty += 13) {
      if (i == tr.sel)
        pd_fill(c, x + 4, ty - 1, boxW - 8, 12, 0x5070A8);
      pd_text_fit(c, x + 8, ty, boxW - 16, yn[i], i == tr.sel ? 0xF8E070 : 0xFFFFFF, PD_NO_SHADOW);
    }
  }
  if (foot[0])
    pd_text_fit(c, x + 6, ty, boxW - 12, foot, 0xC8D0E0, PD_NO_SHADOW);
  return true;
}
