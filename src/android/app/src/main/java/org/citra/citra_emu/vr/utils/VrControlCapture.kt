// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.vr.utils

import android.os.Handler
import android.os.Looper

/**
 * Lets the VR settings wait for a controller button to be pressed (to map it to
 * a 3DS button). While waiting, the VR session reports the next mappable button
 * pressed (see HandleInput() in vr_main.cpp) instead of sending it to the game.
 */
object VrControlCapture {
    private val mainHandler = Handler(Looper.getMainLooper())

    // Main thread only.
    private var listener: ((Int) -> Unit)? = null

    /** Whether a VR session runs in this process, to report button presses. */
    @Volatile
    var isAvailable = false

    /**
     * Calls [onPressed] (on the main thread) with the next controller input
     * pressed, as an index into VrButtonMappingSetting.INPUTS. Main thread only.
     */
    fun start(onPressed: (Int) -> Unit) {
        listener = onPressed
        VrMessageQueue.post(VrMessageQueue.MessageType.CAPTURE_CONTROL, 1)
    }

    /** Stops waiting. Main thread only. */
    fun stop() {
        if (listener == null) {
            return
        }
        listener = null
        VrMessageQueue.post(VrMessageQueue.MessageType.CAPTURE_CONTROL, 0)
    }

    /** Called by the VR session (any thread) when a controller input is pressed. */
    fun onControlPressed(input: Int) {
        mainHandler.post {
            val onPressed = listener ?: return@post
            stop()
            onPressed(input)
        }
    }
}
