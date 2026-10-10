package org.citra.citra_emu.vr.ui

import android.app.Presentation
import android.content.Context
import android.graphics.Bitmap
import android.graphics.drawable.ColorDrawable
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.os.SystemClock
import android.util.DisplayMetrics
import android.view.InputDevice
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.MotionEvent.PointerCoords
import android.view.MotionEvent.PointerProperties
import android.view.Surface
import android.view.View
import android.view.Window
import android.view.WindowManager
import org.citra.citra_emu.utils.Log
import org.citra.citra_emu.vr.VrActivity
import java.io.File
import java.io.FileOutputStream
import kotlin.math.hypot
import kotlin.math.roundToInt

/*
* This class populates an "SwapchainAndroidSurfaceKHR" with the
* contents of a secondary virtual display. It allows for smooth animations,
* but the perf doesn't scale well with texture size and it doesn't support mip
*levels. Therefore, it is important to set the the display size of the texture
*(using resource sizes, display density and the native density constant) to be
*something that's close enough to 1:1 texels:pixels so as to not require mips.
**/
abstract class VrUILayer(
    val activity: VrActivity,
    private val layoutId: Int,
    densityDpi: Int = DEFAULT_DENSITY.toInt()
) {
    private val requestedDensity: Float = densityDpi.toFloat()
    private var virtualDisplay: VirtualDisplay? = null
    private var presentation: Presentation? = null

    val window: Window?
        get() = presentation!!.window

    /// Called from JNI ////
    fun getBoundsForView(handle: Long): Int {
        val contentView = LayoutInflater.from(activity).inflate(layoutId, null, false)
        if (contentView == null) {
            Log.warning("contentView is null")
            return -1
        }
        // NOTE: this method will only work when the root layout is sized with wrap_content
        contentView.measure(
            View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED),
            View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED)
        )
        contentView.layout(0, 0, contentView.measuredWidth, contentView.measuredHeight)

        val measuredWidthPx = contentView.width
        val measuredHeightPx = contentView.height

        val displayMetrics = activity.resources.displayMetrics
        val measuredWidthDp = (measuredWidthPx / displayMetrics.density) / (DEFAULT_DENSITY / requestedDensity)
        val measuredHeightDp = measuredHeightPx / displayMetrics.density / (DEFAULT_DENSITY / requestedDensity)

        // Call native method with measured dimensions
        nativeSetBounds(handle, 0, 0, measuredWidthDp.roundToInt(), measuredHeightDp.roundToInt())
        return 0
    }

    fun sendClickToUI(x: Float, y: Float, motionType: Int): Int {
        val action = when (motionType) {
            0 -> MotionEvent.ACTION_UP
            1 -> MotionEvent.ACTION_DOWN
            2 -> MotionEvent.ACTION_MOVE
            3 -> MotionEvent.ACTION_CANCEL
            else -> MotionEvent.ACTION_HOVER_ENTER
        }
        activity.runOnUiThread { dispatchPointerEvent(x, y, action) }
        return 0
    }

    // The gesture in progress: where and when the trigger went down, and whether
    // the pointer has since moved far enough to be a drag rather than a click.
    private var gestureDownTime = 0L
    private var gestureDownX = 0f
    private var gestureDownY = 0f
    private var isDragging = false

    /**
     * Turns the controller's trigger into touch events. A pointer held at arm's
     * length shakes by more than Android's touch slop while the trigger is
     * pulled, which made scrollable views take clicks for drags and cancel them.
     * Until the pointer leaves [CLICK_SLOP_PX] of where the trigger went down,
     * it is reported there, so the release lands as a click; beyond it, the
     * gesture becomes a drag (e.g. to scroll a list) and follows the pointer.
     */
    private fun dispatchPointerEvent(x: Float, y: Float, action: Int) {
        when (action) {
            MotionEvent.ACTION_DOWN -> {
                gestureDownTime = SystemClock.uptimeMillis()
                gestureDownX = x
                gestureDownY = y
                isDragging = false
                dispatchTouchEvent(x, y, action)
            }

            MotionEvent.ACTION_MOVE, MotionEvent.ACTION_UP -> {
                if (!isDragging &&
                    hypot(x - gestureDownX, y - gestureDownY) > CLICK_SLOP_PX
                ) {
                    isDragging = true
                }
                if (isDragging) {
                    dispatchTouchEvent(x, y, action)
                } else {
                    dispatchTouchEvent(gestureDownX, gestureDownY, action)
                }
            }

            else -> dispatchTouchEvent(x, y, action)
        }
    }

    fun sendScrollToUI(x: Float, y: Float, scrollX: Float, scrollY: Float): Int {
        activity.runOnUiThread { dispatchScrollEvent(x, y, scrollX, scrollY) }
        return 0
    }

    fun setSurface(
        surface: Surface, widthDp: Int,
        heightDp: Int
    ): Int {
        activity.runOnUiThread { setSurface_(surface, widthDp, heightDp) }
        return 0
    }

    protected open fun onSurfaceCreated() {}

    /**
     * Called from native code before the surface is destroyed. Detaches the virtual
     * display from the surface right away, so it stops drawing into it, and tears
     * the display down on the UI thread.
     */
    fun release() {
        virtualDisplay?.surface = null
        activity.runOnUiThread {
            presentation?.dismiss()
            presentation = null
            virtualDisplay?.release()
            virtualDisplay = null
        }
    }

    /**
     * The window that should receive injected touch events. Defaults to the
     * presentation's window, but subclasses can redirect input to another
     * window on the same virtual display (e.g. a dialog opened on top of the
     * panel), which would otherwise never see events dispatched to the
     * presentation's decor view.
     */
    protected open fun getTouchTargetWindow(): Window? = presentation?.window

    private fun dispatchTouchEvent(x: Float, y: Float, action: Int) {
        val targetWindow = getTouchTargetWindow() ?: return
        val decorView = targetWindow.decorView

        // Incoming coordinates are in virtual-display space. The presentation
        // window fills the display, but other windows (dialogs) are smaller
        // and centered, so translate into the target window's frame.
        val windowOffset = IntArray(2)
        decorView.getLocationOnScreen(windowOffset)
        val localX = x - windowOffset[0]
        val localY = y - windowOffset[1]

        val eventTime = SystemClock.uptimeMillis()
        // Events of a gesture share the time it started, as Android expects (taps,
        // long presses and flings are measured from it).
        val downTime = if (action == MotionEvent.ACTION_DOWN || gestureDownTime == 0L) {
            eventTime
        } else {
            gestureDownTime
        }

        val event = MotionEvent.obtain(
            downTime,
            eventTime,
            action,
            1, // Only one pointer is used here
            arrayOf(PointerProperties().apply {
                id = 0
                toolType = MotionEvent.TOOL_TYPE_FINGER
            }),
            arrayOf(PointerCoords().apply {
                this.x = localX
                this.y = localY
                pressure = 1f
                size = 1f
            }),
            0, 0, // MetaState and buttonState
            1f, 1f, // Precision X and Y
            0, 0, // Device ID and Edge Flags
            InputDevice.SOURCE_TOUCHSCREEN,
            0 // Flags
        )
        try {
            // Dispatch the MotionEvent to the view
            decorView.dispatchTouchEvent(event)
        } finally {
            // Ensure the MotionEvent is recycled after use
            event.recycle()
        }
    }

    /**
     * Synthesizes a mouse-wheel event at the given display coordinates so
     * scrollable views under the cursor (e.g. the settings list, or the list
     * in an open dialog) can be scrolled with the thumbstick. Scroll amounts
     * are in wheel notches.
     */
    private fun dispatchScrollEvent(x: Float, y: Float, scrollX: Float, scrollY: Float) {
        val targetWindow = getTouchTargetWindow() ?: return
        val decorView = targetWindow.decorView

        val windowOffset = IntArray(2)
        decorView.getLocationOnScreen(windowOffset)

        val eventTime = SystemClock.uptimeMillis()
        val event = MotionEvent.obtain(
            eventTime,
            eventTime,
            MotionEvent.ACTION_SCROLL,
            1,
            arrayOf(PointerProperties().apply {
                id = 0
                toolType = MotionEvent.TOOL_TYPE_MOUSE
            }),
            arrayOf(PointerCoords().apply {
                this.x = x - windowOffset[0]
                this.y = y - windowOffset[1]
                setAxisValue(MotionEvent.AXIS_HSCROLL, scrollX)
                setAxisValue(MotionEvent.AXIS_VSCROLL, scrollY)
            }),
            0, 0, // MetaState and buttonState
            1f, 1f, // Precision X and Y
            0, 0, // Device ID and Edge Flags
            InputDevice.SOURCE_MOUSE,
            0 // Flags
        )
        try {
            decorView.dispatchGenericMotionEvent(event)
        } finally {
            event.recycle()
        }
    }

    private fun setSurface_(
        surface: Surface, widthDp: Int,
        heightDp: Int
    ) {
        // Create a virtual display based on the exact dimensions needed for the view
        val displayManager = activity.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        virtualDisplay = displayManager.createVirtualDisplay(
            VIRTUAL_DISPLAY_NAME, widthDp, heightDp, requestedDensity.toInt(), surface,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION
        )
        presentation = Presentation(activity.applicationContext, virtualDisplay!!.display).apply {
            window?.setType(WindowManager.LayoutParams.TYPE_PRIVATE_PRESENTATION)
            setContentView(layoutId)
            // Sets the background to transparent. Remove to set background to white
            // (useful for catching overrendering)
            window?.setBackgroundDrawable(ColorDrawable(0))
            show()
        }
        onSurfaceCreated()
    }

    /*** Debug/Testing  */
    fun writeBitmapToDisk(bmp: Bitmap, outName: String?) {
        val sdCard = activity.externalCacheDir
        if (sdCard != null && outName != null) {
            val file = File(sdCard.absolutePath, outName)
            try {
                FileOutputStream(file).use { out ->
                    bmp.compress(Bitmap.CompressFormat.PNG, 100, out)
                }
            } catch (e: Exception) {
                Log.error("Failed to write bitmap to disk: ${e.message}")
            }
        }
    }

    companion object {
        // Name of the virtual displays the VR UI panels are drawn on. SecondaryDisplay
        // must never pick these to show a game screen on.
        const val VIRTUAL_DISPLAY_NAME = "CitraVR"

        // DPI android uses as "1:1" with dp coordinates.
        // AKA "baseline density"
        private const val DEFAULT_DENSITY = DisplayMetrics.DENSITY_MEDIUM.toFloat()

        // How far (in panel pixels) the pointer may wander while the trigger is held
        // and still make a click: pulling and releasing the trigger tilts the
        // controller too. Panels are shown at 1200 px per meter, so this is about
        // 8 cm, or 3 degrees of hand movement at a panel 1.6 m away.
        private const val CLICK_SLOP_PX = 100f
        private external fun nativeSetBounds(
            handle: Long, leftInDp: Int, topInDp: Int,
            rightInDp: Int, bottomInDp: Int
        )

        /*** Debug/Testing  */
        const val DEBUG_WRITE_VIEW_TO_DISK = false
    }
}