package com.pokedaisy.app

import com.pokedaisy.app.companion.data.coop.CoopBattle
import com.pokedaisy.app.companion.data.coop.CoopBattleChannel
import com.pokedaisy.app.companion.data.coop.CoopMonCheck
import com.pokedaisy.app.companion.data.decodePartyMon
import com.pokedaisy.app.companion.data.speciesName
import com.pokedaisy.app.companion.i18n.tr
import com.pokedaisy.app.companion.ui.CoopTradeScreen
import com.pokedaisy.app.companion.ui.MenuKey

/**
 * Co-op trades (Emerald, the QUICK MENU's TRADE): two linked players each give one party Pokémon for one of the other's, both
 * games held while they choose. The messages are Capsule's (its libretro core, libretro/pd_coop_trade.inc), byte for byte, so an
 * Android and an iPhone / PC trade together:
 *
 *   initiator                          partner
 *   PICK  --- ASK 0x0220 (its Pokémon) --->  OFFERED (picks one of its own, or says no)
 *   CONFIRM <--- REPLY 0x0221 (its Pokémon)  REPLIED
 *   WAIT_DONE --- CONFIRM 0x0222 (yes) -->   writes the Pokémon in, saves: DONE 0x0223 (resent until ACK 0x0224)
 *   writes the Pokémon in, saves: ACK
 *
 * Each side checks the other's record ([CoopBattle.check]) and that its own slot still holds what it offered. A link that drops
 * after the partner saved leaves the initiator's side pending (in memory): DONE resent on the next link completes it. CANCEL is
 * 0x0225. Every call on the emu thread.
 */
interface CoopTradeGame {
    /** The party's 100-byte records. */
    fun party(): List<ByteArray>
    /** The player's name, 8 bytes in the game's charset, 0xFF-ended. */
    fun name(): ByteArray
    /** On the field / START menu: a trade may start or be answered. */
    fun free(): Boolean
    fun onFreeField(): Boolean
    fun tradeMon(slot: Int, mon: ByteArray): Int
    fun evolveTrade(slot: Int): Int
    fun saveGame(): Int
    /** The save file backed up before the game writes it (CoopGlueEnv.beforeSave). */
    fun beforeSave(): Boolean
}

