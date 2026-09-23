// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.openpak.model

import android.content.Context
import android.os.Build
import android.text.format.Formatter
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.BuildConfig
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.settings.model.BooleanSetting
import org.dolphinemu.dolphinemu.features.settings.model.Settings
import org.dolphinemu.dolphinemu.model.GameFile
import org.dolphinemu.dolphinemu.services.GameFileCacheManager
import org.dolphinemu.dolphinemu.ui.platform.Platform
import org.json.JSONArray
import org.json.JSONObject
import java.text.DateFormat
import java.time.OffsetDateTime
import java.util.Date
import java.util.Locale

/**
 * OpenPak as the Dolphin app sees it: the Wii family (Nintendo WFC). An account signs in without
 * a console identity; cloud saves, the friends list and the status page work from it.
 *
 * Every suspend function here runs its native call on Dispatchers.IO: the network is never
 * touched from the main thread.
 */
object OpenPak {
    const val EMULATOR = "Dolphin"

    data class State(
        val signedIn: Boolean,
        val name: String,
        val website: String,
        val statusUrl: String
    )

    data class Profile(
        val name: String,
        val accountId: String,
        val friendCode: String,
        val image: String,
        val linked: List<String>
    )

    data class Friend(
        val accountId: String,
        val name: String,
        val friendCode: String,
        val online: Boolean,
        val titleId: String,
        val console: String,
        val onlineSince: Long,
        val friendsSince: Long
    )

    data class SaveVersion(
        val id: Long,
        val number: Int,
        val conflict: Boolean,
        val size: Long,
        val device: String,
        val savedAt: String
    )

    data class CloudSave(
        val titleId: String,
        val name: String,
        val versions: List<SaveVersion>,
        val localState: String,
        val localVersion: String,
        val lastWritten: Long
    ) {
        val newest: SaveVersion? get() = versions.firstOrNull()
        val conflict: Boolean get() = localState == "no_history" || versions.any { it.conflict }
    }

    data class CloudSaves(val saves: List<CloudSave>, val used: Long, val allowance: Long)

    data class Mod(
        val id: String,
        val name: String,
        val version: String,
        val author: String,
        val licence: String,
        val summary: String
    )

    data class Service(val name: String, val up: Boolean, val uptime: Double, val latency: String)

    data class Count(val id: String, val players: Int)

    data class Status(
        val ok: Boolean,
        val url: String,
        val headline: String,
        val sub: String,
        val state: String,
        val services: List<Service>,
        val playersOk: Boolean,
        val players: Int,
        val titles: List<Count>,
        val networks: List<Count>,
        val refreshed: Long
    )

    data class Connection(val pingMs: Int?, val nat: String?)

    /** A result the screens show: the data, or a sentence from the string table. */
    sealed class Answer<out T> {
        data class Ok<T>(val value: T) : Answer<T>()
        data class Failed(val message: String) : Answer<Nothing>()
    }

    private const val PREFS = "openpak"
    private const val KEY_CONNECT_ASKED = "connect_asked"

    /** Once per process, after Dolphin's own directories and config are ready. */
    @JvmStatic
    fun init(context: Context) {
        OpenPakNative.init(BuildConfig.VERSION_NAME, defaultDeviceName(context))
    }

    fun defaultDeviceName(context: Context): String =
        context.getString(R.string.openpak_signin_device_default_single)
            .replace("{emulator}", EMULATOR)
            .replace("{machine}", Build.MODEL)

    fun state(): State {
        val json = JSONObject(OpenPakNative.state())
        return State(
            signedIn = json.optBoolean("signed_in"),
            name = json.optString("name"),
            website = json.optString("website", "https://openpak.org"),
            statusUrl = json.optString("status_url")
        )
    }

    /** "Signed in as {name}" or "Not signed in", for the settings rows. */
    fun accountLine(context: Context): String {
        val state = state()
        return if (state.signedIn) context.getString(R.string.openpak_menu_signed_in_as, state.name)
        else context.getString(R.string.openpak_status_signed_out)
    }

    /** Whether a game is running: sign-in, sign-out and the enable switch wait until it stops. */
    fun gameRunning(): Boolean = !org.dolphinemu.dolphinemu.NativeLibrary.IsUninitialized()

    // ---- first run ----

    fun connectAsked(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY_CONNECT_ASKED, false)

