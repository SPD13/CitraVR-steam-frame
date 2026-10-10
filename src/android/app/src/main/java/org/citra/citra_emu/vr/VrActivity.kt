package org.citra.citra_emu.vr

import android.app.Activity
import android.app.ActivityManager
import android.app.ActivityOptions
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Display
import android.view.View
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.activities.EmulationActivity
import org.citra.citra_emu.features.settings.ui.SettingsActivity
import org.citra.citra_emu.features.settings.utils.SettingsFile
import org.citra.citra_emu.fragments.EmulationFragment
import org.citra.citra_emu.fragments.EmulationFragment.Companion
import org.citra.citra_emu.model.Game
import org.citra.citra_emu.ui.main.MainActivity
import org.citra.citra_emu.utils.ButtonSwapUtils
import org.citra.citra_emu.utils.EmulationMenuSettings
import org.citra.citra_emu.utils.Log
import org.citra.citra_emu.vr.ui.VrGameListLayer
import org.citra.citra_emu.vr.utils.VRUtils
import kotlin.system.exitProcess


class VrActivity : EmulationActivity() {
    private var mHandle: Long = 0
    private var clickRunnable = ClickRunnable()
    private var isQuitting = false

    override fun onCreate(savedInstanceState: Bundle?) {
        Log.info("VR [Java] onCreate()");
        super.onCreate(savedInstanceState)
        if (hasRun) {
            Log.info("VR [Java] VRActivity already existed")
            finish()
            // When we detect two instances running, due to bad cleanup
            // handling, we have to terminate both of them. Restart the main
            // activity with an error message telling the user what happened.
            val relaunchMainIntent = Intent(this, MainActivity::class.java)
            relaunchMainIntent.putExtra(EXTRA_ERROR_TWO_INSTANCES, true)
            startActivity(relaunchMainIntent)
            currentActivity?.finish()
            return
        }
        hasRun = true
        currentActivity = this
        NativeLibrary.prepareGameSettings(intent.getLongExtra(EXTRA_SELECTED_TITLE_ID, 0L))
        mHandle = nativeOnCreate()
    }

    override fun onDestroy() {
       Log.info("VR [Java] onDestroy");
        currentActivity = null
        if (mHandle != 0L) {
            nativeOnDestroy(mHandle)
            mHandle = 0L
        }
        super.onDestroy()
        exitProcess(0)
    }

    public override fun onStart() {
       Log.info("VR [Java] onStart");
        System.gc()
        super.onStart()
    }

    public override fun onResume() {
       Log.info("VR [Java] onResume");
        super.onResume()
    }

    public override fun onPause() {
       Log.info("VR [Java] onPause");
        // Synchronous save while onPause runs: the OS will not kill the
        // process during this callback, making it the last reliable point to
        // persist the pipeline cache before a potential process death.
        NativeLibrary.saveDiskShaderCaches()
        super.onPause()
    }

    public override fun onStop() {
       Log.info("VR [Java] onStop");
        super.onStop()
    }

    private external fun nativeOnCreate(): Long
    private external fun nativeOnDestroy(handle: Long)
    fun finishActivity() {
        if (!isFinishing) {
            finish()
        }
    }

    fun forwardVRInput(keycode: Int, isPressed: Boolean) {
        val button = org.citra.citra_emu.vr.utils.VRUtils.ButtonType.androidToNativeLibrary(keycode)
            ?: return
        NativeLibrary.onGamePadEvent(
            "Quest controller",
            ButtonSwapUtils.applyBYSwap(button),
            if (isPressed) NativeLibrary.ButtonState.PRESSED else NativeLibrary.ButtonState.RELEASED
        )
    }

    fun forwardVRJoystick(x: Float, y: Float, joystickType: Int) {
        // dispatch joystick input as gamepad joystick input
        NativeLibrary.onGamePadMoveEvent(
            "Quest controller",
            if (joystickType == 0) NativeLibrary.ButtonType.STICK_C else NativeLibrary.ButtonType.STICK_LEFT,
            x, -y
        )
    }

