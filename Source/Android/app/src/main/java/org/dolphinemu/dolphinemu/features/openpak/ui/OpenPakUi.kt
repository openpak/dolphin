// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.openpak.ui

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.view.View
import android.widget.Toast
import androidx.fragment.app.FragmentActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.launch
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.openpak.model.OpenPak
import org.dolphinemu.dolphinemu.features.settings.model.BooleanSetting

/** The View-side pieces the settings screens, the home toolbar and the game menu share. */
object OpenPakUi {
    /** §4.3: Snackbars show for the same 6 s as the desktop toasts. */
    const val SNACKBAR_MS = 6000

    /**
     * A §3.10 event as a Snackbar; suppressed while "Show notifications" is off. [always] is for
     * the result of an action the person just took on a screen (the desktop's status line).
     */
    fun snackbar(
        activity: Activity,
        message: String,
        actionText: Int = 0,
        always: Boolean = false,
        action: (() -> Unit)? = null
    ) {
        if (!always && !BooleanSetting.MAIN_OPENPAK_NOTIFICATIONS.boolean) return
        val view = activity.findViewById<View>(android.R.id.content)
        if (view == null) {
            Toast.makeText(activity, message, Toast.LENGTH_LONG).show()
            return
        }
        val bar = Snackbar.make(view, message, SNACKBAR_MS)
        if (actionText != 0 && action != null) bar.setAction(actionText) { action() }
        bar.show()
    }

    /**
     * §3.5 as a Material alert: Cancel is the default and what Back does; Sign out revokes the
     * token (off the main thread) and then says so.
     */
    fun confirmSignOut(activity: FragmentActivity, onDone: () -> Unit) {
        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.openpak_signout_title)
            .setMessage(R.string.openpak_signout_body_single)
            .setPositiveButton(R.string.openpak_signout_confirm) { _, _ ->
                activity.lifecycleScope.launch {
                    OpenPak.signOut()
                    snackbar(activity, activity.getString(R.string.openpak_toast_signed_out))
                    onDone()
                }
            }
            .setNegativeButton(R.string.openpak_common_cancel, null)
            .show()
    }

    /**
     * §3.2 / §4.2 first run, for a non-Switch app: the connect screen, once per install, after
     * Dolphin's own onboarding (the analytics question). Never when a game was asked for.
     */
    @JvmStatic
    fun maybeShowConnect(activity: Activity) {
        if (OpenPak.connectAsked(activity)) return
        OpenPak.setConnectAsked(activity)
        OpenPakActivity.launch(activity, OpenPakActivity.Screen.CONNECT)
    }

    fun openUrl(context: Context, url: String) {
        try {
            context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (e: Exception) {
            Toast.makeText(context, url, Toast.LENGTH_LONG).show()
        }
    }
}
