// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.features.romserver

import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import androidx.preference.PreferenceManager
import org.citra.citra_emu.R
import org.citra.citra_emu.ui.main.MainActivity

/**
 * Keeps the ROM upload server running while CitraVR is in the background
 * (for example while a game is being played), with an ongoing notification
 * showing its address.
 */
class RomServerService : Service() {
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var activeTransfers = 0

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "CitraVR:RomServer")
        @Suppress("DEPRECATION")
        wifiLock = applicationContext.getSystemService(WifiManager::class.java)
            ?.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "CitraVR:RomServer")
        // Keep the device and Wi-Fi awake only while a file is being uploaded.
        RomServer.onTransferActive = { active -> onTransferActive(active) }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        RomServer.start(this)
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
        if (RomServer.state.value is RomServer.State.Failed) {
            stopSelf()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        RomServer.onTransferActive = null
        RomServer.stop()
        releaseLocks()
        super.onDestroy()
    }

    @Synchronized
    private fun onTransferActive(active: Boolean) {
        activeTransfers += if (active) 1 else -1
        if (activeTransfers > 0) {
            if (wakeLock?.isHeld == false) wakeLock?.acquire()
            if (wifiLock?.isHeld == false) wifiLock?.acquire()
        } else {
            activeTransfers = 0
            releaseLocks()
        }
    }

    private fun releaseLocks() {
        if (wakeLock?.isHeld == true) wakeLock?.release()
        if (wifiLock?.isHeld == true) wifiLock?.release()
    }

    private fun buildNotification(): Notification {
        val state = RomServer.state.value
        val text = if (state is RomServer.State.Running && state.urls.isNotEmpty()) {
            state.urls.first()
        } else {
            getString(R.string.rom_server_no_network)
        }
        val openApp = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, getString(R.string.app_notification_channel_id))
            .setSmallIcon(R.drawable.ic_stat_notification_logo)
            .setContentTitle(getString(R.string.rom_server_notification_title))
            .setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(text))
            .setContentIntent(openApp)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    companion object {
        private const val NOTIFICATION_ID = 0x524F4D // "ROM"
        const val PREF_ENABLED = "rom_server_enabled"

        fun isEnabled(context: Context): Boolean =
            PreferenceManager.getDefaultSharedPreferences(context).getBoolean(PREF_ENABLED, false)

        /** Persists the choice and starts or stops the server accordingly. */
        fun setEnabled(context: Context, enabled: Boolean) {
            PreferenceManager.getDefaultSharedPreferences(context).edit()
                .putBoolean(PREF_ENABLED, enabled)
                .apply()
            applyEnabledState(context)
        }

        /** Starts the server if it is enabled. Called at app start. */
        fun applyEnabledState(context: Context) {
            val intent = Intent(context, RomServerService::class.java)
            if (isEnabled(context)) {
                ContextCompat.startForegroundService(context, intent)
            } else {
                context.stopService(intent)
            }
        }
    }
}
