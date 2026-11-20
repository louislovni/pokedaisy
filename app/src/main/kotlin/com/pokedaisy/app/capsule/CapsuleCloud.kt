package com.pokedaisy.app.capsule

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Base64
import com.pokedaisy.app.BuildConfig
import com.pokedaisy.app.Prefs
import com.pokedaisy.app.RomArchive
import com.pokedaisy.app.RomCrcs
import com.pokedaisy.app.RomFolder
import com.pokedaisy.app.SaveBackups
import com.pokedaisy.app.SaveStates
import com.pokedaisy.app.SavesLocation
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.InetAddress
import java.net.ServerSocket
import java.net.URL
import java.net.URLDecoder
import java.net.URLEncoder
import java.security.MessageDigest
import java.security.SecureRandom
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/**
 * Capsule's cloud saves (Capsule's build only, [enabled]): the player's Capsule account (Google, through Firebase) and
 * their saves kept in step with the iPhone app and the Windows build - the same files, pieces and rules as Capsule's
 * CloudSync.swift and windows/launcher/cloud.go. A game's save goes by its ROM's name ("Game.sav" for "Game.nds"),
 * which is what ties it to the iPhone's: the ROM files must be named alike everywhere.
 *
 * users/UID/files/ID describes a file (name, SHA-256, size, pieces, version), users/UID/chunks/ID_VERSION_N holds its
 * bytes in base64 pieces. The hash each save had at the last sync says which side changed since: one side only, it goes
 * to the other; both, the newer stays and the other is kept in the backups folder. Blocking calls: never on the UI thread.
 */
object CapsuleCloud {
    private const val API_KEY = "AIzaSyDcwS9Uahx5i8XaUzHmQy2J7dHZbHAj694" // the web app's (public: Firestore's rules guard the data)
    private const val PROJECT = "capsule-bbb39"
    private const val REFERER = "https://louislovni.github.io/"
    private const val CHUNK = 600_000
    private const val BANK = "#banque"
    const val WEB_PAGE = "https://louislovni.github.io/capsule/"

    /** Signing in needs Google's "Desktop app" client, set at build time; without it the build has no cloud. */
    val enabled: Boolean get() = BuildConfig.CAPSULE_GOOGLE_CLIENT_ID.isNotEmpty()

    private fun store(context: Context) = context.getSharedPreferences("capsule_cloud", Context.MODE_PRIVATE)

    /** The signed-in account's e-mail, or null. */
    fun email(context: Context): String? = store(context).getString("email", null)?.takeIf { refreshToken(context) != null }
    private fun refreshToken(context: Context): String? = store(context).getString("refresh", null)
    fun lastSync(context: Context): Long = store(context).getLong("lastSync", 0L)

    fun signOut(context: Context) {
        store(context).edit().clear().apply()
        File(context.filesDir, "capsule-cloud.json").delete()
    }

    // ---- Sign-in: Google's flow for installed apps (the browser, a one-time local address, PKCE)

    @Volatile private var signingIn: Thread? = null