    fun openSettingsMenu() {
        SettingsActivity.launch(this, SettingsFile.FILE_NAME_CONFIG, "")
    }

    fun sendClickToWindow(
        x: Float, y: Float,
        motionType: Int
    ) {
        clickRunnable.updateState(x, y, motionType)
        runOnUiThread(clickRunnable)
    }

    /** "Close Game" in the VR menu bar. */
    fun quitToMenu() {
        // On Steam Frame, ending the VR session drops the user into the Steam
        // interface, with CitraVR's 2D window left in the background. Offer the
        // next game in VR instead.
        if (VRUtils.hMDType == VRUtils.HMDType.STEAMFRAME.value) {
            // Keep the pipeline cache fresh in case the user picks another game,
            // which ends this process without stopping the core.
            NativeLibrary.saveDiskShaderCaches()
            if (VrGameListLayer.sVrGameListLayer.get()?.show() == true) {
                return
            }
        }
        exitToCitraWindow()
    }

    /** Ends the VR session and returns to CitraVR's 2D window. */
    fun exitToCitraWindow() {
        // The exit button can deliver multiple events per click; only the
        // first call may relaunch MainActivity or duplicate panels appear.
        if (isQuitting) return
        isQuitting = true
        // Save the pipeline cache before starting teardown: onDestroy calls
        // exitProcess(0) without ever stopping the core, so the renderer
        // destructor (the normal save point) never runs on this path.
        NativeLibrary.saveDiskShaderCaches()
        finish()
        val relaunchMainIntent = Intent(this, MainActivity::class.java)
        startActivity(relaunchMainIntent)
    }

    /**
     * Starts [game] from the VR game list. A game can't be started in a VR process
     * that already ran one, so this process ends and MainActivity starts the game
     * in a new one once it's gone (see [launchGameFromIntentIfRequested]).
     */
    fun switchToGame(game: Game) {
        if (isQuitting) return
        isQuitting = true
        Log.info("VR [Java] switching to ${game.title}")
        NativeLibrary.saveDiskShaderCaches()
        val relaunchMainIntent = Intent(this, MainActivity::class.java)
            .putExtra(EXTRA_LAUNCH_GAME_PATH, game.path)
            .putExtra(EXTRA_LAUNCH_GAME_TITLE, game.title)
            .putExtra(EXTRA_LAUNCH_GAME_TITLE_ID, game.titleId)
        startActivity(relaunchMainIntent)
        finish()
    }

    fun pauseGame() {
       Log.info("VR [Java] pauseGame");
        NativeLibrary.pauseEmulation();
        // Emulation pauses whenever the user opens a menu or loses focus;
        // piggyback on that to keep the on-disk pipeline cache fresh in case
        // the process is later killed without warning.
        NativeLibrary.saveDiskShaderCaches()
    }

    fun resumeGame() {
       Log.info("VR [Java] resumeGame");
        // Note: isRunning() checks to make sure the emulation has started and pausing it is
        // safe -- not whether it's paused/resumed
         NativeLibrary.unPauseEmulation();
    }

    class ClickRunnable : Runnable {
        private var xPosition: Float = 0.0F
        private var yPosition: Float = 0.0F
        private var motionType: Int = 0

        fun updateState(x: Float, y: Float, motionType: Int) {
            this.xPosition = x
            this.yPosition = y
            this.motionType = motionType
        }
        override fun run() {
            when (motionType) {
                0 -> NativeLibrary.onTouchEvent(0.0F, 0.0F, false)
                1 -> NativeLibrary.onTouchEvent(xPosition, yPosition, true)
                2 -> NativeLibrary.onTouchMoved(xPosition, yPosition)
                else -> Log.error("VR [Java] sendClickToWindow: unknown motionType: $motionType")
            }
        }
    }

