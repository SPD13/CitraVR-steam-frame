// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.vr.ui

import android.graphics.Color
import android.graphics.Typeface
import android.util.TypedValue
import android.view.Gravity
import android.widget.FrameLayout
import android.widget.TextView
import org.citra.citra_emu.R
import org.citra.citra_emu.vr.VrActivity

/**
 * The Steam Frame controllers, labelled with what each control does in the
 * current mapping. Shown in VR to the side of the game (see vr_main.cpp), so
 * the mapping can be checked by turning one's head. The labels come from the
 * mapping the VR session uses (per-game values included), sent by native code
 * whenever it changes ([setMapping]).
 */
class VrControllerDiagramLayer(activity: VrActivity) :
    VrUILayer(activity, R.layout.vr_controller_diagram) {
    private var labelsView: FrameLayout? = null
    private var mapping: IntArray? = null

    override fun onSurfaceCreated() {
        super.onSurfaceCreated()
        labelsView = window!!.findViewById(R.id.controller_diagram_labels)
        mapping?.let(::showLabels)
    }

    /**
     * Called from native code (VR thread) with the 3DS button of each control,
     * in the order of VRSettings::VrControl (vr_settings.h).
     */
    fun setMapping(newMapping: IntArray) {
        activity.runOnUiThread {
            mapping = newMapping
            showLabels(newMapping)
        }
    }

    private enum class Align { START, CENTER, END }

    /**
     * A label, in a box of the source drawing (2400 x 1224 pixels; the same
     * boxes as steam-frame/make_controls_image.py), aligned like the original.
     */
    private class Label(
        val left: Int, val top: Int, val right: Int, val bottom: Int,
        val align: Align, val alignBottom: Boolean, val lines: List<String>,
        val color: Int = Color.WHITE, val textSize: Float = TEXT_SIZE,
        val width: Float = LABEL_WIDTH
    )

    private fun showLabels(map: IntArray) {
        val container = labelsView ?: return
        container.removeAllViews()
        labels(map).forEach { container.addView(textViewFor(it)) }
        val title = TextView(container.context).apply {
            text = container.context.getString(R.string.vr_controller_diagram_title)
            setTextColor(Color.WHITE)
            setTypeface(typeface, Typeface.BOLD)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, TITLE_SIZE)
            gravity = Gravity.CENTER_HORIZONTAL
        }
        container.addView(title, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = 24 })
    }

    private fun textViewFor(label: Label): TextView {
        val left = label.left * SCALE
        val right = label.right * SCALE
        val bottom = label.bottom * SCALE
        val centerY = (label.top + label.bottom) / 2 * SCALE
        val width = label.width
        val height = LABEL_HEIGHT
        val x = when (label.align) {
            Align.START -> left
            Align.CENTER -> (left + right) / 2 - width / 2
            Align.END -> right - width
        }
        val y = if (label.alignBottom) bottom + 4 - height else centerY - height / 2
        val horizontal = when (label.align) {
            Align.START -> Gravity.START
            Align.CENTER -> Gravity.CENTER_HORIZONTAL
            Align.END -> Gravity.END
        }
        val vertical = if (label.alignBottom) Gravity.BOTTOM else Gravity.CENTER_VERTICAL
        return TextView(labelsView!!.context).apply {
            text = label.lines.joinToString("\n")
            setTextColor(label.color)
            setTypeface(typeface, Typeface.BOLD)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, label.textSize)
            setLineSpacing(0f, 0.95f)
            gravity = horizontal or vertical
            layoutParams = FrameLayout.LayoutParams(width.toInt(), height.toInt()).apply {
                leftMargin = x.toInt()
                topMargin = y.toInt()
            }
        }
    }

    private fun labels(map: IntArray): List<Label> {
        fun input(control: Int) = map.getOrElse(control) { 0 }
        val lines = mutableListOf<Label>()
        // Left controller.
        lines += Label(410, 202, 562, 222, Align.CENTER, true,
            listOf("Circle Pad", "click: ${name(input(LEFT_STICK_CLICK))}"))
        lines += Label(399, 315, 461, 340, Align.CENTER, true, actionLines(input(LEFT_VIEW)))
        lines += Label(107, 474, 182, 500, Align.END, false, dpadLines(map))
        lines += Label(95, 568, 179, 616, Align.END, false, listOf("Steam", "(SteamVR)"))
        // Grips (both controllers).
        lines += Label(576, 671, 752, 727, Align.CENTER, false,
            gripLines(input(LEFT_GRIP), input(RIGHT_GRIP)))
        // Right controller.
        lines += Label(758, 206, 910, 226, Align.CENTER, true,
            listOf("C-Stick", "click: ${name(input(RIGHT_STICK_CLICK))}"))
        lines += Label(862, 317, 930, 340, Align.START, true, actionLines(input(RIGHT_MENU)))
        lines += Label(1141, 481, 1212, 499, Align.START, false, listOf(
            "A → ${name(input(A))}   B → ${name(input(B))}",
            "X → ${name(input(X))}   Y → ${name(input(Y))}"
        ))
        lines += Label(1141, 573, 1225, 621, Align.START, false, listOf("Steam", "(SteamVR)"))
        // Triggers and bumpers: the right controller is drawn first, seen from the front.
        lines += Label(1595, 222, 1694, 246, Align.CENTER, true,
            listOf("Bumper: ${name(input(RIGHT_BUMPER))}"))
        lines += Label(1471, 292, 1610, 348, Align.CENTER, false,
            listOf("Trigger:", triggerText(input(RIGHT_TRIGGER))))
        lines += Label(1975, 222, 2074, 246, Align.CENTER, true,
            listOf("Bumper: ${name(input(LEFT_BUMPER))}"))
        lines += Label(2055, 292, 2194, 348, Align.CENTER, false,
            listOf("Trigger:", triggerText(input(LEFT_TRIGGER))))
        lines += Label(1440, 1080, 2230, 1150, Align.CENTER, false, listOf(
            "Triggers click when pointing at a panel",
            "(bottom screen, menus), and press their button elsewhere"
        ), color = NOTE_COLOR, textSize = NOTE_SIZE, width = NOTE_WIDTH)
        return lines
    }

    private fun name(button: Int): String = when (button) {
        96 -> "A"
        97 -> "B"
        99 -> "X"
        100 -> "Y"
        102 -> "L"
        103 -> "R"
        104 -> "ZL"
        105 -> "ZR"
        108 -> "Start"
        109 -> "Select"
        110 -> "Home"
        19 -> "D-pad ↑"
        20 -> "D-pad ↓"
        21 -> "D-pad ←"
        22 -> "D-pad →"
        MENU_BAR -> "Menu bar"
        else -> "—"
    }

    private fun actionLines(button: Int): List<String> =
        if (button == MENU_BAR) listOf("Show/hide", "menu bar") else listOf(name(button))

    private fun triggerText(button: Int): String =
        if (button == 0) "click" else "${name(button)} or click"

    private fun gripLines(left: Int, right: Int): List<String> =
        if (left == 0 && right == 0) {
            listOf("Grips:", "not mapped")
        } else {
            listOf("Grips: ${name(left)} (left)", "${name(right)} (right)")
        }

    private fun dpadLines(map: IntArray): List<String> {
        val up = map.getOrElse(DPAD_UP) { 0 }
        val down = map.getOrElse(DPAD_DOWN) { 0 }
        val left = map.getOrElse(DPAD_LEFT) { 0 }
        val right = map.getOrElse(DPAD_RIGHT) { 0 }
        if (up == 19 && down == 20 && left == 21 && right == 22) {
            return listOf("D-pad")
        }
        return listOf("↑ ${name(up)}   ↓ ${name(down)}", "← ${name(left)}   → ${name(right)}")
    }

    companion object {
        // Controls, in the order of VRSettings::VrControl (vr_settings.h).
        private const val A = 0
        private const val B = 1
        private const val X = 2
        private const val Y = 3
        private const val DPAD_UP = 4
        private const val DPAD_DOWN = 5
        private const val DPAD_LEFT = 6
        private const val DPAD_RIGHT = 7
        private const val LEFT_BUMPER = 8
        private const val RIGHT_BUMPER = 9
        private const val LEFT_GRIP = 10
        private const val RIGHT_GRIP = 11
        private const val LEFT_STICK_CLICK = 12
        private const val RIGHT_STICK_CLICK = 13
        private const val LEFT_TRIGGER = 14
        private const val RIGHT_TRIGGER = 15
        private const val LEFT_VIEW = 16
        private const val RIGHT_MENU = 17

        /** Not a 3DS button: shows or hides the menu bar (vr_settings.h). */
        private const val MENU_BAR = 1000

        // The drawing is 1600 pixels wide here, from a 2400-pixel source.
        private const val SCALE = 1600f / 2400f
        private const val TEXT_SIZE = 25f
        private const val NOTE_SIZE = 22f
        private const val TITLE_SIZE = 32f
        private const val LABEL_WIDTH = 330f
        private const val LABEL_HEIGHT = 96f
        private const val NOTE_WIDTH = 560f
        private val NOTE_COLOR = Color.rgb(140, 200, 255)
    }
}
