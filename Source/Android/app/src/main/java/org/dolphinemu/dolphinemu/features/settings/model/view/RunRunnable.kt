// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model.view

import android.content.Context
import org.dolphinemu.dolphinemu.NativeLibrary
import org.dolphinemu.dolphinemu.features.settings.model.AbstractSetting

class RunRunnable : SettingsItem {
    val alertText: Int
    val toastTextAfterRun: Int
    private val worksDuringEmulation: Boolean
    val runnable: Runnable

    /** A drawable shown before the name, or 0 for none. */
    var iconId: Int = 0
        private set

    constructor(
        context: Context,
        titleId: Int,
        descriptionId: Int,
        alertText: Int,
        toastTextAfterRun: Int,
        worksDuringEmulation: Boolean,
        runnable: Runnable
    ) : super(context, titleId, descriptionId) {
        this.alertText = alertText
        this.toastTextAfterRun = toastTextAfterRun
        this.worksDuringEmulation = worksDuringEmulation
        this.runnable = runnable
    }

    /** A row whose name and description are only known at runtime (e.g. who is signed in). */
    constructor(
        name: CharSequence,
        description: CharSequence,
        worksDuringEmulation: Boolean,
        iconId: Int = 0,
        runnable: Runnable
    ) : super(name, description) {
        this.alertText = 0
        this.toastTextAfterRun = 0
        this.worksDuringEmulation = worksDuringEmulation
        this.iconId = iconId
        this.runnable = runnable
    }

    override val type: Int = TYPE_RUN_RUNNABLE

    override val setting: AbstractSetting? = null

    override val isEditable: Boolean
        get() = worksDuringEmulation || NativeLibrary.IsUninitialized()
}
