// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.features.settings.model

import org.citra.citra_emu.features.settings.SettingKeys
import org.citra.citra_emu.vr.utils.VRUtils
import org.citra.citra_emu.vr.utils.VRUtils.hMDType

enum class IntSetting(
    override val key: String,
    override val section: String,
    override val defaultValue: Int
) : AbstractIntSetting {
    FRAME_LIMIT(SettingKeys.frame_limit(), Settings.SECTION_RENDERER, 100),
    EMULATED_REGION(SettingKeys.region_value(), Settings.SECTION_SYSTEM, -1),
    INIT_CLOCK(SettingKeys.init_clock(), Settings.SECTION_SYSTEM, 0),
    CAMERA_INNER_FLIP(SettingKeys.camera_inner_flip(), Settings.SECTION_CAMERA, 0),
    CAMERA_OUTER_LEFT_FLIP(SettingKeys.camera_outer_left_flip(), Settings.SECTION_CAMERA, 0),
    CAMERA_OUTER_RIGHT_FLIP(SettingKeys.camera_outer_right_flip(), Settings.SECTION_CAMERA, 0),
    GRAPHICS_API(SettingKeys.graphics_api(), Settings.SECTION_RENDERER, 2),
    // Keep the CitraVR defaults for resolution (0 = auto/per-HMD) and stereo
    // depth in sync with the native defaults in common/settings.h.
    RESOLUTION_FACTOR(SettingKeys.resolution_factor(), Settings.SECTION_RENDERER, 0),
    STEREOSCOPIC_3D_MODE(SettingKeys.render_3d(), Settings.SECTION_RENDERER, 2),
    STEREOSCOPIC_3D_DEPTH(SettingKeys.factor_3d(), Settings.SECTION_RENDERER, 50),
    STEPS_PER_HOUR(SettingKeys.steps_per_hour(), Settings.SECTION_SYSTEM, 0),
    CARDBOARD_SCREEN_SIZE(SettingKeys.cardboard_screen_size(), Settings.SECTION_LAYOUT, 85),
    CARDBOARD_X_SHIFT(SettingKeys.cardboard_x_shift(), Settings.SECTION_LAYOUT, 0),
    CARDBOARD_Y_SHIFT(SettingKeys.cardboard_y_shift(), Settings.SECTION_LAYOUT, 0),
    SCREEN_LAYOUT(SettingKeys.layout_option(), Settings.SECTION_LAYOUT, 0),
    SMALL_SCREEN_POSITION(SettingKeys.small_screen_position(), Settings.SECTION_LAYOUT, 0),
    LANDSCAPE_TOP_X(SettingKeys.custom_top_x(), Settings.SECTION_LAYOUT, 0),
    LANDSCAPE_TOP_Y(SettingKeys.custom_top_y(), Settings.SECTION_LAYOUT, 0),
    LANDSCAPE_TOP_WIDTH(SettingKeys.custom_top_width(), Settings.SECTION_LAYOUT, 800),
    LANDSCAPE_TOP_HEIGHT(SettingKeys.custom_top_height(), Settings.SECTION_LAYOUT, 480),
    LANDSCAPE_BOTTOM_X(SettingKeys.custom_bottom_x(), Settings.SECTION_LAYOUT, 80),
    LANDSCAPE_BOTTOM_Y(SettingKeys.custom_bottom_y(), Settings.SECTION_LAYOUT, 480),
    LANDSCAPE_BOTTOM_WIDTH(SettingKeys.custom_bottom_width(), Settings.SECTION_LAYOUT, 640),
    LANDSCAPE_BOTTOM_HEIGHT(SettingKeys.custom_bottom_height(), Settings.SECTION_LAYOUT, 480),
    SCREEN_GAP(SettingKeys.screen_gap(), Settings.SECTION_LAYOUT, 0),
    PORTRAIT_SCREEN_LAYOUT(SettingKeys.portrait_layout_option(), Settings.SECTION_LAYOUT, 0),
    SECONDARY_DISPLAY_LAYOUT(SettingKeys.secondary_display_layout(), Settings.SECTION_LAYOUT, 4),
    PORTRAIT_TOP_X(SettingKeys.custom_portrait_top_x(), Settings.SECTION_LAYOUT, 0),
    PORTRAIT_TOP_Y(SettingKeys.custom_portrait_top_y(), Settings.SECTION_LAYOUT, 0),
    PORTRAIT_TOP_WIDTH(SettingKeys.custom_portrait_top_width(), Settings.SECTION_LAYOUT, 800),
    PORTRAIT_TOP_HEIGHT(SettingKeys.custom_portrait_top_height(), Settings.SECTION_LAYOUT, 480),
    PORTRAIT_BOTTOM_X(SettingKeys.custom_portrait_bottom_x(), Settings.SECTION_LAYOUT, 80),
    PORTRAIT_BOTTOM_Y(SettingKeys.custom_portrait_bottom_y(), Settings.SECTION_LAYOUT, 480),
    PORTRAIT_BOTTOM_WIDTH(SettingKeys.custom_portrait_bottom_width(), Settings.SECTION_LAYOUT, 640),
    PORTRAIT_BOTTOM_HEIGHT(
        SettingKeys.custom_portrait_bottom_height(),
        Settings.SECTION_LAYOUT,
        480
    ),
    AUDIO_INPUT_TYPE(SettingKeys.input_type(), Settings.SECTION_AUDIO, 0),
    CPU_CLOCK_SPEED(SettingKeys.cpu_clock_percentage(), Settings.SECTION_CORE, 100),
    TEXTURE_FILTER(SettingKeys.texture_filter(), Settings.SECTION_RENDERER, 0),
    TEXTURE_SAMPLING(SettingKeys.texture_sampling(), Settings.SECTION_RENDERER, 0),
    USE_FRAME_LIMIT(SettingKeys.use_frame_limit(), Settings.SECTION_RENDERER, 1),
    DELAY_RENDER_THREAD_US(SettingKeys.delay_game_render_thread_us(), Settings.SECTION_RENDERER, 0),
    ORIENTATION_OPTION(SettingKeys.screen_orientation(), Settings.SECTION_LAYOUT, 2),
    TURBO_LIMIT(SettingKeys.turbo_limit(), Settings.SECTION_CORE, 200),
    PERFORMANCE_OVERLAY_POSITION(SettingKeys.performance_overlay_position(), Settings.SECTION_LAYOUT, 0),
    RENDER_3D_WHICH_DISPLAY(SettingKeys.render_3d_which_display(),Settings.SECTION_RENDERER,0),
    ASPECT_RATIO(SettingKeys.aspect_ratio(), Settings.SECTION_LAYOUT, 0),
    VR_ENVIRONMENT(
        "vr_environment",
        Settings.SECTION_VR,
        if (hMDType == VRUtils.HMDType.QUEST3.value) 1 else 2
    ),
    // CitraVR: Default to the highest CPU clock level
    // Keep in sync with the native default in jni/config.cpp.
    VR_CPU_LEVEL("vr_cpu_level", Settings.SECTION_VR, 4),
    VR_IMMERSIVE_MODE("vr_immersive_mode", Settings.SECTION_VR, 0),
    VR_IMMERSIVE_POSITIONAL_GAME_SCALER(
        "vr_immersive_positional_game_scaler",
        Settings.SECTION_VR,
        0
    ),
    UPDATE_CHECK_CHANNEL(SettingKeys.update_check_channel(), Settings.SECTION_MISC, 0),
    VR_SI_MODE_REGISTER_OFFSET("vr_si_mode_register_offset", Settings.SECTION_VR, -1),

    // Controller mapping: the 3DS button each controller input presses, as the
    // Android key code sent to the emulated gamepad (0: none). Shown per 3DS
    // button by VrButtonMappingSetting.
    // Defaults (keep in sync with vr_settings.h and config.cpp): the Nintendo
    // layout on the face buttons (the 3DS's B where the Frame's A is), L/R on the
    // bumpers, Select/Start on the stick clicks, ZL/ZR on the triggers, Home on
    // View and the menu bar (VrButtonMappingSetting.MENU_BAR) on Menu. Headsets
    // without bumpers or a right Menu button have L/R on the grips and the menu
    // bar on the left Menu button. Triggers only press their button when pulled
    // while the controller doesn't point at a panel (otherwise they click).
    VR_MAP_A("vr_map_a", Settings.SECTION_VR, 97),
    VR_MAP_B("vr_map_b", Settings.SECTION_VR, 96),
    VR_MAP_X("vr_map_x", Settings.SECTION_VR, 100),
    VR_MAP_Y("vr_map_y", Settings.SECTION_VR, 99),
    VR_MAP_DPAD_UP("vr_map_dpad_up", Settings.SECTION_VR, 19),
    VR_MAP_DPAD_DOWN("vr_map_dpad_down", Settings.SECTION_VR, 20),
    VR_MAP_DPAD_LEFT("vr_map_dpad_left", Settings.SECTION_VR, 21),
    VR_MAP_DPAD_RIGHT("vr_map_dpad_right", Settings.SECTION_VR, 22),
    VR_MAP_LEFT_BUMPER("vr_map_left_bumper", Settings.SECTION_VR, if (IS_STEAM_FRAME) 102 else 0),
    VR_MAP_RIGHT_BUMPER("vr_map_right_bumper", Settings.SECTION_VR, if (IS_STEAM_FRAME) 103 else 0),
    VR_MAP_LEFT_GRIP("vr_map_left_grip", Settings.SECTION_VR, if (IS_STEAM_FRAME) 0 else 102),
    VR_MAP_RIGHT_GRIP("vr_map_right_grip", Settings.SECTION_VR, if (IS_STEAM_FRAME) 0 else 103),
    VR_MAP_LEFT_STICK_CLICK("vr_map_left_stick_click", Settings.SECTION_VR, 109),
    VR_MAP_RIGHT_STICK_CLICK("vr_map_right_stick_click", Settings.SECTION_VR, 108),
    VR_MAP_LEFT_TRIGGER("vr_map_left_trigger", Settings.SECTION_VR, 104),
    VR_MAP_RIGHT_TRIGGER("vr_map_right_trigger", Settings.SECTION_VR, 105),
    VR_MAP_LEFT_VIEW("vr_map_left_view", Settings.SECTION_VR, if (IS_STEAM_FRAME) 110 else 1000),
    VR_MAP_RIGHT_MENU("vr_map_right_menu", Settings.SECTION_VR, if (IS_STEAM_FRAME) 1000 else 0);

    override var int: Int = defaultValue

    override val valueAsString: String
        get() = int.toString()

    override val isRuntimeEditable: Boolean
        get() {
            for (setting in NOT_RUNTIME_EDITABLE) {
                if (setting == this) {
                    return false
                }
            }
            return true
        }

    companion object {
        private val NOT_RUNTIME_EDITABLE = listOf(
            EMULATED_REGION,
            INIT_CLOCK,
            GRAPHICS_API,
            AUDIO_INPUT_TYPE,
            RESOLUTION_FACTOR,
            VR_ENVIRONMENT,
            VR_CPU_LEVEL,
            VR_IMMERSIVE_MODE
        )

        fun from(key: String): IntSetting? = IntSetting.values().firstOrNull { it.key == key }

        fun clear() = IntSetting.values().forEach { it.int = it.defaultValue }
    }
}

// Steam Frame (Lepton reports this model), for its controller mapping defaults.
private val IS_STEAM_FRAME = android.os.Build.MODEL == "Lepton"
