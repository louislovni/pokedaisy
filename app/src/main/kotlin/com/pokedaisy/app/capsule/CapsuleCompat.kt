package com.pokedaisy.app.capsule

import android.content.Context
import com.pokedaisy.app.RomArchive
import com.pokedaisy.app.RomIdentity
import org.json.JSONObject
import java.io.File

/**
 * Capsule's list of the games it knows (COMPATIBLE GAMES, the capsule build only): `compat.json`, the iPhone app's list
 * (louislovni/capsule's tools/gen_compat.py, from this fork's own tables), copied into the APK's assets by Capsule's
 * build (scripts/build_android.sh); without it (a plain PokeDaisy build) [available] is false and nothing shows.
 * Titles and checksums only, never where to get a game. For each game: whether the companion reads it here
 * ([Game.pokedaisy]), whether Capsule's bank opens its save, and a hack's base ROM (the patch goes on it).
 */
object CapsuleCompat {
    class Base(val code: String?, val rev: Int?, val official: String?, val title: String)

    class Game(
        val title: String, val console: String, val hack: Boolean, val version: String, val code: String?,
        val revisions: List<Int>?, val sha1: List<String>, val pokedaisy: Boolean, val bank: String,
        val unverified: Boolean, val note: String?, val base: Base?,
    )

    /** An official cart in the library: its code and revision, or (Game Boy) its entry's title. */
    class Owned(val code: String?, val revision: Int?, val title: String)

    sealed interface BaseState {
        data object Have : BaseState
        data class OtherRevision(val revision: Int) : BaseState
        data object Missing : BaseState
    }

    @Volatile private var loaded: List<Game>? = null

    fun available(context: Context): Boolean = games(context).isNotEmpty()

    fun games(context: Context): List<Game> = loaded ?: runCatching {
        val text = context.assets.open("compat.json").use { it.readBytes().decodeToString() }
        val list = JSONObject(text).getJSONArray("games")
        (0 until list.length()).map { i ->
            val g = list.getJSONObject(i)
            fun str(k: String) = if (g.has(k) && !g.isNull(k)) g.getString(k) else null
            val base = g.optJSONObject("base")?.let { b ->
                Base(
                    b.optString("code").takeIf { b.has("code") && !b.isNull("code") },
                    if (b.has("rev") && !b.isNull("rev")) b.getInt("rev") else null,
                    b.optString("official").takeIf { b.has("official") && !b.isNull("official") },
                    b.getString("title"),
                )
            }
            Game(
                g.getString("title"), g.getString("console"), g.getString("kind") == "hack", str("version").orEmpty(),
                str("code"), g.optJSONArray("revisions")?.let { a -> (0 until a.length()).map(a::getInt) },
                g.optJSONArray("sha1")?.let { a -> (0 until a.length()).map(a::getString) }.orEmpty(),
                g.optBoolean("pokedaisy"), g.optString("bank", "no"), g.optBoolean("unverified"), str("note"), base,
            )
        }
    }.getOrDefault(emptyList()).also { loaded = it }

    /** [rom]'s SHA-1, worked out once per file version (size and date) and kept. Reads the whole file: off the UI thread. */
    fun sha1(context: Context, rom: File): String? {
        val prefs = context.getSharedPreferences("capsule_compat", Context.MODE_PRIVATE)
        val key = "sha1|${rom.absolutePath}|${rom.length()}|${rom.lastModified()}"
        prefs.getString(key, null)?.let { return it }
        return RomIdentity.sha1(rom)?.also { prefs.edit().putString(key, it).apply() }
    }

    /** The cart's code and revision from its header (GBA at 0xAC / 0xBC, DS at 0x0C / 0x1E). */
    fun header(rom: File): Pair<String, Int>? {
        val nds = RomIdentity.isNds(rom)
        val gba = !nds && !RomIdentity.isGameBoy(rom)
        if (!nds && !gba) return null
        val (at, rev) = if (nds) 0x0CL to 0x1EL else 0xACL to 0xBCL
        val code = RomArchive.readAt(rom, at, 4)?.takeIf { it.size == 4 }?.let { String(it, Charsets.US_ASCII) } ?: return null
        val r = RomArchive.readAt(rom, rev, 1)?.firstOrNull()?.toInt()?.and(0xFF) ?: return null
        return code to r
    }

    /** The list's entry for [rom]: its fingerprint first, else its cart's code (an official cart of its own size). */
    fun match(context: Context, rom: File, sha1: String?): Game? {
        val all = games(context)
        if (sha1 != null) all.firstOrNull { sha1 in it.sha1 }?.let { return it }
        val (code, rev) = header(rom) ?: return null
        // A hack keeps its base game's header: only an official cart's own size counts as that cart.
        if (!RomIdentity.isNds(rom) && rom.length() > 16L shl 20) return null
        return all.firstOrNull { !it.hack && it.code == code && (it.revisions?.contains(rev) ?: true) }
    }

    /** Where a hack's [base] stands among the library's official carts. */
    fun state(base: Base, owned: List<Owned>): BaseState {
        if (base.code != null) {
            val same = owned.filter { it.code == base.code }
            if (same.any { base.rev == null || it.revision == base.rev }) return BaseState.Have
            same.firstNotNullOfOrNull { it.revision }?.let { return BaseState.OtherRevision(it) }
            return BaseState.Missing
        }
        return if (owned.any { it.title == base.official }) BaseState.Have else BaseState.Missing
    }
}