class CoopTradeFlow(
    private val channel: CoopBattleChannel,
    private val game: CoopTradeGame,
    private val clock: () -> Long,
    private val partnerName: () -> String,
    private val show: (CoopTradeScreen?) -> Unit,
    private val notice: (String) -> Unit,
    private val log: (String) -> Unit = {},
) {
    enum class Phase { IDLE, PICK, ASKED, OFFERED, REPLIED, CONFIRM, WAIT_DONE }

    var phase = Phase.IDLE
        private set
    /** The partner's last STATUS: free on its field. */
    var partnerFree = false

    private var id = 0
    private var phaseAt = 0L
    private var sel = 0
    private var mySlot = -1
    private var myMon = ByteArray(MON)
    private var theirMon = ByteArray(MON)

    // The partner's side once saved: DONE until acknowledged.
    private var doneOwed = false
    private var doneId = 0
    private var doneSent = 0L

    // The initiator's side: what to commit when DONE comes (kept past a timeout).
    private class Pending(val id: Int, val slot: Int, val mine: ByteArray, val theirs: ByteArray)
    private var pending: Pending? = null
    private val committed = ArrayDeque<Int>()

    // After a commit, on free-field frames: the save (then DONE / ACK), a trade evolution, a second save.
    private enum class Op { SAVE_MSG, EVOLVE, SAVE }
    private val ops = ArrayDeque<Op>()
    private var afterSave = 0
    private var afterSaveId = 0
    private var evolveSlot = 0
    private var opsSince = 0L

    /** The game is held: a trade window is up. */
    val hold: Boolean get() = phase != Phase.IDLE
    /** A trade is under way (LOAD STATE refused). */
    val busy: Boolean get() = phase != Phase.IDLE || ops.isNotEmpty() || pending != null

    /** Why TRADE can't start now (null = it can); [battleBusy]: a joined battle is under way. */
    fun why(battleBusy: Boolean): String? = when {
        !channel.connected -> tr("No partner linked")
        battleBusy || busy || !game.free() -> tr("Not now")
        !partnerFree -> tr("Your partner is busy")
        else -> null
    }

    fun start(battleBusy: Boolean) {
        if (why(battleBusy) != null) return
        enter(Phase.PICK)
    }

    private fun enter(p: Phase) {
        phase = p
        phaseAt = clock()
        sel = 0
        redraw()
    }

    private fun reset() {
        phase = Phase.IDLE
        show(null)
    }

    private fun cancel(tell: Boolean, text: String?) {
        if (tell) send(CANCEL, id)
        if (text != null) notice(text)
        reset()
    }

    // --- the wire ---

    private fun send(type: Int, id: Int, body: ByteArray = ByteArray(0)): Boolean {
        val p = ByteArray(4 + body.size)
        put32(p, 0, id)
        body.copyInto(p, 4)
        return channel.send(type, p)
    }

    private fun monOk(mon: ByteArray) = mon.size == MON && CoopBattle.check(mon) == CoopMonCheck.Ok

    private fun nameOk(b: ByteArray, at: Int): Boolean {
        val end = (0 until NAME).firstOrNull { b[at + it] == 0xFF.toByte() } ?: return false
        return (0 until end).all { (b[at + it].toInt() and 0xFF) <= 0xF4 }
    }

    private fun slotHolds(slot: Int, mon: ByteArray): Boolean {
        val party = game.party()
        return slot in party.indices && party[slot].copyOfRange(0, 8).contentEquals(mon.copyOfRange(0, 8))
    }

    /** A message from the partner (types 0x0220-0x0225); anything malformed is dropped. */
    fun onMessage(type: Int, p: ByteArray) {
        if (p.size < 4) return
        val mid = get32(p, 0)
        if (mid == 0) return
        val left = p.size - 4
        when (type) {
            ASK -> {
                if (left != NAME + 1 + MON || !nameOk(p, 4) || (p[4 + NAME].toInt() and 0xFF) > 5) return
                val mon = p.copyOfRange(5 + NAME, 5 + NAME + MON)
                if (!monOk(mon)) return
                if (phase != Phase.IDLE || pending != null || ops.isNotEmpty() || !game.free()) { send(CANCEL, mid); return }
                id = mid
                theirMon = mon
                enter(Phase.OFFERED)
            }
            REPLY -> {
                if (phase != Phase.ASKED || mid != id || left < 1) return
                if (p[4].toInt() == 0 && left == 1) { cancel(false, tr("YOUR PARTNER SAID NO.")); return }
                val ok = p[4].toInt() == 1 && left == 1 + NAME + 1 + MON && nameOk(p, 5) && (p[5 + NAME].toInt() and 0xFF) <= 5
                val mon = if (ok) p.copyOfRange(6 + NAME, 6 + NAME + MON) else null
                if (mon == null || !monOk(mon)) { cancel(true, tr("THE TRADE WAS CALLED OFF.")); return }
                theirMon = mon
                enter(Phase.CONFIRM)
            }
            CONFIRM -> {
                if (phase != Phase.REPLIED || mid != id || left != 1) return
                if (p[4].toInt() == 0) { cancel(false, tr("YOUR PARTNER SAID NO.")); return }
                if (!commit(mySlot, myMon, theirMon, DONE, id)) { cancel(true, tr("THE TRADE WAS CALLED OFF.")); return }
                reset()
            }
            DONE -> {
                if (mid in committed) { send(ACK, mid); return }   // the ACK was lost: again
                val pd = pending ?: return
                if (pd.id != mid) return
                if (commit(pd.slot, pd.mine, pd.theirs, ACK, mid)) pending = null
                else notice(tr("THE TRADE COULD NOT BE FINISHED."))
                if (phase == Phase.WAIT_DONE) reset()
            }
            ACK -> if (doneOwed && mid == doneId) doneOwed = false
            CANCEL -> {
                if (mid != id) return
                when (phase) {
                    Phase.ASKED -> cancel(false, tr("YOUR PARTNER IS BUSY."))
                    Phase.OFFERED, Phase.REPLIED, Phase.CONFIRM -> cancel(false, tr("THE TRADE WAS CALLED OFF."))
                    Phase.WAIT_DONE -> { pending = null; cancel(false, tr("THE TRADE WAS CALLED OFF.")) }
                    else -> {}
                }
            }
        }
    }

    fun linkChanged(up: Boolean) {
        if (up) { doneSent = 0L; return }
        if (phase != Phase.IDLE && phase != Phase.WAIT_DONE) cancel(false, tr("THE LINK DROPPED. THE TRADE WAS CALLED OFF."))
    }

    /** The app is leaving: an open window goes (a pending commit stays, in memory). */
    fun stopNow() {
        if (phase != Phase.IDLE && phase != Phase.WAIT_DONE) send(CANCEL, id)
        if (phase != Phase.IDLE) reset()
    }

    // --- the commit ---

    private fun commit(slot: Int, mine: ByteArray, theirs: ByteArray, msg: Int, tid: Int): Boolean {
        if (!slotHolds(slot, mine)) return false
        if (game.tradeMon(slot, theirs) != 1) return false
        committed.addLast(tid)
        while (committed.size > 8) committed.removeFirst()
        afterSave = msg
        afterSaveId = tid
        evolveSlot = slot
        ops.clear()
        ops.addAll(listOf(Op.SAVE_MSG, Op.EVOLVE, Op.SAVE))
        opsSince = clock()
        return true
    }

    private fun opsStep(now: Long) {
        val op = ops.firstOrNull() ?: return
        if (now - opsSince > OPS_PATIENCE_MS) { ops.clear(); notice(tr("SOME CHANGES COULD NOT BE APPLIED.")); return }
        if (!game.onFreeField()) return
        val r = when (op) {
            Op.EVOLVE -> game.evolveTrade(evolveSlot)
            Op.SAVE, Op.SAVE_MSG -> if (!game.beforeSave()) 0 else game.saveGame()
        }
        if (r == -1) return
        if (op != Op.EVOLVE && r != 1) notice(tr("THE GAME COULD NOT BE SAVED."))
        if (op == Op.SAVE_MSG && r == 1) {
            if (afterSave == DONE) { doneOwed = true; doneId = afterSaveId; doneSent = 0L } else send(ACK, afterSaveId)
            notice(tr("TRADE COMPLETE."))
        }
        ops.removeFirst()
        opsSince = now
    }

    // --- every frame ---

    fun tick() {
        val now = clock()
        if (doneOwed && channel.connected && (doneSent == 0L || now - doneSent >= RESEND_MS)) {
            doneSent = now
            send(DONE, doneId)
        }
        opsStep(now)
        when (phase) {
            Phase.OFFERED, Phase.CONFIRM -> if (now - phaseAt >= ANSWER_MS) {
                send(if (phase == Phase.OFFERED) REPLY else CONFIRM, id, byteArrayOf(0))
                reset()
            } else redraw()
            Phase.ASKED -> if (now - phaseAt >= ANSWER_MS) cancel(true, tr("NO ANSWER FROM YOUR PARTNER.")) else redraw()
            Phase.REPLIED -> if (now - phaseAt >= ANSWER_MS) cancel(true, tr("THE TRADE WAS CALLED OFF.")) else redraw()
            Phase.WAIT_DONE -> if (now - phaseAt >= DONE_MS) {
                notice(tr("THE TRADE WILL FINISH WHEN YOUR PARTNER IS BACK."))
                reset()
            }
            else -> {}
        }
    }

    // --- the window ---

    private fun rows(): Int = when (phase) {
        Phase.PICK -> game.party().size
        Phase.OFFERED -> game.party().size + 1
        Phase.CONFIRM -> 2
        else -> 0
    }

    /** A game button while the window is up. */
    fun key(k: MenuKey) {
        val n = rows()
        when (k) {
            MenuKey.UP -> if (n > 0) { sel = (sel + n - 1) % n; redraw() }
            MenuKey.DOWN -> if (n > 0) { sel = (sel + 1) % n; redraw() }
            MenuKey.A -> pick(sel)
            MenuKey.B -> back()
            else -> {}
        }
    }

    /** A row tapped (or A on it). */
    fun pick(row: Int) {
        val party = game.party()
        when (phase) {
            Phase.PICK, Phase.OFFERED -> {
                if (phase == Phase.OFFERED && row == party.size) { send(REPLY, id, byteArrayOf(0)); reset(); return }
                val m = party.getOrNull(row) ?: return
                if (!monOk(m)) { notice(tr("THAT POKéMON CAN'T BE TRADED.")); return }
                mySlot = row
                myMon = m.copyOf()
                val name = game.name().copyOf(NAME)
                if (phase == Phase.PICK) {
                    id = 0
                    while (id == 0) id = kotlin.random.Random.nextInt() and 0x7FFFFFFF
                    if (!send(ASK, id, name + byteArrayOf(row.toByte()) + m)) { cancel(false, tr("THE TRADE WAS CALLED OFF.")); return }
                    enter(Phase.ASKED)
                } else {
                    if (!send(REPLY, id, byteArrayOf(1) + name + byteArrayOf(row.toByte()) + m)) {
                        cancel(false, tr("THE LINK DROPPED. THE TRADE WAS CALLED OFF.")); return
                    }
                    enter(Phase.REPLIED)
                }
            }
            Phase.CONFIRM -> {
                var yes = row == 0
                if (yes && !slotHolds(mySlot, myMon)) { yes = false; notice(tr("THE TRADE WAS CALLED OFF.")) }
                send(CONFIRM, id, byteArrayOf(if (yes) 1 else 0))
                if (!yes) { reset(); return }
                // From here the partner commits: this side commits when its DONE comes, however late.
                pending = Pending(id, mySlot, myMon.copyOf(), theirMon.copyOf())
                enter(Phase.WAIT_DONE)
            }
            else -> {}
        }
    }

    fun back() {
        when (phase) {
            Phase.PICK -> reset()
            Phase.ASKED -> cancel(true, null)
            Phase.OFFERED -> { send(REPLY, id, byteArrayOf(0)); reset() }
            Phase.CONFIRM -> { send(CONFIRM, id, byteArrayOf(0)); reset() }
            else -> {}
        }
    }

    private fun line(mon: ByteArray): String {
        val m = decodePartyMon(mon, 0)
        val name = m?.nickname ?: m?.let { speciesName(it.species) } ?: "-"
        return tr("{0} Lv{1}", name, mon[LEVEL].toInt() and 0xFF)
    }

    private fun secondsLeft(total: Long): Int = ((total - (clock() - phaseAt) + 999) / 1000).toInt().coerceAtLeast(0)

    private fun redraw() {
        val party = game.party()
        val partner = partnerName()
        show(when (phase) {
            Phase.IDLE -> null
            Phase.PICK -> CoopTradeScreen(tr("TRADE: PICK YOUR POKéMON"), party.map(::line), sel, null)
            Phase.OFFERED -> CoopTradeScreen(
                tr("{0} OFFERS {1}", partner, line(theirMon)), party.map(::line) + tr("DON'T TRADE"), sel, secondsLeft(ANSWER_MS))
            Phase.ASKED -> CoopTradeScreen(tr("WAITING FOR {0}...", partner), listOf(tr("CANCEL")), 0, secondsLeft(ANSWER_MS))
            Phase.REPLIED -> CoopTradeScreen(tr("WAITING FOR {0}...", partner), emptyList(), -1, secondsLeft(ANSWER_MS))
            Phase.CONFIRM -> CoopTradeScreen(
                tr("GIVE {0} FOR {1}?", line(myMon), line(theirMon)), listOf(tr("YES"), tr("NO")), sel, secondsLeft(ANSWER_MS))
            Phase.WAIT_DONE -> CoopTradeScreen(tr("TRADING..."), emptyList(), -1, null)
        })
    }

    companion object {
        const val ASK = 0x0220
        const val REPLY = 0x0221
        const val CONFIRM = 0x0222
        const val DONE = 0x0223
        const val ACK = 0x0224
        const val CANCEL = 0x0225
        val TYPES = ASK..CANCEL

        private const val MON = 100
        private const val NAME = 8
        private const val LEVEL = 0x54
        private const val ANSWER_MS = 60_000L
        private const val DONE_MS = 30_000L
        private const val RESEND_MS = 2_000L
        private const val OPS_PATIENCE_MS = 60_000L

        private fun put32(b: ByteArray, at: Int, v: Int) {
            for (i in 0 until 4) b[at + i] = (v ushr (8 * i)).toByte()
        }

        private fun get32(b: ByteArray, at: Int): Int =
            (b[at].toInt() and 0xFF) or ((b[at + 1].toInt() and 0xFF) shl 8) or
                ((b[at + 2].toInt() and 0xFF) shl 16) or ((b[at + 3].toInt() and 0xFF) shl 24)
    }
}
