// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.vr.ui

import org.citra.citra_emu.R
import org.citra.citra_emu.vr.VrActivity

/**
 * The 3DS-style button layout (X at the top, A on the right, B at the bottom, Y
 * on the left), shown in VR opposite the controller diagram, to compare with
 * the controller mapping. Shown with the controller diagram (see vr_main.cpp).
 */
class VrButtonLayoutLayer(activity: VrActivity) : VrUILayer(activity, R.layout.vr_3ds_layout)
