// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.fragments

import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.ViewGroup.MarginLayoutParams
import android.widget.CompoundButton
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.preference.PreferenceManager
import kotlinx.coroutines.launch
import org.citra.citra_emu.R
import org.citra.citra_emu.databinding.FragmentRomServerBinding
import org.citra.citra_emu.features.romserver.RomServer
import org.citra.citra_emu.features.romserver.RomServerService
import org.citra.citra_emu.utils.GameHelper

/**
 * The ROM upload server's screen, opened from the navigation rail: turns the
 * server on and off and shows the address to open from a computer.
 */
class RomServerFragment : Fragment() {
    private var _binding: FragmentRomServerBinding? = null
    private val binding get() = _binding!!

    private val onSwitchChanged = CompoundButton.OnCheckedChangeListener { _, isChecked ->
        RomServerService.setEnabled(requireContext(), isChecked)
        updateViews()
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentRomServerBinding.inflate(inflater)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        binding.buttonRomServerRefresh.setOnClickListener {
            RomServer.refreshUrls()
            updateViews()
        }
        viewLifecycleOwner.lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                RomServer.state.collect { updateViews() }
            }
        }
        setInsets()
    }

    override fun onResume() {
        super.onResume()
        updateViews()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    private fun updateViews() {
        val binding = _binding ?: return
        val context = requireContext()
        val isEnabled = RomServerService.isEnabled(context)

        binding.switchRomServer.setOnCheckedChangeListener(null)
        binding.switchRomServer.isChecked = isEnabled
        binding.switchRomServer.setOnCheckedChangeListener(onSwitchChanged)

        val state = RomServer.state.value
        val urls = (state as? RomServer.State.Running)?.urls.orEmpty()
        binding.textRomServerAddress.text = when {
            !isEnabled -> getString(R.string.rom_server_off)
            urls.isNotEmpty() -> urls.joinToString("\n")
            else -> "—"
        }
        binding.textRomServerStatus.text = when {
            !isEnabled -> getString(R.string.rom_server_off)
            state is RomServer.State.Failed -> getString(R.string.rom_server_failed, state.message)
            state is RomServer.State.Running && urls.isEmpty() ->
                getString(R.string.rom_server_no_network)
            state is RomServer.State.Running -> getString(R.string.rom_server_status_running)
            else -> getString(R.string.rom_server_starting)
        }
        binding.buttonRomServerRefresh.isEnabled = isEnabled

        val gamesPath = PreferenceManager.getDefaultSharedPreferences(context)
            .getString(GameHelper.KEY_GAME_PATH, "")
        binding.textRomServerGamesFolder.text = if (gamesPath.isNullOrEmpty()) {
            getString(R.string.rom_server_no_games_folder)
        } else {
            Uri.parse(gamesPath).path ?: gamesPath
        }
    }

    private fun setInsets() = ViewCompat.setOnApplyWindowInsetsListener(
        binding.root
    ) { view: View, windowInsets: WindowInsetsCompat ->
        val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
        val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())
        val spacingNavigation = resources.getDimensionPixelSize(R.dimen.spacing_navigation)
        val spacingNavigationRail =
            resources.getDimensionPixelSize(R.dimen.spacing_navigation_rail)

        binding.scrollViewRomServer.updatePadding(
            top = barInsets.top,
            bottom = barInsets.bottom
        )
        val mlp = binding.scrollViewRomServer.layoutParams as MarginLayoutParams
        mlp.leftMargin = barInsets.left + cutoutInsets.left
        mlp.rightMargin = barInsets.right + cutoutInsets.right
        binding.scrollViewRomServer.layoutParams = mlp

        // Keep clear of the navigation bar (portrait) or rail (landscape).
        binding.linearLayoutRomServer.updatePadding(bottom = spacingNavigation)
        val sidePadding = resources.getDimensionPixelSize(R.dimen.spacing_large)
        if (ViewCompat.getLayoutDirection(view) == ViewCompat.LAYOUT_DIRECTION_LTR) {
            binding.linearLayoutRomServer.updatePadding(left = spacingNavigationRail + sidePadding)
        } else {
            binding.linearLayoutRomServer.updatePadding(right = spacingNavigationRail + sidePadding)
        }

        windowInsets
    }
}