    companion object {
        const val EXTRA_ERROR_TWO_INSTANCES = "org.citra.citra_emu.vr.ERROR_TWO_INSTANCES"
        private const val EXTRA_LAUNCH_GAME_PATH = "org.citra.citra_emu.vr.LAUNCH_GAME_PATH"
        private const val EXTRA_LAUNCH_GAME_TITLE = "org.citra.citra_emu.vr.LAUNCH_GAME_TITLE"
        private const val EXTRA_LAUNCH_GAME_TITLE_ID = "org.citra.citra_emu.vr.LAUNCH_GAME_TITLE_ID"
        private const val VR_PROCESS_EXIT_TIMEOUT_MS = 5000L
        private const val VR_PROCESS_POLL_INTERVAL_MS = 100L
        private const val EXTRA_SELECTED_TITLE_ID = "SelectedTitleId"
        var hasRun = false
        var currentActivity: VrActivity? = null

        init {
            if (Build.BRAND == "oculus") {
                try {
                    System.loadLibrary("openxr_forwardloader.oculus")
                } catch (e: UnsatisfiedLinkError) {
                    // This was needed before v62
                    // In v62 this library is deleted
                }
            }
        }

        fun launch(
            context: Context, gamePath: String?,
            gameTitle: String?, gameTitleId: Long
        ) {
            val intent = Intent(context, VrActivity::class.java)
            val mainDisplayId = getMainDisplay(context)
            if (mainDisplayId < 0) {
                throw RuntimeException("Could not find main display")
            }
            val options = ActivityOptions.makeBasic().setLaunchDisplayId(mainDisplayId)
            intent.setFlags(
                Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK or
                        Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP
            )
            intent.putExtra("SelectedGame", gamePath)
            intent.putExtra("SelectedTitle", gameTitle)
            intent.putExtra(EXTRA_SELECTED_TITLE_ID, gameTitleId)
            (context as Activity).finish()
            if (context is ContextWrapper) {
                val baseContext = context.baseContext
                baseContext.startActivity(intent, options.toBundle())
            } else {
                context.startActivity(intent, options.toBundle())
            }
        }

        /**
         * Called by MainActivity with its intent: if the VR game list asked for a
         * game ([switchToGame]), starts it once the previous VR process has exited.
         */
        fun launchGameFromIntentIfRequested(activity: Activity, intent: Intent?) {
            val path = intent?.getStringExtra(EXTRA_LAUNCH_GAME_PATH) ?: return
            val title = intent.getStringExtra(EXTRA_LAUNCH_GAME_TITLE)
            val titleId = intent.getLongExtra(EXTRA_LAUNCH_GAME_TITLE_ID, 0L)
            intent.removeExtra(EXTRA_LAUNCH_GAME_PATH)

            val handler = Handler(Looper.getMainLooper())
            val deadline = SystemClock.elapsedRealtime() + VR_PROCESS_EXIT_TIMEOUT_MS
            handler.post(object : Runnable {
                override fun run() {
                    if (activity.isFinishing || activity.isDestroyed) {
                        return
                    }
                    if (isVrProcessRunning(activity) && SystemClock.elapsedRealtime() < deadline) {
                        handler.postDelayed(this, VR_PROCESS_POLL_INTERVAL_MS)
                        return
                    }
                    Log.info("VR [Java] launching $title from the VR game list")
                    launch(activity, path, title, titleId)
                }
            })
        }

        private fun isVrProcessRunning(context: Context): Boolean {
            val activityManager = context.getSystemService(ActivityManager::class.java)
            val vrProcessName = "${context.packageName}:vr_process"
            return activityManager.runningAppProcesses?.any { it.processName == vrProcessName } ==
                true
        }

        private fun getMainDisplay(context: Context): Int {
            val displayManager = context.getSystemService(DISPLAY_SERVICE) as DisplayManager
            val displays = displayManager.displays
            for (i in displays.indices) {
                if (displays[i].displayId == Display.DEFAULT_DISPLAY) {
                    return displays[i].displayId
                }
            }
            return -1
        }
    }
}
