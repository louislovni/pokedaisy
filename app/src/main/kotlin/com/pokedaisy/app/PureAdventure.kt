package com.pokedaisy.app

import com.pokedaisy.app.companion.QuickAction

/**
 * Capsule's build (the `capsule` build type, `BuildConfig.PURE_ADVENTURE`): the game as it is, one save per game. No
 * save states (slots, undo, BOSS RETRY, the STATES tab, their hotkeys and QUICK MENU tiles), no rewind, no cheats. The
 * suspend state that resumes a game where it was left stays: it can't take the player back. And the same gameplay helpers
 * as Capsule on iPhone (the player's call): no REUSABLE TMS, NO HMS NEEDED, EXP SHARE, LEVEL CAP, INSTANT TEXT, AUTO REFILL,
 * SHINY ODDS / ALERT, LAST BALL, TRADE EVOS, POKéMON CENTER (the summary's CENTER, the QUICK MENU's HEAL), and the low-HP
 * beep / field poison as the game has them (Prefs' getters, the rows hidden). Every other build: off.
 */
object PureAdventure {
    val on: Boolean get() = BuildConfig.PURE_ADVENTURE

    private val stateHotkeys = setOf(
        Hotkeys.Action.SAVE_STATE, Hotkeys.Action.LOAD_STATE, Hotkeys.Action.UNDO_SAVE, Hotkeys.Action.UNDO_LOAD,
        Hotkeys.Action.SLOT_NEXT, Hotkeys.Action.SLOT_PREV, Hotkeys.Action.REWIND_HOLD,
    )

    /** Whether HOTKEYS lists (and the game runs) this one. */
    fun offers(action: Hotkeys.Action): Boolean = !on || action !in stateHotkeys

    /** Whether the QUICK MENU has this tile. */
    fun offers(action: QuickAction): Boolean =
        !on || (action != QuickAction.SAVE_STATE && action != QuickAction.LOAD_STATE && action != QuickAction.HEAL)

    /** Whether the companion has this tab. */
    fun offersTab(id: String): Boolean = !on || id != "STATES"
}
