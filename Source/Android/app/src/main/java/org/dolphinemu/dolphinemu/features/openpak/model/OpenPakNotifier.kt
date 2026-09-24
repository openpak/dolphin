// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.openpak.model

import android.os.Handler
import android.os.Looper
import androidx.annotation.Keep
import com.google.android.material.snackbar.Snackbar
import org.dolphinemu.dolphinemu.DolphinApplication
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.openpak.ui.OpenPakActivity
import org.dolphinemu.dolphinemu.features.openpak.ui.OpenPakUi

/**
 * What the native cloud-save hooks (and the redirects re-check) tell the app (openpak-ux-spec §3.10, §4.3): the words come
 * from the string table here, and the app shows them as a Snackbar (or, during emulation, the
 * native side shows them as the overlay message).
 */
@Keep
object OpenPakNotifier {
    const val PULLED = 0
    const val CONFLICT = 1
    const val PUSHED = 2
    const val PUSH_FAILED = 3
    const val REDIRECTS_CHANGED = 4

    private val main = Handler(Looper.getMainLooper())
    private var checking: Snackbar? = null

    @Keep
    @JvmStatic
    fun text(kind: Int, name: String, detail: String): String {
        val context = DolphinApplication.getAppContext()
        return when (kind) {
            PULLED -> context.getString(R.string.openpak_toast_saves_pulled, name)
            CONFLICT -> context.getString(R.string.openpak_toast_saves_conflict, name)
            PUSHED -> context.getString(R.string.openpak_toast_saves_pushed, name)
            PUSH_FAILED -> context.getString(R.string.openpak_toast_saves_push_failed, name, detail)
            REDIRECTS_CHANGED -> context.getString(R.string.openpak_toast_redirects_changed_emulator)
            else -> ""
        }
    }

    /** From any thread. Clicking the Snackbar's action opens Cloud saves. */
    @Keep
    @JvmStatic
    fun notify(kind: Int, name: String, detail: String) {
        val message = text(kind, name, detail)
        main.post {
            val activity = DolphinApplication.getAppActivity() ?: return@post
            // The redirects notice has nowhere to open: a restart is what it asks for.
            if (kind == REDIRECTS_CHANGED) {
                OpenPakUi.snackbar(activity, message)
                return@post
            }
            OpenPakUi.snackbar(activity, message, R.string.openpak_page_saves) {
                OpenPakActivity.launch(activity, OpenPakActivity.Screen.SAVES)
            }
        }
    }

    /** "Checking cloud save..." with Skip, while the pull before a launch runs (§5.1). */
    @Keep
    @JvmStatic
    fun checking(show: Boolean) {
        main.post {
            checking?.dismiss()
            checking = null
            if (!show) return@post
            val activity = DolphinApplication.getAppActivity() ?: return@post
            val view = activity.findViewById<android.view.View>(android.R.id.content) ?: return@post
            checking = Snackbar.make(view, R.string.openpak_saves_checking, Snackbar.LENGTH_INDEFINITE)
                .setAction(R.string.openpak_saves_skip) { OpenPakNative.skipCloudPull() }
                .also { it.show() }
        }
    }
}
