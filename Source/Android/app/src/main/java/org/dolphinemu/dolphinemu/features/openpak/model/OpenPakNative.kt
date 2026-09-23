// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.openpak.model

/**
 * The bridge to openpak-client (Source/Android/jni/OpenPakNative.cpp). Everything that answers
 * with data answers with a JSON string. Calls marked "network" block: make them from
 * Dispatchers.IO (see [OpenPak]), never the main thread.
 */
object OpenPakNative {
    /** Directories, client name, saves platform; fetches the network profile off-thread. */
    @JvmStatic
    external fun init(version: String, device: String)

    /** Local: {signed_in, name, website, status_url}. */
    @JvmStatic
    external fun state(): String

    /** Network: {ok, name} or {ok:false, code, message}. */
    @JvmStatic
    external fun signIn(email: String, password: String, device: String): String

    /** Network: forgets the token here and revokes it on the server. */
    @JvmStatic
    external fun signOut()

    /** Network. */
    @JvmStatic
    external fun profile(): String

    /** Network. */
    @JvmStatic
    external fun friends(): String

    /** Network, plus local I/O for each title's local copy. */
    @JvmStatic
    external fun cloudSaves(): String

    /** Network: deletes every listed version. Empty on success, else why. */
    @JvmStatic
    external fun deleteSave(versionIds: LongArray): String

    /** Network: take the cloud's copy. Empty on success, else why. */
    @JvmStatic
    external fun downloadSave(titleId: String): String

    /** Network: keep this machine's copy. Empty on success, else why. */
    @JvmStatic
    external fun uploadSave(titleId: String, newestCloud: Int): String

    /** Local. */
    @JvmStatic
    external fun hasLocalSave(titleId: String): Boolean

    /** Network: the public mods catalogue for a title. */
    @JvmStatic
    external fun mods(titleId: String): String

    /** Network: the status page and the player counts. */
    @JvmStatic
    external fun status(): String

    /** Network: {ping_ms?, nat?}. */
    @JvmStatic
    external fun testConnection(): String

    /** Network: "Refresh network settings"; answers the {0} of settings.network_status. */
    @JvmStatic
    external fun refreshNetwork(): String

    /** Local: the stored network profile, as the {0} of settings.network_status. */
    @JvmStatic
    external fun networkSummary(): String

    /** Local: the running Wii title as 16 hex digits, or empty. */
    @JvmStatic
    external fun runningTitle(): String

    /** Skip on the "Checking cloud save..." line. */
    @JvmStatic
    external fun skipCloudPull()
}
