// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.openpak.ui

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import org.dolphinemu.dolphinemu.ui.main.ThemeProvider
import org.dolphinemu.dolphinemu.ui.theme.DolphinTheme
import org.dolphinemu.dolphinemu.utils.ThemeHelper

/**
 * OpenPak's native screens (openpak-ux-spec §4.2): the home screen, the seven sections, sign-in
 * and the first-run connect screen. Opened from the Settings list, the home toolbar and the
 * in-game menu; over a running game it opens with the game paused, as Settings does.
 */
class OpenPakActivity : AppCompatActivity(), ThemeProvider {
    enum class Screen { HOME, ACCOUNT, FRIENDS, INVITATIONS, SAVES, MODS, NEWS, STATUS, SIGN_IN, CONNECT }

    override var themeId: Int = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)

        val start = intent.getStringExtra(EXTRA_SCREEN)
            ?.let { name -> Screen.entries.firstOrNull { it.name == name } } ?: Screen.HOME

        setContent {
            DolphinTheme {
                OpenPakApp(activity = this, start = start, onFinish = { finish() })
            }
        }
    }

    override fun setTheme(themeId: Int) {
        super.setTheme(themeId)
        this.themeId = themeId
    }

    override fun onResume() {
        ThemeHelper.setCorrectTheme(this)
        super.onResume()
    }

    companion object {
        private const val EXTRA_SCREEN = "screen"

        @JvmStatic
        fun launch(context: Context, screen: Screen) {
            val intent = Intent(context, OpenPakActivity::class.java).putExtra(EXTRA_SCREEN, screen.name)
            if (context !is Activity) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(intent)
        }
    }
}
