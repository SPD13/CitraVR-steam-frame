/*******************************************************************************

Filename    :   vr_settings.h

Content     :   VR-specific settings initialized in config.cpp

Authors     :   Amanda M. Watson
License     :   Licensed under GPLv3 or any later version.
                Refer to the license.txt file included.

*******************************************************************************/

#pragma once

#pragma once

#include "OpenXR.h"

#include <array>
#include <cstddef>
#include <string>

// Note: update this if CPU levels are added to OpenXR
#define XR_HIGHEST_CPU_PERF_LEVEL XR_PERF_SETTINGS_LEVEL_BOOST_EXT
#define XR_HIGHEST_CPU_PREFERENCE 4 // corresponds to XR_HIGHEST_CPU_PERF_LEVEL in quest logging

namespace VRSettings {

// NOTE: keep this in-sync with HMDType in sync with VRUtils.java
enum class HMDType { UNKNOWN = 0, QUEST1, QUEST2, QUEST3, QUESTPRO, QUEST3S, STEAMFRAME };

enum class VREnvironmentType { PASSTHROUGH = 1, VOID = 2 };

// Given a CPU level preference, return the corresponding OpenXR performance
// level
XrPerfSettingsLevelEXT CPUPrefToPerfSettingsLevel(const int32_t cpu_level_pref);
// Return the HMD type as a string (queried from ro.product.model)
std::string GetHMDTypeStr();
// Given a string from GetHMDTypeStr(), return the corresponding HMDType
HMDType HmdTypeFromStr(const std::string& hmdType);

// Controller controls that can be mapped to a 3DS button in the settings.
enum class VrControl : size_t {
    A,
    B,
    X,
    Y,
    DPAD_UP,
    DPAD_DOWN,
    DPAD_LEFT,
    DPAD_RIGHT,
    LEFT_BUMPER,
    RIGHT_BUMPER,
    LEFT_GRIP,
    RIGHT_GRIP,
    LEFT_STICK_CLICK,
    RIGHT_STICK_CLICK,
    // Only when pulled while the controller doesn't point at a panel (otherwise
    // they click; see UpdateTriggerRoles() in vr_main.cpp).
    LEFT_TRIGGER,
    RIGHT_TRIGGER,
    LEFT_VIEW,  // left Menu button on Quest
    RIGHT_MENU, // Steam Frame
    COUNT
};

// A mapping value that isn't a 3DS button: shows or hides the menu bar.
constexpr int32_t kVrActionToggleMenuBar = 1000;
constexpr size_t kNumVrControls = static_cast<size_t>(VrControl::COUNT);

// Each control's config key ([VR] section, also per game) and default 3DS button
// on Steam Frame (other headsets have L/R on the grips, and the menu bar on the
// left Menu button, as they have no bumpers or right Menu button; see
// config.cpp). Buttons are Android key codes, as sent to the emulated gamepad
// (0: none). The settings UI keeps the mapping one to one.
struct VrControlMapping {
    const char* key;
    int32_t     defaultKeyCode;
};
constexpr std::array<VrControlMapping, kNumVrControls> kVrControlMappings = {{
    {"vr_map_a", 97},                // BUTTON_B: the 3DS's B sits where the Frame's A is
    {"vr_map_b", 96},                // BUTTON_A
    {"vr_map_x", 100},               // BUTTON_Y
    {"vr_map_y", 99},                // BUTTON_X
    {"vr_map_dpad_up", 19},          // DPAD_UP
    {"vr_map_dpad_down", 20},        // DPAD_DOWN
    {"vr_map_dpad_left", 21},        // DPAD_LEFT
    {"vr_map_dpad_right", 22},       // DPAD_RIGHT
    {"vr_map_left_bumper", 102},     // BUTTON_L1 (3DS L)
    {"vr_map_right_bumper", 103},    // BUTTON_R1 (3DS R)
    {"vr_map_left_grip", 0},         // none
    {"vr_map_right_grip", 0},        // none
    {"vr_map_left_stick_click", 109},  // BUTTON_SELECT
    {"vr_map_right_stick_click", 108}, // BUTTON_START
    {"vr_map_left_trigger", 104},    // BUTTON_L2 (3DS ZL)
    {"vr_map_right_trigger", 105},   // BUTTON_R2 (3DS ZR)
    {"vr_map_left_view", 110},       // BUTTON_MODE (3DS Home)
    {"vr_map_right_menu", kVrActionToggleMenuBar},
}};

constexpr std::array<int32_t, kNumVrControls> DefaultVrButtonMap() {
    std::array<int32_t, kNumVrControls> map{};
    for (size_t i = 0; i < kNumVrControls; i++) {
        map[i] = kVrControlMappings[i].defaultKeyCode;
    }
    return map;
}

struct Values {
    XrPerfSettingsLevelEXT cpu_level         = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
    HMDType                hmd_type          = HMDType::UNKNOWN;
    uint32_t               resolution_factor = 0;
    int32_t                vr_environment    = 0;
    int32_t                vr_immersive_mode = 0;
    int32_t                vr_si_mode_register_offset          = -1;
    int32_t                vr_factor_3d                        = 100;
    int32_t                vr_immersive_positional_game_scaler = 0;
    std::string            vr_immersive_eye_indicator;
    bool                   vr_show_controllers                 = true;
    bool                   vr_show_pointer_ray                 = true;
    bool                   vr_show_controller_diagram          = true;
    // The 3DS button (Android key code, 0: none) of each VrControl.
    std::array<int32_t, kNumVrControls> vr_button_map = DefaultVrButtonMap();
} extern values;

} // namespace VRSettings
