// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.features.settings.model.view

import androidx.annotation.StringRes
import org.citra.citra_emu.R
import org.citra.citra_emu.features.settings.model.IntSetting

/**
 * A 3DS button in the VR controller mapping, and the controller input that
 * presses it. The mapping is one to one: a button has at most one input, and an
 * input presses at most one button. Settings are stored per input
 * (vr_map_<input> = the 3DS button, as an Android key code; 0: none).
 *
 * @param button the 3DS button, as the Android key code sent to the emulated gamepad,
 *        or [MENU_BAR]
 */
class VrButtonMappingSetting(
    val button: Int,
    @StringRes titleId: Int
) : SingleChoiceSetting(
    null,
    titleId,
    0,
    R.array.vrInputNames,
    R.array.vrInputValues,
    null,
    NO_INPUT
) {
    /** The input (index into [INPUTS]) mapped to this button, or [NO_INPUT]. */
    override val selectedValue: Int
        get() = INPUTS.indices.firstOrNull { INPUTS[it].int == button } ?: NO_INPUT

    // The list redraws rows whose value changed since they were built; this row's
    // value is its input (which changes when per-game values load, or when its
    // input is given to another button).
    override val valueAtCreation: String? = selectedValue.toString()

    // Stored through the inputs' settings, which can all change in game.
    override val isEditable: Boolean
        get() = !isReadOnly

    /**
     * Makes `input` (or [NO_INPUT]) the only input of this button: the button's
     * other inputs are unmapped, and the input leaves the button it pressed
     * before. Returns the settings that changed, to save.
     */
    fun mapInput(input: Int): List<IntSetting> {
        val changed = mutableListOf<IntSetting>()
        INPUTS.forEachIndexed { index, setting ->
            val newValue = when {
                index == input -> button
                setting.int == button -> 0
                else -> setting.int
            }
            if (setting.int != newValue) {
                setting.int = newValue
                changed += setting
            }
        }
        return changed
    }

    companion object {
        const val NO_INPUT = -1

        /** Not a 3DS button: shows or hides the VR menu bar (see vr_settings.h). */
        const val MENU_BAR = 1000

        /** The mappable inputs, in the order of vrInputNames/vrInputValues. */
        val INPUTS = listOf(
            IntSetting.VR_MAP_A,
            IntSetting.VR_MAP_B,
            IntSetting.VR_MAP_X,
            IntSetting.VR_MAP_Y,
            IntSetting.VR_MAP_DPAD_UP,
            IntSetting.VR_MAP_DPAD_DOWN,
            IntSetting.VR_MAP_DPAD_LEFT,
            IntSetting.VR_MAP_DPAD_RIGHT,
            IntSetting.VR_MAP_LEFT_BUMPER,
            IntSetting.VR_MAP_RIGHT_BUMPER,
            IntSetting.VR_MAP_LEFT_GRIP,
            IntSetting.VR_MAP_RIGHT_GRIP,
            IntSetting.VR_MAP_LEFT_STICK_CLICK,
            IntSetting.VR_MAP_RIGHT_STICK_CLICK,
            IntSetting.VR_MAP_LEFT_TRIGGER,
            IntSetting.VR_MAP_RIGHT_TRIGGER,
            IntSetting.VR_MAP_LEFT_VIEW,
            IntSetting.VR_MAP_RIGHT_MENU
        )
    }
}
