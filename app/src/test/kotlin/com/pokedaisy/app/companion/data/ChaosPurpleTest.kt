package com.pokedaisy.app.companion.data

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import okio.Path.Companion.toOkioPath

/**
 * Chaos Purple (ChaosPurple): read as Heartfelt Gold, its own data's places, from the cart (skips without the ROM:
 * `-DromDir=`'s ../nds/).
 */
class ChaosPurpleTest {
    private val rom = File(File(RETAIL_ROM_DIR).parentFile, "nds/Pokemon Chaos Purple (Demo) (Silph Co. Fix).nds")

    @After
    fun reset() {
        heartfeltGold = false
        HeartfeltGold.data = null
        NdsArt.names = null
        romGameCode = ""
    }

    @Test
    fun `the cart - detection and its tables`() {
        if (!rom.isFile) return
        assertTrue(ChaosPurple.matches(rom.toOkioPath()))
        assertTrue(HeartfeltGold.matches(rom.toOkioPath()))
        val heartfelt = File(rom.parentFile, "Pokemon Heartfelt Gold (v1.0.15).nds")
        if (heartfelt.isFile) assertFalse(ChaosPurple.matches(heartfelt.toOkioPath()))
        val names = NdsArt.readNames(rom.toOkioPath())!!
        assertEquals("Victini", names.species[544])
        assertEquals("Pecharunt", names.species[1075])
        val data = NdsArt.readHeartfelt(rom.toOkioPath())!!
        assertEquals(SpeciesTypes(9, 9), data.speciesTypes[35])        // CLEFAIRY: Fairy
        assertEquals(SpeciesTypes(10, 10), data.speciesTypes[4])        // CHARMANDER: Fire
        assertEquals(200, data.typeChart[9 * 100 + 16])                 // its chart in overlay 130: Fairy beats Dragon
        assertEquals(0, data.typeChart[16 * 100 + 9])
        assertEquals(2, data.moveCategories[45])                        // GROWL: status
    }

    @Test
    fun `the cart - icons, DEX pages, evolutions and level-up moves`() {
        if (!rom.isFile) return
        val dir = kotlin.io.path.createTempDirectory("chaos-art").toFile()
        val before = NdsArt.dir
        try {
            NdsArt.names = NdsArt.readNames(rom.toOkioPath())
            assertTrue(NdsArt.extractTo(rom.toOkioPath(), dir.toOkioPath()))
            NdsArt.dir = dir.toOkioPath()
            for (sp in listOf(4, 494, 700, 1075)) assertTrue("icon $sp", File(dir, "${NdsArt.HEARTFELT_ICONS}/$sp.png").isFile)
            val evo = Gen4Dex.evolutions(GEN4_DEX_HEARTFELT)!!
            assertEquals(listOf(5), evo.getValue(4).map { it.to })
            assertTrue(evo.getValue(133).any { it.to == 750 })
            assertEquals(LevelMove(1, 33), Gen4Dex.levelUp(GEN4_DEX_HEARTFELT, 1)!!.first())   // its own file a species
            assertEquals(544, PokedexSource.entry(POKEDEX_HEARTFELT, 494)!!.species)
        } finally {
            NdsArt.dir = before
            PokedexSource.romChanged()
            dir.deleteRecursively()
        }
    }
}
