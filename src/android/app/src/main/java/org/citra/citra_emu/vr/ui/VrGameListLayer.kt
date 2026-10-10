// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.vr.ui

import android.content.Context
import android.graphics.Bitmap
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import java.lang.ref.WeakReference
import java.nio.IntBuffer
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json
import org.citra.citra_emu.R
import org.citra.citra_emu.model.Game
import org.citra.citra_emu.utils.GameHelper
import org.citra.citra_emu.vr.VrActivity
import org.citra.citra_emu.vr.utils.VrMessageQueue

/**
 * Game list shown in VR after "Close Game", so the next game can be picked
 * without leaving VR. Used where the system doesn't bring CitraVR's 2D window
 * back when the VR session ends (Steam Frame).
 */
class VrGameListLayer(activity: VrActivity) : VrUILayer(activity, R.layout.vr_game_list) {
    private val isSurfaceCreated = AtomicBoolean(false)
    private val adapter = GameListAdapter()
    private var emptyView: TextView? = null

    override fun onSurfaceCreated() {
        super.onSurfaceCreated()
        val window = window!!
        emptyView = window.findViewById(R.id.game_list_empty)
        window.findViewById<RecyclerView>(R.id.game_list).apply {
            layoutManager = LinearLayoutManager(context)
            adapter = this@VrGameListLayer.adapter
        }
        window.findViewById<Button>(R.id.button_back_to_game).setOnClickListener {
            VrMessageQueue.post(VrMessageQueue.MessageType.SHOW_GAME_LIST, 0)
        }
        window.findViewById<Button>(R.id.button_citra_window).setOnClickListener {
            activity.exitToCitraWindow()
        }
        isSurfaceCreated.set(true)
        sVrGameListLayer = WeakReference(this)
    }

    /** Refreshes the list and shows the panel. Returns false if it isn't ready yet. */
    fun show(): Boolean {
        if (!isSurfaceCreated.get()) {
            return false
        }
        Thread {
            val games = loadGames()
            activity.runOnUiThread {
                adapter.setGames(games)
                emptyView?.visibility = if (games.isEmpty()) View.VISIBLE else View.GONE
                VrMessageQueue.post(VrMessageQueue.MessageType.SHOW_GAME_LIST, 1)
            }
        }.start()
        return true
    }

    /**
     * The game list cached by the 2D app (GameHelper.getGames()). Scanning the
     * games folder here would call into the emulator core, which is busy with
     * the current game in this process. (Checking that each file still exists
     * isn't possible either: those document queries fail from this process.)
     */
    private fun loadGames(): List<Game> {
        // MODE_MULTI_PROCESS re-reads the file, which the 2D app's process writes.
        @Suppress("DEPRECATION")
        val preferences = activity.getSharedPreferences(
            "${activity.packageName}_preferences",
            Context.MODE_MULTI_PROCESS
        )
        val storedGames = preferences.getStringSet(GameHelper.KEY_GAMES, emptySet()) ?: emptySet()
        return storedGames.mapNotNull {
            try {
                Json.decodeFromString<Game>(it)
            } catch (e: Exception) {
                null
            }
        }.sortedWith(
            compareBy({ it.title.lowercase(Locale.getDefault()) }, { it.path })
        )
    }

    private inner class GameListAdapter : RecyclerView.Adapter<GameViewHolder>() {
        private var games: List<Game> = emptyList()

        fun setGames(newGames: List<Game>) {
            games = newGames
            @Suppress("NotifyDataSetChanged")
            notifyDataSetChanged()
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): GameViewHolder =
            GameViewHolder(
                LayoutInflater.from(parent.context)
                    .inflate(R.layout.vr_game_list_item, parent, false)
            )

        override fun onBindViewHolder(holder: GameViewHolder, position: Int) {
            val game = games[position]
            holder.title.text = game.title.ifEmpty { game.filename }
            holder.details.text = listOf(game.company, game.regions)
                .filter { it.isNotEmpty() }
                .joinToString(" · ")
            holder.icon.setImageBitmap(decodeIcon(game.icon))
            holder.itemView.setOnClickListener { activity.switchToGame(game) }
        }

        override fun getItemCount(): Int = games.size
    }

    private class GameViewHolder(view: View) : RecyclerView.ViewHolder(view) {
        val icon: ImageView = view.findViewById(R.id.game_icon)
        val title: TextView = view.findViewById(R.id.game_title)
        val details: TextView = view.findViewById(R.id.game_details)
    }

    companion object {
        var sVrGameListLayer: WeakReference<VrGameListLayer?> = WeakReference(null)

        // Same format as GameIconUtils: a 48x48 RGB 565 bitmap.
        private fun decodeIcon(icon: IntArray?): Bitmap? {
            if (icon == null) {
                return null
            }
            return try {
                Bitmap.createBitmap(48, 48, Bitmap.Config.RGB_565).apply {
                    copyPixelsFromBuffer(IntBuffer.wrap(icon))
                }
            } catch (e: RuntimeException) {
                null
            }
        }
    }
}