    /**
     * Opens Google's sign-in in the browser and waits (up to 5 minutes, on its own thread) for the answer on a
     * local address; then signs in to Firebase. [done] gets the e-mail, or an error message, on that thread.
     */
    fun signIn(context: Context, done: (email: String?, error: String?) -> Unit) {
        signingIn?.interrupt()
        val app = context.applicationContext
        val server = ServerSocket(0, 1, InetAddress.getByName("127.0.0.1")).apply { soTimeout = 5 * 60_000 }
        val redirect = "http://127.0.0.1:${server.localPort}"
        val verifier = random(48)
        val challenge = Base64.encodeToString(MessageDigest.getInstance("SHA-256").digest(verifier.toByteArray()),
            Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP)
        val state = random(16)
        val auth = "https://accounts.google.com/o/oauth2/v2/auth?" + form(
            "client_id" to BuildConfig.CAPSULE_GOOGLE_CLIENT_ID,
            "redirect_uri" to redirect,
            "response_type" to "code",
            "scope" to "openid email profile",
            "code_challenge" to challenge,
            "code_challenge_method" to "S256",
            "state" to state,
            "prompt" to "select_account",
        )
        signingIn = Thread {
            try {
                val code = server.use { awaitCode(it, state, app.packageName) }
                val tokens = postForm("https://oauth2.googleapis.com/token", form(
                    "code" to code,
                    "client_id" to BuildConfig.CAPSULE_GOOGLE_CLIENT_ID,
                    "client_secret" to BuildConfig.CAPSULE_GOOGLE_CLIENT_SECRET,
                    "redirect_uri" to redirect,
                    "grant_type" to "authorization_code",
                    "code_verifier" to verifier,
                ))
                val googleId = tokens.optString("id_token").ifEmpty { throw IllegalStateException("Google n'a pas renvoyé d'identité") }
                val body = JSONObject()
                    .put("postBody", "id_token=$googleId&providerId=google.com")
                    .put("requestUri", "http://localhost")
                    .put("returnSecureToken", true)
                    .put("returnIdpCredential", true)
                val user = postJson("https://identitytoolkit.googleapis.com/v1/accounts:signInWithIdp?key=$API_KEY", body)
                store(app).edit()
                    .putString("uid", user.getString("localId"))
                    .putString("email", user.optString("email"))
                    .putString("refresh", user.getString("refreshToken"))
                    .apply()
                idToken = user.getString("idToken")
                idTokenUntil = System.currentTimeMillis() + 50 * 60_000
                done(user.optString("email"), null)
            } catch (e: Exception) {
                runCatching { server.close() }
                done(null, e.message ?: e.toString())
            }
        }.apply { isDaemon = true; start() }
        app.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(auth)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }

    /** The browser's request on the local address: its code, after a page that says so (and leads back to the app). */
    private fun awaitCode(server: ServerSocket, state: String, pkg: String): String {
        while (true) {
            server.accept().use { socket ->
                val line = socket.getInputStream().bufferedReader().readLine() ?: return@use
                val query = line.split(' ').getOrNull(1)?.substringAfter('?', "") ?: ""
                val params = query.split('&').filter { '=' in it }.associate {
                    URLDecoder.decode(it.substringBefore('='), "UTF-8") to URLDecoder.decode(it.substringAfter('='), "UTF-8")
                }
                val ok = params["state"] == state && !params["code"].isNullOrEmpty()
                val back = "intent:#Intent;action=android.intent.action.MAIN;category=android.intent.category.LAUNCHER;package=$pkg;end"
                val page = if (ok) page("C'est bon !", "Tu es connecté à Capsule.", back) else page("La connexion n'a pas abouti", "Réessaie depuis Capsule.", back)
                val bytes = page.toByteArray()
                socket.getOutputStream().apply {
                    write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: ${bytes.size}\r\nConnection: close\r\n\r\n".toByteArray())
                    write(bytes)
                    flush()
                }
                if (ok) return params.getValue("code")
                params["error"]?.let { throw IllegalStateException("connexion refusée ($it)") }
            }
        }
    }

    private fun page(title: String, text: String, back: String) = """<!DOCTYPE html><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Capsule</title><body style="font-family:sans-serif;background:#121216;color:#f1f1f4;text-align:center;padding:60px 20px">
<h1 style="color:#ff5a5f">$title</h1><p>$text</p>
<p><a href="$back" style="display:inline-block;background:#e5383b;color:#fff;padding:14px 22px;border-radius:14px;text-decoration:none;font-weight:bold">Revenir à Capsule</a></p>"""

    private fun random(n: Int): String {
        val b = ByteArray(n).also { SecureRandom().nextBytes(it) }
        return Base64.encodeToString(b, Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP).take(n)
    }

    // ---- Tokens

    @Volatile private var idToken: String? = null
    @Volatile private var idTokenUntil = 0L

    private fun token(context: Context): String {
        idToken?.takeIf { System.currentTimeMillis() < idTokenUntil }?.let { return it }
        val refresh = refreshToken(context) ?: throw IllegalStateException("pas connecté")
        val out = postForm("https://securetoken.googleapis.com/v1/token?key=$API_KEY",
            form("grant_type" to "refresh_token", "refresh_token" to refresh))
        out.optString("refresh_token").takeIf { it.isNotEmpty() && it != refresh }?.let {
            store(context).edit().putString("refresh", it).apply()
        }
        idToken = out.getString("id_token")
        idTokenUntil = System.currentTimeMillis() + 50 * 60_000
        return idToken!!
    }

