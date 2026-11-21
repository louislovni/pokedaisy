package com.pokedaisy.app.companion.data

import okio.Path

/**
 * Pokémon Chaos Purple (the demo, its Silph Co. fix): another hg-engine build over the US HeartGold, read as a
 * [HeartfeltGold] (the companion's `heartfeltGold` covers both). Checked headless on a player's save (2026-11-21): the US
 * cart's RAM (sSaveDataPtr 0x021D2228, sFieldSysPtr 0x021D4158), overlays 0-128 at retail's addresses, and a save laid out
 * as Heartfelt Gold's to the byte (its SaveData's section table: the bag at 0x644, 0x990 long, the place at 0x1424, the
 * Pokédex at 0x14A8 with the seen bits at +0x400 by species id), so GEN4_HEARTFELT_GOLD reads it. Its species ids are
 * Heartfelt's too (ARCEUS 493, then the eggs and Gen 4's forms, VICTINI 544 .. PECHARUNT 1075), fewer forms after
 * (1393 personal records). What differs is where its data sits:
 *
 * - its type chart: HeartGold's triples with Fairy's rows, in overlay 130 ([TYPE_CHART_OFFSET]);
 * - its party icons' palettes: a byte per species at the start of overlay [ICON_PALETTE_OVERLAY] (GetMonIconPalette,
 *   retail's 0x02074364, jumps to 0x023D9950, which reads 0x023D8000 + species);
 * - its level-up moves: a member of `/a/0/3/3` per species ({ u16 move, u16 level } to 0xFFFF), not one file.
 *
 * Told apart from Heartfelt by its overlay table (149 overlays: [matches]).
 */
object ChaosPurple {
    const val TITLE = "Pokémon Chaos Purple"
    /** The demo with its Silph Co. fix, on the US HeartGold. */
    const val DEMO_SILPH_FIX_SHA1 = "330d9eb56e069df13c09bbcb3c748c269c94a88e"
    val SHA1S = listOf(DEMO_SILPH_FIX_SHA1)

    /** The header's overlay table size (0x54): 149 overlays of 32 bytes. */
    const val OVERLAY_TABLE_SIZE = 149 * 32
    /** Its type chart (122 triples, 0xFF-ended): overlay 130's file + 0x10E89. */
    const val TYPE_CHART_OFFSET = 0x3F2889L
    const val ICON_PALETTE_OVERLAY = 129
    /** Its last personal record. */
    const val LAST_SPECIES = 1392

    /** Whether [read] (cart offset, length) is this build: Heartfelt's header signature, 149 overlays. */
    fun matches(read: (Long, Int) -> ByteArray?): Boolean {
        val h = read(0, 0x60)?.takeIf { it.size == 0x60 } ?: return false
        if (asciiString(h, 0x0C, 4) != HeartfeltGold.CODE || (h[0x1E].toInt() and 0xFF) != HeartfeltGold.REV) return false
        return u32le(h, HeartfeltGold.SIG_OFFSET.toInt()) == HeartfeltGold.SIG && u32le(h, 0x54) == OVERLAY_TABLE_SIZE.toLong()
    }

    fun matches(rom: Path): Boolean = runCatching { withRomFile(rom) { matches(it.cartReader()) } }.getOrDefault(false)
}
