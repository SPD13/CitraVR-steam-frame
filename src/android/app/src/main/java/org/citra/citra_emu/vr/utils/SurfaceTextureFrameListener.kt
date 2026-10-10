// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.vr.utils

import android.graphics.SurfaceTexture
import android.os.Handler
import android.os.Looper
import androidx.annotation.Keep
import java.util.concurrent.atomic.AtomicLong

/**
 * Counts the frames queued into a SurfaceTexture, so the native VR code
 * (SurfaceSwapchain) knows when there is something new to copy into its
 * swapchain. Buffer timestamps can't be used for this: not every producer sets
 * them.
 */
@Keep
class SurfaceTextureFrameListener(surfaceTexture: SurfaceTexture) :
    SurfaceTexture.OnFrameAvailableListener {
    private val frameCount = AtomicLong(0)

    init {
        surfaceTexture.setOnFrameAvailableListener(this, Handler(Looper.getMainLooper()))
    }

    override fun onFrameAvailable(surfaceTexture: SurfaceTexture) {
        frameCount.incrementAndGet()
    }

    fun getFrameCount(): Long = frameCount.get()
}
