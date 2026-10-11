// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.vr.utils

import android.os.SystemClock
import java.io.File

/**
 * CPU and GPU load measured by the app itself, for runtimes whose performance
 * metrics don't report them (SteamVR on Steam Frame). Each call measures the
 * load since the previous one, so the first returns NaN.
 *
 * - CPU: the whole device's, from /proc/stat (Lepton's container shares the
 *   headset's kernel, so it covers SteamVR too).
 * - GPU: CitraVR's own, from the time its GPU contexts were busy, as the DRM
 *   driver reports it in /proc/self/fdinfo (drm-engine-gpu). Other processes'
 *   GPU use (the SteamVR compositor) is outside the container and not counted.
 */
class SystemLoadMonitor {
    private var lastCpuBusy = -1L
    private var lastCpuTotal = -1L
    private var lastGpuBusyNs = -1L
    private var lastGpuTimeNs = -1L

    /** Percentage of time the device's CPU cores were busy, or NaN. */
    fun cpuPercent(): Float {
        val fields = try {
            File("/proc/stat").bufferedReader().use { it.readLine() }
                .trim().split(Regex("\\s+")).drop(1).map { it.toLong() }
        } catch (e: Exception) {
            return Float.NaN
        }
        if (fields.size < 4) return Float.NaN
        val total = fields.sum()
        // Idle and I/O wait (the 4th and 5th fields) count as not busy.
        val busy = total - fields[3] - fields.getOrElse(4) { 0L }
        val percent = if (lastCpuTotal >= 0 && total > lastCpuTotal) {
            100f * (busy - lastCpuBusy) / (total - lastCpuTotal)
        } else {
            Float.NaN
        }
        lastCpuBusy = busy
        lastCpuTotal = total
        return percent.coerceIn(0f, 100f)
    }

    /** Percentage of time CitraVR kept the GPU busy, or NaN. */
    fun gpuPercent(): Float {
        val busyNs = gpuBusyNs() ?: return Float.NaN
        val now = SystemClock.elapsedRealtimeNanos()
        val percent = if (lastGpuTimeNs >= 0 && now > lastGpuTimeNs && busyNs >= lastGpuBusyNs) {
            100f * (busyNs - lastGpuBusyNs) / (now - lastGpuTimeNs)
        } else {
            Float.NaN
        }
        lastGpuBusyNs = busyNs
        lastGpuTimeNs = now
        return percent.coerceIn(0f, 100f)
    }

    // Total GPU busy time of this process's DRM clients (an fd can be shared by
    // several descriptors, so each client id is counted once), or null.
    private fun gpuBusyNs(): Long? {
        val fds = File("/proc/self/fd").listFiles() ?: return null
        val busyByClient = mutableMapOf<String, Long>()
        for (fd in fds) {
            val target = try {
                fd.canonicalPath
            } catch (e: Exception) {
                continue
            }
            if (!target.startsWith("/dev/dri/")) continue
            val info = try {
                File("/proc/self/fdinfo/${fd.name}").readLines()
            } catch (e: Exception) {
                continue
            }
            val client = info.firstOrNull { it.startsWith("drm-client-id:") } ?: continue
            val busy = info.firstOrNull { it.startsWith("drm-engine-gpu:") }
                ?.substringAfter(':')?.trim()?.removeSuffix("ns")?.trim()?.toLongOrNull()
                ?: continue
            busyByClient[client] = busy
        }
        return if (busyByClient.isEmpty()) null else busyByClient.values.sum()
    }
}
