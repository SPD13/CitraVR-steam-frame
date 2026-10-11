// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.vr.ui

import android.widget.FrameLayout
import org.citra.citra_emu.R
import org.citra.citra_emu.vr.VrActivity

/**
 * The Steam Frame controllers, labelled with what each control does in the
 * current mapping. Shown in VR to the side of the game (see vr_main.cpp), so
 * the mapping can be checked by turning one's head. The labels come from the
 * mapping the VR session uses (per-game values included), sent by native code
 * whenever it changes ([setMapping]); the menu bar's Controls tab shows them too.
 */
class VrControllerDiagramLayer(activity: VrActivity) :
    VrUILayer(activity, R.layout.vr_controller_diagram) {

    override fun onSurfaceCreated() {
        super.onSurfaceCreated()
        val labelsView = window!!.findViewById<FrameLayout>(R.id.controller_diagram_labels)
        ControllerDiagram.observe { map ->
            ControllerDiagram.populate(labelsView, map, ControllerDiagram.DRAWING_WIDTH)
        }
    }

    /**
     * Called from native code (VR thread) with the 3DS button of each control,
     * in the order of VRSettings::VrControl (vr_settings.h).
     */
    fun setMapping(newMapping: IntArray) {
        activity.runOnUiThread { ControllerDiagram.setMapping(newMapping) }
    }
}