    fun setConnectAsked(context: Context) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putBoolean(KEY_CONNECT_ASKED, true).apply()
    }

    /** After a sign-in from the connect screen: this family's connection and cloud sync go on. */
    suspend fun turnOnConnection() = withContext(Dispatchers.IO) {
        Settings().use { settings ->
            settings.loadSettings()
            BooleanSetting.MAIN_WII_OPENPAK_ENABLE.setBoolean(settings, true)
            BooleanSetting.MAIN_OPENPAK_CLOUD_SAVE.setBoolean(settings, true)
            settings.saveSettings()
        }
        // The profile fetch at start was skipped while the connection was off.
        OpenPakNative.refreshNetwork()
    }

    // ---- account ----

    sealed class SignInResult {
        data class Ok(val name: String) : SignInResult()
        data class Failed(val message: String) : SignInResult()
    }

    suspend fun signIn(context: Context, email: String, password: String, device: String): SignInResult =
        withContext(Dispatchers.IO) {
            val json = JSONObject(OpenPakNative.signIn(email.trim(), password, device.trim()))
            if (json.optBoolean("ok")) {
                SignInResult.Ok(json.optString("name"))
            } else {
                val website = state().website
                SignInResult.Failed(
                    when (json.optString("code")) {
                        "credentials" -> context.getString(R.string.openpak_error_credentials)
                        "rate_limited" -> context.getString(R.string.openpak_error_rate_limited)
                        "server" -> context.getString(
                            R.string.openpak_error_server, json.optString("message")
                        )

                        else -> context.getString(R.string.openpak_error_unreachable, host(website))
                    }
                )
            }
        }

    suspend fun signOut() = withContext(Dispatchers.IO) { OpenPakNative.signOut() }

    suspend fun profile(context: Context): Answer<Profile> = withContext(Dispatchers.IO) {
        val json = JSONObject(OpenPakNative.profile())
        if (!json.optBoolean("ok")) return@withContext failed(context, json.optString("error"))
        Answer.Ok(
            Profile(
                name = json.optString("name"),
                accountId = json.optString("account_id"),
                friendCode = json.optString("friend_code"),
                image = json.optString("image"),
                linked = json.optJSONArray("linked").strings()
            )
        )
    }

    suspend fun friends(context: Context): Answer<List<Friend>> = withContext(Dispatchers.IO) {
        val json = JSONObject(OpenPakNative.friends())
        if (!json.optBoolean("ok")) return@withContext failed(context, json.optString("error"))
        val array = json.optJSONArray("friends") ?: JSONArray()
        val friends = (0 until array.length()).map { i ->
            val f = array.getJSONObject(i)
            Friend(
                accountId = f.optString("account_id"),
                name = f.optString("name"),
                friendCode = f.optString("friend_code"),
                online = f.optBoolean("online"),
                titleId = f.optString("title_id"),
                console = f.optString("console"),
                onlineSince = f.optLong("online_since"),
                friendsSince = f.optLong("friends_since")
            )
        }
        // Online friends first, then alphabetical.
        Answer.Ok(friends.sortedWith(compareBy<Friend> { !it.online }.thenBy(String.CASE_INSENSITIVE_ORDER) { it.name }))
    }

    suspend fun cloudSaves(context: Context): Answer<CloudSaves> = withContext(Dispatchers.IO) {
        val json = JSONObject(OpenPakNative.cloudSaves())
        if (!json.optBoolean("ok")) return@withContext failed(context, json.optString("error"))
        val array = json.optJSONArray("saves") ?: JSONArray()
        val names = localGameNames()
        val saves = (0 until array.length()).map { i ->
            val s = array.getJSONObject(i)
            val versionArray = s.optJSONArray("versions") ?: JSONArray()
            val versions = (0 until versionArray.length()).map { j ->
                val v = versionArray.getJSONObject(j)
                SaveVersion(
                    id = v.optLong("id"),
                    number = v.optInt("number"),
                    conflict = v.optBoolean("conflict"),
                    size = v.optLong("size"),
                    device = v.optString("device"),
                    savedAt = v.optString("saved_at")
                )
            }
            val titleId = s.optString("title_id").lowercase(Locale.ROOT)
            val local = s.optJSONObject("local") ?: JSONObject()
            val serverName = s.optString("name")
            CloudSave(
                titleId = titleId,
                // The site falls back to the title id when it does not know the game.
                name = names[titleId] ?: serverName.ifEmpty { titleId },
                versions = versions,
                localState = local.optString("state", "none"),
                localVersion = local.optString("version"),
                lastWritten = local.optLong("last_written")
            )
        }
        Answer.Ok(CloudSaves(saves, json.optLong("used"), json.optLong("allowance")))
    }

    suspend fun deleteSave(save: CloudSave): String = withContext(Dispatchers.IO) {
        OpenPakNative.deleteSave(save.versions.map { it.id }.toLongArray())
    }

    suspend fun downloadSave(save: CloudSave): String =
        withContext(Dispatchers.IO) { OpenPakNative.downloadSave(save.titleId) }

    suspend fun uploadSave(save: CloudSave): String = withContext(Dispatchers.IO) {
        OpenPakNative.uploadSave(save.titleId, save.newest?.number ?: 0)
    }

    fun isInstalled(titleId: String): Boolean = localGameNames().containsKey(titleId)

    suspend fun hasLocalSave(titleId: String): Boolean =
        withContext(Dispatchers.IO) { OpenPakNative.hasLocalSave(titleId) }

    suspend fun mods(titleId: String): List<Mod> = withContext(Dispatchers.IO) {
        val array = JSONArray(OpenPakNative.mods(titleId))
        (0 until array.length()).map { i ->
            val m = array.getJSONObject(i)
            Mod(
                id = m.optString("id"),
                name = m.optString("name"),
                version = m.optString("version"),
                author = m.optString("author"),
                licence = m.optString("licence"),
                summary = m.optString("summary")
            )
        }
    }

    suspend fun status(): Status = withContext(Dispatchers.IO) {
        val json = JSONObject(OpenPakNative.status())
        val services = json.optJSONArray("services") ?: JSONArray()
        Status(
            ok = json.optBoolean("ok"),
            url = json.optString("url"),
            headline = json.optString("headline"),
            sub = json.optString("sub"),
            state = json.optString("state"),
            services = (0 until services.length()).map { i ->
                val s = services.getJSONObject(i)
                Service(
                    s.optString("name"),
                    s.optBoolean("up"),
                    s.optDouble("uptime"),
                    s.optString("latency")
                )
            },
            playersOk = json.optBoolean("players_ok"),
            players = json.optInt("players"),
            titles = json.optJSONArray("titles").counts(),
            networks = json.optJSONArray("networks").counts(),
            refreshed = System.currentTimeMillis()
        )
    }

    suspend fun testConnection(): Connection = withContext(Dispatchers.IO) {
        val json = JSONObject(OpenPakNative.testConnection())
        Connection(
            pingMs = if (json.has("ping_ms")) json.optInt("ping_ms") else null,
            nat = json.optString("nat").ifEmpty { null }
        )
    }

    suspend fun refreshNetwork(): String = withContext(Dispatchers.IO) { OpenPakNative.refreshNetwork() }

    fun networkSummary(): String = OpenPakNative.networkSummary()

    fun runningTitle(): String = OpenPakNative.runningTitle()

    // ---- the library's Wii games, by title id ----

    data class Game(val titleId: String, val name: String)

    /** The Wii games in the library, for names and the Mods title picker. */
    fun localGames(): List<Game> {
        val files: Array<GameFile> = GameFileCacheManager.getGameFiles().value ?: return emptyList()
        return files.asSequence()
            .filter {
                val platform = Platform.fromInt(it.getPlatform())
                platform == Platform.WII || platform == Platform.WIIWARE
            }
            .mapNotNull { file ->
                val id = file.getTitleId()
                if (id == 0L) null else Game(String.format(Locale.ROOT, "%016x", id), file.getTitle())
            }
            .distinctBy { it.titleId }
            .sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })
            .toList()
    }

    private fun localGameNames(): Map<String, String> = localGames().associate { it.titleId to it.name }

    fun gameName(titleId: String): String? =
        if (titleId.isEmpty()) null else localGameNames()[titleId.lowercase(Locale.ROOT)]

    // ---- formatting (§5.4: absolute, the locale's short date and time) ----

    fun formatTime(epochMillis: Long): String =
        DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(Date(epochMillis))

    fun formatRfc3339(value: String): String {
        if (value.isEmpty()) return ""
        return try {
            formatTime(OffsetDateTime.parse(value).toInstant().toEpochMilli())
        } catch (e: Exception) {
            value
        }
    }

    fun formatSize(context: Context, bytes: Long): String = Formatter.formatShortFileSize(context, bytes)

    fun consoleName(namespace: String): String = when (namespace.lowercase(Locale.ROOT)) {
        "switch" -> "Switch"
        "wiiu" -> "Wii U"
        "3ds" -> "3DS"
        "wii" -> "Wii"
        "ds" -> "DS"
        else -> namespace
    }

    fun host(url: String): String = url.substringAfter("://").substringBefore('/')

    /**
     * A library error as the screen shows it: the server's own sentence when it sent one,
     * "Could not reach {0}." otherwise. HTTP codes and exception text stay in the log.
     */
    private fun failed(context: Context, error: String): Answer.Failed {
        val technical = error.isEmpty() || error.contains("HTTP") || error.startsWith("Could not reach") ||
            error.startsWith("Unexpected") || error.contains("exception", ignoreCase = true)
        return Answer.Failed(
            if (technical) context.getString(R.string.openpak_error_unreachable, host(state().website))
            else context.getString(R.string.openpak_error_server, error)
        )
    }

    private fun JSONArray?.strings(): List<String> =
        if (this == null) emptyList() else (0 until length()).map { optString(it) }

    private fun JSONArray?.counts(): List<Count> =
        if (this == null) emptyList() else (0 until length()).map {
            val c = getJSONObject(it)
            Count(c.optString("id"), c.optInt("players"))
        }.sortedByDescending { it.players }
}