    // ---- HTTP

    class ApiError(val status: Int, message: String) : Exception("serveur $status : $message")

    private fun form(vararg pairs: Pair<String, String>) =
        pairs.joinToString("&") { URLEncoder.encode(it.first, "UTF-8") + "=" + URLEncoder.encode(it.second, "UTF-8") }

    private fun call(method: String, url: String, body: String?, contentType: String?, bearer: String? = null): String {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            requestMethod = method
            connectTimeout = 15_000
            readTimeout = 60_000
            setRequestProperty("Referer", REFERER)
            bearer?.let { setRequestProperty("Authorization", "Bearer $it") }
            if (body != null) {
                doOutput = true
                setRequestProperty("Content-Type", contentType)
            }
        }
        // HttpURLConnection has no PATCH: Firestore takes the method from this header.
        if (method == "PATCH") {
            conn.requestMethod = "POST"
            conn.setRequestProperty("X-HTTP-Method-Override", "PATCH")
        }
        try {
            body?.let { b -> conn.outputStream.use { it.write(b.toByteArray()) } }
            val status = conn.responseCode
            val text = (if (status >= 300) conn.errorStream else conn.inputStream)?.use { it.readBytes().toString(Charsets.UTF_8) } ?: ""
            if (status >= 300) {
                val message = runCatching {
                    val e = JSONObject(text).opt("error")
                    if (e is JSONObject) e.optString("message") else e.toString()
                }.getOrDefault(text.take(200))
                throw ApiError(status, message)
            }
            return text
        } finally {
            conn.disconnect()
        }
    }

    private fun postForm(url: String, body: String) = JSONObject(call("POST", url, body, "application/x-www-form-urlencoded"))
    private fun postJson(url: String, body: JSONObject) = JSONObject(call("POST", url, body.toString(), "application/json"))

    // ---- Firestore (REST)

    private class Doc(val id: String, val fields: JSONObject) {
        fun str(key: String): String? = fields.optJSONObject(key)?.optString("stringValue")?.takeIf { fields.optJSONObject(key)!!.has("stringValue") }
        fun int(key: String): Int = fields.optJSONObject(key)?.optString("integerValue")?.toIntOrNull() ?: 0
        fun time(key: String): Long = fields.optJSONObject(key)?.optString("timestampValue")?.let(::parseTime) ?: 0L
    }

    private class Store(val context: Context, uid: String) {
        val root = "https://firestore.googleapis.com/v1/projects/$PROJECT/databases/(default)/documents/users/$uid/"

        fun request(method: String, path: String, body: JSONObject? = null): String =
            call(method, root + path, body?.toString(), "application/json", token(context))

        fun list(collection: String): List<Doc> {
            val docs = mutableListOf<Doc>()
            var page = ""
            do {
                val out = JSONObject(request("GET", "$collection?pageSize=300" + if (page.isNotEmpty()) "&pageToken=$page" else ""))
                val arr = out.optJSONArray("documents") ?: JSONArray()
                for (i in 0 until arr.length()) docs += toDoc(arr.getJSONObject(i))
                page = out.optString("nextPageToken")
            } while (page.isNotEmpty())
            return docs
        }

        fun get(path: String): Doc? = try {
            toDoc(JSONObject(request("GET", path)))
        } catch (e: ApiError) {
            if (e.status == 404) null else throw e
        }

        fun set(path: String, fields: JSONObject) {
            request("PATCH", path, JSONObject().put("fields", fields))
        }

        fun delete(path: String) {
            request("DELETE", path)
        }

        private fun toDoc(o: JSONObject) = Doc(o.getString("name").substringAfterLast('/'), o.optJSONObject("fields") ?: JSONObject())
    }

    private fun sv(s: String) = JSONObject().put("stringValue", s)
    private fun iv(n: Int) = JSONObject().put("integerValue", n.toString())

    private fun hash(data: ByteArray) = MessageDigest.getInstance("SHA-256").digest(data).joinToString("") { "%02x".format(it) }
    private fun fileId(name: String) = hash(name.toByteArray()).take(32)

    /** The save's hash without mGBA's 16-byte clock footer (a save chip is a multiple of 512 bytes; the footer is
     * what's past the last whole 512). It moves with the clock alone, and the PC's core writes none: two saves with
     * the same core are the same save. */
    private fun coreHash(data: ByteArray): String {
        val n = data.size - data.size % 512
        return if (n == 0) hash(data) else hash(data.copyOf(n))
    }

    private fun stamp(millis: Long): String =
        SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date(millis))

    private fun parseTime(s: String): Long = runCatching {
        val trimmed = s.replace(Regex("\\.(\\d{3})\\d*Z$"), ".$1Z").let { if ('.' in it) it else it.replace("Z", ".000Z") }
        SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.parse(trimmed)!!.time
    }.getOrDefault(0L)

    private fun now(): String =
        SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date())

    /** [modified]: when the save was written here (conflicts go by it, not by when it was uploaded). */
    private fun Store.upload(name: String, data: ByteArray, sum: String, modified: Long, file: String = "") {
        val id = fileId(name)
        val text = Base64.encodeToString(data, Base64.NO_WRAP)
        val pieces = text.chunked(CHUNK).ifEmpty { listOf("") }
        val version = sum.take(12)
        pieces.forEachIndexed { i, piece -> set("chunks/${id}_${version}_$i", JSONObject().put("data", sv(piece))) }
        val old = get("files/$id")
        set("files/$id", JSONObject()
            .put("name", sv(name))
            .put("file", sv(file))
            .put("hash", sv(sum))
            .put("core", sv(coreHash(data)))
            .put("modifiedAt", JSONObject().put("timestampValue", stamp(modified)))
            .put("size", iv(data.size))
            .put("chunks", iv(pieces.size))
            .put("version", sv(version))
            .put("updatedAt", JSONObject().put("timestampValue", now()))
            .put("device", sv("Android " + android.os.Build.MODEL)))
        val oldVersion = old?.str("version")
        if (old != null && !oldVersion.isNullOrEmpty() && oldVersion != version) {
            // The version replaced goes into the game's cloud history (its pieces kept), for every device's history
            // ([syncHistory]); one that can't be kept is deleted as before.
            val entry = JSONObject(old.fields.toString()).put("fileId", sv(id))
            if (runCatching { set("history/${id}_$oldVersion", entry) }.isFailure)
                for (i in 0 until old.int("chunks")) runCatching { delete("chunks/${id}_${oldVersion}_$i") }
        }
    }

    /** A cloud file and its pieces (an old name-keyed copy, once moved to its game's key). */
    private fun Store.remove(d: Doc) {
        val version = d.str("version")
        if (!version.isNullOrEmpty()) for (i in 0 until d.int("chunks")) runCatching { delete("chunks/${d.id}_${version}_$i") }
        delete("files/${d.id}")
    }

    /** A game's cloud name: "rom:" + its ROM's CRC32 (an archive's: the ROM inside), the same on every device that
     * has that ROM whatever its file is called (Capsule's iPhone CloudSync.swift and PC cloud.go do the same). */
    private fun romKey(context: Context, rom: File): String? {
        val crc = RomCrcs.cached(context, rom) ?: run {
            RomCrcs.prepare(context, rom)
            RomCrcs.cached(context, rom)
        } ?: return null
        return "rom:%08X".format(crc)
    }

    /** The versions a game's cloud history keeps (as SAVE HISTORY here: 20). */
    private const val HISTORY_KEEP = 20

    /** The cloud's history for the games here (older versions of their saves, from any device) comes down into
     * their save backups (SAVE HISTORY / RESTORE BACKUP, by when each was saved), each once; one already here (the
     * same core as a backup or the save) is skipped. Past [HISTORY_KEEP] a game's oldest go. Capsule's iPhone app and
     * PC launcher do the same. */
    private fun Store.syncHistory(saveOf: Map<String, File>, manifest: JSONObject) {
        val entries = runCatching { list("history") }.getOrNull() ?: return
        val stampFormat = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US)
        fun date(d: Doc) = d.time("modifiedAt").takeIf { it > 0 } ?: d.time("updatedAt")
        for ((key, all) in entries.groupBy { it.str("name").orEmpty() }) {
            val list = all.sortedByDescending(::date)
            for (d in list.drop(HISTORY_KEEP)) runCatching {
                val version = d.str("version")
                val fileId = d.str("fileId")
                if (version != null && fileId != null) for (i in 0 until d.int("chunks")) runCatching { delete("chunks/${fileId}_${version}_$i") }
                delete("history/${d.id}")
            }
            val save = saveOf[key] ?: continue
            var have: MutableSet<String>? = null   // the cores here: read once a game has something new to look at
            for (d in list.take(HISTORY_KEEP)) {
                if (manifest.optString("history:${d.id}").isNotEmpty()) continue
                val cores = have ?: HashSet<String>().also { set ->
                    runCatching { set += coreHash(save.readBytes()) }
                    SaveBackups.list(save).forEach { f -> runCatching { set += coreHash(f.readBytes()) } }
                    have = set
                }
                if (d.str("core")?.let { it in cores } == true) {
                    manifest.put("history:${d.id}", "1")
                    continue
                }
                val fileId = d.str("fileId") ?: continue
                val data = runCatching { download(d, fileId) }.getOrNull() ?: continue
                val dir = SaveBackups.dir(save)
                val out = File(dir, "${save.nameWithoutExtension}.backup-${stampFormat.format(Date(date(d)))}.${save.extension}")
                if (!out.exists()) {
                    val ok = runCatching { dir.mkdirs(); out.writeBytes(data); out.setLastModified(date(d)) }.isSuccess
                    if (!ok) continue
                }
                cores += coreHash(data)
                manifest.put("history:${d.id}", "1")
            }
        }
    }

    private fun Store.download(d: Doc, fileId: String = d.id): ByteArray {
        val version = d.str("version") ?: throw IllegalStateException("fichier incomplet dans le cloud")
        val sum = d.str("hash") ?: throw IllegalStateException("fichier incomplet dans le cloud")
        val text = StringBuilder()
        for (i in 0 until d.int("chunks")) {
            text.append(get("chunks/${fileId}_${version}_$i")?.str("data") ?: throw IllegalStateException("morceau manquant dans le cloud"))
        }
        val data = Base64.decode(text.toString(), Base64.DEFAULT)
        if (hash(data) != sum) throw IllegalStateException("fichier abîmé dans le cloud")
        return data
    }

    // ---- The sync

    class Report(val received: List<String>, val sent: List<String>, val conflicts: List<String>, val error: String?) {
        val summary: String
            get() = error?.let { "ÉCHEC : $it" } ?: when {
                received.isEmpty() && sent.isEmpty() && conflicts.isEmpty() -> "À JOUR"
                else -> listOfNotNull(
                    received.size.takeIf { it > 0 }?.let { "$it REÇUE${if (it > 1) "S" else ""}" },
                    sent.size.takeIf { it > 0 }?.let { "$it ENVOYÉE${if (it > 1) "S" else ""}" },
                    conflicts.size.takeIf { it > 0 }?.let { "$it CONFLIT${if (it > 1) "S" else ""}" },
                ).joinToString(", ")
            }
    }

    private val lock = Any()

    /** Syncs every library game's save with the account. Blocking; null when not signed in or already syncing. */
    /** While [rom] plays: its save, just written in the game, goes up if it changed here alone (nothing comes down
     * over a save in use, no conflict is settled: the sync back in the library does that). Off the UI thread;
     * failures (offline) wait for the next. */
    fun push(context: Context, prefs: Prefs, rom: File) {
        if (!enabled) return
        val uid = store(context).getString("uid", null) ?: return
        if (refreshToken(context) == null) return
        synchronized(lock) {
            status.value = status.value.copy(syncing = true, pushing = true)
            val report = runCatching { syncLocked(context, prefs, uid, RomArchive.saveNames(rom).first() + ".sav") }.getOrNull()
            val failed = report == null || report.error != null
            status.value = Status(last = if (failed) status.value.last else System.currentTimeMillis(), failed = failed)
        }
    }

    /** The library's cloud line ([Status]): Compose state, set from the sync threads. */
    data class Status(val syncing: Boolean = false, val pushing: Boolean = false, val last: Long = 0L, val failed: Boolean = false)

    val status = androidx.compose.runtime.mutableStateOf(Status())

    fun sync(context: Context, prefs: Prefs): Report? {
        val uid = store(context).getString("uid", null) ?: return null
        if (refreshToken(context) == null) return null
        synchronized(lock) {
            busy = true
            status.value = status.value.copy(syncing = true, pushing = false, last = maxOf(status.value.last, lastSync(context)))
            var report: Report? = null
            try {
                report = syncLocked(context, prefs, uid)
                return report
            } finally {
                busy = false
                val failed = report == null || report.error != null
                status.value = Status(last = lastSync(context), failed = failed)
            }
        }
    }

    private fun syncLocked(context: Context, prefs: Prefs, uid: String, pushOnly: String? = null): Report {
        return run {
            val received = mutableListOf<String>()
            val sent = mutableListOf<String>()
            val conflicts = mutableListOf<String>()
            val manifestFile = File(context.filesDir, "capsule-cloud.json")
            val manifest = runCatching { JSONObject(manifestFile.readText()) }.getOrDefault(JSONObject())
            var error: String? = null
            try {
                val s = Store(context, uid)
                val cloud = s.list("files").mapNotNull { d -> d.str("name")?.takeIf { it != BANK }?.let { it to d } }.toMap()
                // The library's games: each one's cloud name, and where its save is (or goes).
                val library = RomFolder.libraryRoms(context, prefs)
                val games = library.associateBy(
                    { RomArchive.saveNames(it).first() + ".sav" },
                    { SavesLocation.saveFor(context, prefs, it) },
                )
                val roms = library.associateBy { RomArchive.saveNames(it).first() + ".sav" }
                val keysTaken = HashSet<String>()
                val saveOf = HashMap<String, File>()   // a game's key -> its save here, for the cloud's history
                for ((fileName, file) in games) {
                    if (pushOnly != null && fileName != pushOnly) continue
                    // Saves go by their game ([romKey]); one uploaded by its file name (older versions) is the game's
                    // too, the newer of the two if both are there; the old copy goes once this one is synced.
                    val key = roms[fileName]?.let { romKey(context, it) }?.takeIf { keysTaken.add(it) }
                    val name = key ?: fileName
                    saveOf[name] = file
                    val old = if (key != null) cloud[fileName] else null
                    if (key != null && manifest.optString(key).isEmpty() && manifest.optString(fileName).isNotEmpty()) {
                        manifest.put(key, manifest.optString(fileName)).put("core:$key", manifest.optString("core:$fileName").ifEmpty { null })
                    }
                    try {
                        val local = file.takeIf { it.isFile && it.length() in 1 until 4_000_000 }?.readBytes()
                        val keyed = cloud[name]
                        val d = if (old != null && (keyed == null || old.time("updatedAt") > keyed.time("updatedAt"))) old else keyed
                        val localHash = local?.let(::hash)
                        val cloudHash = d?.str("hash")
                        val synced = manifest.optString(name).ifEmpty { null }
                        val syncedCore = manifest.optString("core:$name").ifEmpty { null }
                        val localCore = local?.let(::coreHash)
                        val cloudCore = d?.str("core")
                        val same = localHash == cloudHash || (localCore != null && localCore == cloudCore)
                        if (same && localHash != null) {
                            // The same save (maybe with another clock footer): nothing to send.
                            manifest.put(name, localHash).put("core:$name", localCore)
                        }
                        // Changed since the last sync: by the save's core when the last sync noted it, else by the whole file.
                        var localChanged = localHash != null && localHash != synced
                        if (localChanged && syncedCore != null) localChanged = localCore != syncedCore
                        var cloudChanged = cloudHash != null && cloudHash != synced
                        if (cloudChanged && syncedCore != null && cloudCore != null) cloudChanged = cloudCore != syncedCore
                        val goesUp = local != null && (cloudHash == null || (localChanged && !cloudChanged))
                        when {
                            same -> {}
                            pushOnly != null && !goesUp -> {}
                            local != null && (cloudHash == null || (localChanged && !cloudChanged)) -> {
                                s.upload(name, local, localHash!!, file.lastModified(), fileName)
                                manifest.put(name, localHash).put("core:$name", localCore)
                                sent += fileName.removeSuffix(".sav")
                            }
                            d != null && (local == null || (cloudChanged && !localChanged)) -> {
                                val theirs = s.download(d)
                                write(context, file, theirs, keepOld = local != null)
                                freshBoot(context, roms[fileName])
                                manifest.put(name, cloudHash).put("core:$name", coreHash(theirs))
                                received += fileName.removeSuffix(".sav")
                            }
                            local != null && d != null -> {
                                // Changed on both sides: the save made last goes on (when each was written, not
                                // uploaded), the other is kept.
                                val theirs = s.download(d)
                                val cloudDate = d.time("modifiedAt").takeIf { it > 0 } ?: d.time("updatedAt")
                                if (file.lastModified() >= cloudDate) {
                                    backup(context, fileName, "cloud", theirs)
                                    s.upload(name, local, localHash!!, file.lastModified(), fileName)
                                    manifest.put(name, localHash).put("core:$name", localCore)
                                } else {
                                    write(context, file, theirs, keepOld = true)
                                    freshBoot(context, roms[fileName])
                                    manifest.put(name, cloudHash).put("core:$name", coreHash(theirs))
                                }
                                conflicts += fileName.removeSuffix(".sav")
                            }
                        }
                        // The old name-keyed copy, now under the game's key: the save as it stands goes up under the
                        // key (when the old copy was the cloud's only one), then the old copy goes.
                        if (old != null && pushOnly == null) {
                            if (d === old) {
                                val current = file.readBytes()
                                val sum = hash(current)
                                s.upload(name, current, sum, file.lastModified(), fileName)
                                manifest.put(name, sum).put("core:$name", coreHash(current))
                            }
                            s.remove(old)
                        }
                    } catch (e: ApiError) {
                        throw e
                    } catch (e: Exception) {
                        if (error == null) error = "${fileName.removeSuffix(".sav")} : ${e.message}"
                    }
                }
                if (pushOnly == null) {
                    s.syncHistory(saveOf, manifest)
                    store(context).edit().putLong("lastSync", System.currentTimeMillis()).apply()
                }
            } catch (e: Exception) {
                error = e.message ?: e.toString()
            }
            runCatching { manifestFile.writeText(manifest.toString()) }
            Report(received, sent, conflicts, error)
        }
    }

    /** A save that came from the cloud: the game boots from it next time, not from its resume state, which still
     * holds the old save's progress and would write it back over this one (as LOAD SAVE does, GameSaves.load). */
    private fun freshBoot(context: Context, rom: File?) {
        rom ?: return
        runCatching {
            SaveStates(context.getExternalFilesDir(null) ?: context.filesDir, SaveStates.crc32(rom)).freshBootFile.createNewFile()
        }
    }

    /** True while a sync runs: a game waits for it before starting ([awaitIdle]). */
    @Volatile var busy = false
        private set

    /** Blocks until no sync runs (at most [timeoutMs]): a save being written mustn't meet a game starting on it. */
    fun awaitIdle(timeoutMs: Long = 20_000) {
        val end = System.currentTimeMillis() + timeoutMs
        while (busy && System.currentTimeMillis() < end) Thread.sleep(50)
    }

    private fun backupDir(context: Context) = File(context.getExternalFilesDir(null) ?: context.filesDir, "backups/cloud")

    private fun backup(context: Context, name: String, tag: String, data: ByteArray) {
        val dir = backupDir(context).apply { mkdirs() }
        val stamp = SimpleDateFormat("yyyy-MM-dd-HHmmss", Locale.US).format(Date())
        File(dir, "${name.substringBeforeLast('.')}-$tag-$stamp.${name.substringAfterLast('.')}").writeBytes(data)
    }

    private fun write(context: Context, file: File, data: ByteArray, keepOld: Boolean) {
        if (keepOld && file.isFile) backup(context, file.name, "avant-cloud", file.readBytes())
        file.parentFile?.mkdirs()
        val tmp = File(file.parentFile, file.name + ".capsule-tmp")
        tmp.writeBytes(data)
        if (!tmp.renameTo(file)) {
            file.writeBytes(data)
            tmp.delete()
        }
    }
}
