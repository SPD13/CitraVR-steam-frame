// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.utils

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import androidx.core.content.ContextCompat
import androidx.preference.PreferenceManager
import java.io.File
import org.citra.citra_emu.NativeLibrary

/**
 * CitraVR's own folders, used as plain file-system paths (file:// URIs) when the app
 * has all-files access, instead of folders granted through the system file picker.
 *
 * Plain paths need no per-folder grant, so they can't be lost: on Steam Frame,
 * Lepton drops picker grants whenever the app is updated, which left CitraVR
 * without its user and games folders. They also allow a setup without any picker:
 * [useDefaultDirectories] creates the default folders.
 */
object CitraVrDirectories {
    val defaultUserDirectory: File
        get() = File(Environment.getExternalStorageDirectory(), "CitraVR")
    val defaultGamesDirectory: File
        get() = File(defaultUserDirectory, "games")

    /** Whether the app may use plain paths on shared storage. */
    fun hasAllFilesAccess(context: Context): Boolean {
        if (BuildUtil.isGooglePlayBuild) {
            return false
        }
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            ContextCompat.checkSelfPermission(context, Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
                PackageManager.PERMISSION_GRANTED
        }
    }

    /**
     * Creates the default user and games folders if needed and makes them CitraVR's
     * folders. Returns false if they can't be created.
     */
    fun useDefaultDirectories(context: Context): Boolean {
        val user = defaultUserDirectory
        val games = defaultGamesDirectory
        if (!(user.isDirectory || user.mkdirs()) || !(games.isDirectory || games.mkdirs())) {
            Log.error("[CitraVrDirectories] Cannot create $user or $games")
            return false
        }
        PreferenceManager.getDefaultSharedPreferences(context).edit()
            .putString(PermissionsHandler.CITRA_DIRECTORY, Uri.fromFile(user).toString())
            .putString(GameHelper.KEY_GAME_PATH, Uri.fromFile(games).toString())
            .apply()
        Log.info("[CitraVrDirectories] Using the default folders $user and $games")
        DirectoryInitialization.resetCitraDirectoryState()
        DirectoryInitialization.start()
        return true
    }

    /**
     * With all-files access, turns user and games folders picked with the system
     * file picker into plain paths to the same folders, so they keep working if the
     * picker grants are lost. Folders that can't be mapped to a path are left as is.
     */
    fun migrateToPlainPaths(context: Context) {
        if (!hasAllFilesAccess(context)) {
            return
        }
        val preferences = PreferenceManager.getDefaultSharedPreferences(context)
        val editor = preferences.edit()
        var changed = false
        for (key in listOf(PermissionsHandler.CITRA_DIRECTORY, GameHelper.KEY_GAME_PATH)) {
            val value = preferences.getString(key, "") ?: ""
            if (!value.startsWith("content://")) {
                continue
            }
            val path = try {
                NativeLibrary.getNativePath(Uri.parse(value))
            } catch (e: Exception) {
                ""
            }
            val directory = File(path)
            if (path.isEmpty() || !directory.isDirectory) {
                continue
            }
            editor.putString(key, Uri.fromFile(directory).toString())
            changed = true
            // Native logging may not be up yet.
            android.util.Log.i("CitraVrDirectories", "$key: $value -> $directory")
        }
        if (changed) {
            editor.commit()
        }
    }
}
