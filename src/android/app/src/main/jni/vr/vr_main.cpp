/*******************************************************************************

Filename    :   vr_main.cpp
Content     :   VR entrypoint for Android. Called from onCreate to iniitalize
                the "VrApp" thread, which handles OpenXR and XR-specific
                rendering.
Authors     :   Amanda M. Watson
License     :   Licensed under GPLv3 or any later version.
                Refer to the license.txt file included.

*******************************************************************************/

#include "OpenXR.h"
#include "utils/LogUtils.h"

#include "XrController.h"
#include "layers/ControllerDiagramLayer.h"
#include "layers/ControllerLayer.h"
#include "layers/CursorLayer.h"
#include "layers/GameSurfaceLayer.h"
#include "layers/PassthroughLayer.h"
#include "layers/RibbonLayer.h"
#include "layers/UILayer.h"
#include "vr_settings.h"

#include "utils/Common.h"
#include "utils/JniClassNames.h"
#include "utils/MessageQueue.h"
#include "utils/SyspropUtils.h"
#include "utils/XrMath.h"

#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <assert.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "common/stall_telemetry.h"
#include "core/core.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

#if defined(DEBUG_INPUT_VERBOSE)
#define ALOG_INPUT_VERBOSE(...) ALOGI(__VA_ARGS__)
#else
#define ALOG_INPUT_VERBOSE(...)
#endif

// Used by Citra core to set a higher priority to the non-VR render thread.
namespace vr {
XrSession  gSession     = XR_NULL_HANDLE;
int        gPriorityTid = -1;
bool       gCitraReady  = false;
XrSession& GetSession() { return gSession; }
void       PrioritizeTid(const int tid) {
          assert(gSession != XR_NULL_HANDLE);
          if (gSession == XR_NULL_HANDLE) {
              ALOGE("PrioritizeTid() called before session is initialized");
              return;
    }
    if (!OpenXr::GetExtensions().mAndroidThreadSettings) {
        return;
    }
          PFN_xrSetAndroidApplicationThreadKHR pfnSetAndroidApplicationThreadKHR = NULL;
          OXR(xrGetInstanceProcAddr(OpenXr::GetInstance(), "xrSetAndroidApplicationThreadKHR",
                                    (PFN_xrVoidFunction*)(&pfnSetAndroidApplicationThreadKHR)));

          OXR(pfnSetAndroidApplicationThreadKHR(gSession, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, tid));
          gPriorityTid = tid;
          ALOGD("Setting prio tid from original code {}", vr::gPriorityTid);
}

void SetCitraReady() { gCitraReady = true; }
} // namespace vr

namespace {
constexpr XrPerfSettingsLevelEXT                   kGpuPerfLevel = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
std::chrono::time_point<std::chrono::steady_clock> gOnCreateStartTime;
std::unique_ptr<OpenXr>                            gOpenXr;
MessageQueue                                       gMessageQueue;

const std::vector<float> immersiveScaleFactor = {1.0f, 3.0f, 1.4f};

void ForwardButtonStateChangeToCitra(JNIEnv* jni, jobject activityObject,
                                     jmethodID forwardVRInputMethodID, const int androidButtonCode,
                                     const XrBool32 xrButtonState) {
    jni->CallVoidMethod(activityObject, forwardVRInputMethodID, androidButtonCode,
                        xrButtonState != XR_FALSE);
}

// Whether the cursor is hovering over the settings menu panel. Set during the
// cursor pass and consumed by input handling on the following frame: while
// hovering, the pointing hand's thumbstick scrolls the settings list, so its
// state must not also be forwarded to the game.
bool sIsCursorHoveringSettingsMenu = false;

void SendTriggerStateToWindow(JNIEnv* jni, jobject activityObject,
                              jmethodID                   sendClickToWindowMethodID,
                              const XrActionStateBoolean& triggerState,
                              const XrVector2f&           cursorPos2d) {
    if (triggerState.currentState == 0 && triggerState.changedSinceLastSync) {
        jni->CallVoidMethod(activityObject, sendClickToWindowMethodID, cursorPos2d.x, cursorPos2d.y,
                            0);
    } else if (triggerState.changedSinceLastSync && triggerState.currentState == 1) {
        jni->CallVoidMethod(activityObject, sendClickToWindowMethodID, cursorPos2d.x, cursorPos2d.y,
                            1);
    } else if (triggerState.currentState == 1 && !triggerState.changedSinceLastSync) {
        jni->CallVoidMethod(activityObject, sendClickToWindowMethodID, cursorPos2d.x, cursorPos2d.y,
                            2);
    }
}

[[maybe_unused]] const char* XrSessionStateToString(const XrSessionState state) {
    switch (state) {
        case XR_SESSION_STATE_UNKNOWN:
            return "XR_SESSION_STATE_UNKNOWN";
        case XR_SESSION_STATE_IDLE:
            return "XR_SESSION_STATE_IDLE";
        case XR_SESSION_STATE_READY:
            return "XR_SESSION_STATE_READY";
        case XR_SESSION_STATE_SYNCHRONIZED:
            return "XR_SESSION_STATE_SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE:
            return "XR_SESSION_STATE_VISIBLE";
        case XR_SESSION_STATE_FOCUSED:
            return "XR_SESSION_STATE_FOCUSED";
        case XR_SESSION_STATE_STOPPING:
            return "XR_SESSION_STATE_STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING:
            return "XR_SESSION_STATE_LOSS_PENDING";
        case XR_SESSION_STATE_EXITING:
            return "XR_SESSION_STATE_EXITING";
        case XR_SESSION_STATE_MAX_ENUM:
            return "XR_SESSION_STATE_MAX_ENUM";
        default:
            return "Unknown";
    }
}

const char* XrPerfSettingsDomainToString(const XrPerfSettingsDomainEXT domain) {
    switch (domain) {
        case XR_PERF_SETTINGS_DOMAIN_CPU_EXT:
            return "CPU";
        case XR_PERF_SETTINGS_DOMAIN_GPU_EXT:
            return "GPU";
        default:
            return "Unknown";
    }
}

const char* XrPerfSettingsSubDomainToString(const XrPerfSettingsSubDomainEXT subDomain) {
    switch (subDomain) {
        case XR_PERF_SETTINGS_SUB_DOMAIN_COMPOSITING_EXT:
            return "compositing";
        case XR_PERF_SETTINGS_SUB_DOMAIN_RENDERING_EXT:
            return "rendering";
        case XR_PERF_SETTINGS_SUB_DOMAIN_THERMAL_EXT:
            return "thermal";
        default:
            return "unknown";
    }
}

const char*
XrPerfSettingsNotificationLevelToString(const XrPerfSettingsNotificationLevelEXT level) {
    switch (level) {
        case XR_PERF_SETTINGS_NOTIF_LEVEL_NORMAL_EXT:
            return "NORMAL";
        case XR_PERF_SETTINGS_NOTIF_LEVEL_WARNING_EXT:
            return "WARNING";
        case XR_PERF_SETTINGS_NOTIF_LEVEL_IMPAIRED_EXT:
            return "IMPAIRED";
        default:
            return "UNKNOWN";
    }
}

// Logs a prominent, greppable marker plus a snapshot of the stall telemetry.
// Triggered by the user clicking a thumbstick when they see a visible
// stall/artifact, so log readers know where in the log to look.
void LogUserStallMarker(const uint64_t frameIndex) {
    auto&          telemetry = Common::StallTelemetry::Get();
    const uint32_t markerIndex =
        telemetry.user_marker_count.fetch_add(1, std::memory_order_relaxed) + 1;

    ALOGW("==================== [USER-MARKER #{}] ====================", markerIndex);
    ALOGW("[USER-MARKER #{}] VR frame {}: user flagged a visible stall/artifact here",
          markerIndex, frameIndex);

    const uint32_t compiles = telemetry.shader_compile_count.load(std::memory_order_relaxed);
    const double   compileTimeMs =
        telemetry.shader_compile_time_us.load(std::memory_order_relaxed) / 1000.0;
    const int64_t lastCompileUs =
        telemetry.last_shader_compile_steady_us.load(std::memory_order_relaxed);
    if (lastCompileUs > 0) {
        ALOGW("[USER-MARKER #{}] shaders: {} draw-time compiles ({:.1f} ms total), last one "
              "{:.1f} s ago",
              markerIndex, compiles, compileTimeMs,
              (Common::SteadyNowUs() - lastCompileUs) / 1e6);
    } else {
        ALOGW("[USER-MARKER #{}] shaders: no draw-time compiles yet", markerIndex);
    }

    ALOGW("[USER-MARKER #{}] emu: slowFrames={} stallFrames={} worstFrame={:.1f} ms",
          markerIndex, telemetry.emu_slow_frame_count.load(std::memory_order_relaxed),
          telemetry.emu_stall_frame_count.load(std::memory_order_relaxed),
          telemetry.emu_worst_frame_time_us.load(std::memory_order_relaxed) / 1000.0);

    ALOGW("[USER-MARKER #{}] vr: missedFrames={} worstFrameGap={:.1f} ms worstWaitFrame={:.1f} ms",
          markerIndex, telemetry.vr_missed_frame_count.load(std::memory_order_relaxed),
          telemetry.vr_worst_frame_gap_us.load(std::memory_order_relaxed) / 1000.0,
          telemetry.vr_worst_wait_frame_us.load(std::memory_order_relaxed) / 1000.0);

    auto& system = Core::System::GetInstance();
    if (system.IsPoweredOn() && system.perf_stats != nullptr) {
        const auto stats = system.perf_stats->GetLastStats();
        ALOGW("[USER-MARKER #{}] game (last interval): fps={:.1f} speed={:.0f}% frame={:.1f} ms "
              "(svc={:.1f} ipc={:.1f} gpu={:.1f} swap={:.1f} other={:.1f} ms) peak={:.1f} ms",
              markerIndex, stats.game_fps, stats.emulation_speed * 100.0,
              stats.time_vblank_interval * 1000.0, stats.time_hle_svc * 1000.0,
              stats.time_hle_ipc * 1000.0, stats.time_gpu * 1000.0, stats.time_swap * 1000.0,
              stats.time_remaining * 1000.0, stats.frametime_peak * 1000.0);
    }
    ALOGW("============================================================");
}

uint32_t GetDefaultGameResolutionFactorForHmd(const VRSettings::HMDType& hmdType) {
    static constexpr uint32_t kDefaultResolutionFactor = 2;
    switch (hmdType) {
        case VRSettings::HMDType::QUEST3:
        case VRSettings::HMDType::STEAMFRAME:
            return 3;
        case VRSettings::HMDType::UNKNOWN:
            ALOGW("Warning: Unknown HMD type, using default scale factor of {}",
                  kDefaultResolutionFactor);
        case VRSettings::HMDType::QUEST1:
            ALOGW("Warning: Unsupported HMD type, using default scale factor "
                  "of {}",
                  kDefaultResolutionFactor);
        case VRSettings::HMDType::QUEST2:
        case VRSettings::HMDType::QUESTPRO:
        case VRSettings::HMDType::QUEST3S:
            return kDefaultResolutionFactor;
    }
}

// Called whenever a session is started/resumed. Creates the head space based on the
// current pose of the HMD.
void CreateRuntimeInitatedReferenceSpaces(const XrTime predictedDisplayTime) {
    // Create a reference space with the forward direction from the
    // starting frame.
    {
        const XrReferenceSpaceCreateInfo sci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
                                                XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                XrMath::Posef::Identity()};
        OXR(xrCreateReferenceSpace(gOpenXr->mSession, &sci, &gOpenXr->mForwardDirectionSpace));
    }

    {
        const XrReferenceSpaceCreateInfo sci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
                                                XR_REFERENCE_SPACE_TYPE_VIEW,
                                                XrMath::Posef::Identity()};
        OXR(xrCreateReferenceSpace(gOpenXr->mSession, &sci, &gOpenXr->mViewSpace));
    }

    // Get the pose of the local space.
    XrSpaceLocation lsl = {XR_TYPE_SPACE_LOCATION};
    OXR(xrLocateSpace(gOpenXr->mForwardDirectionSpace, gOpenXr->mLocalSpace, predictedDisplayTime,
                      &lsl));

    // Set the forward direction of the new space.
    const XrPosef forwardDirectionPose = lsl.pose;

    // Create a reference space with the same position and rotation as
    // local.
    const XrReferenceSpaceCreateInfo sci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
                                            XR_REFERENCE_SPACE_TYPE_LOCAL, forwardDirectionPose};
    OXR(xrCreateReferenceSpace(gOpenXr->mSession, &sci, &gOpenXr->mHeadSpace));
}

} // anonymous namespace

//-----------------------------------------------------------------------------
// VRApp

class VRApp {
public:
    VRApp(jobject activityObjectGlobalRef)
        : mActivityObject(activityObjectGlobalRef) {}

    ~VRApp() { assert(mLastAppState.mIsStopRequested); }

private:
    class AppState;

public:
    void MainLoop(JNIEnv* jni) {

        //////////////////////////////////////////////////
        // Init
        //////////////////////////////////////////////////

        Init(jni);

        //////////////////////////////////////////////////
        // Frame loop
        //////////////////////////////////////////////////

        while (true) {
            // Handle events/state-changes.
            AppState appState = HandleEvents(jni);
            if (appState.mIsStopRequested) { break; }
            HandleStateChanges(jni, appState);
            if (appState.mIsXrSessionActive) {
                // Increment the frame index.
                // Frame index starts at 1. I don't know why, we've always done this.
                // Doesn't actually matter, except to make the indices
                // consistent in traces
                mFrameIndex++;
                // Log time to first frame
                if (mFrameIndex == 1) {
                    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
                    ALOGI("Time to first frame: {} ms",
                          std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                                gOnCreateStartTime)
                              .count());
                }

                // Update non-tracking-dependent-state.
                mInputStateFrame.SyncButtonsAndThumbSticks(gOpenXr->mSession, mInputStateStatic);
                UpdateTriggerRoles(appState);
                HandleInput(jni, mInputStateFrame, appState);

                Frame(jni, appState);
            } else {
                // FIXME: currently, the way this is set up, some messages can be discarded if they
                // aren't processed on the next frame.
                // This currently requires that all AppState state-related events be handled in
                // HandleStateChanges. and not in the frame for 100% correctness (consequence is
                // losing messages on unmount). This is possibly solved by handling MessageQueue
                // events inside the frame call.
                // TODO should block here
                mFrameIndex = 0;
            }
            mLastAppState = appState;
        }

        //////////////////////////////////////////////////
        // Exit
        //////////////////////////////////////////////////

        ALOGI("::MainLoop() exiting");
    }

private:
    void Init(JNIEnv* jni) {
        assert(gOpenXr != nullptr);
        mInputStateStatic =
            std::make_unique<InputStateStatic>(OpenXr::GetInstance(), gOpenXr->mSession);

        //////////////////////////////////////////////////
        // Create the layers
        //////////////////////////////////////////////////

        // The background: passthrough or the void, from the settings at first, then
        // from the menu bar's Passthrough button (see UpdatePassthrough()).
        assert(VRSettings::values.vr_environment ==
                   static_cast<int32_t>(VRSettings::VREnvironmentType::VOID) ||
               VRSettings::values.vr_environment ==
                   static_cast<int32_t>(VRSettings::VREnvironmentType::PASSTHROUGH));
        mLastAppState.mIsPassthroughOn =
            VRSettings::values.vr_environment ==
            static_cast<int32_t>(VRSettings::VREnvironmentType::PASSTHROUGH);

        // Create the game surface layer.
        {
            ALOGI("VR Extra Performance Mode: {}",
                  Settings::values.vr_extra_performance_mode_enabled.GetValue() ? "enabled"
                                                                                : "disabled");

            const uint32_t defaultResolutionFactor =
                GetDefaultGameResolutionFactorForHmd(VRSettings::values.hmd_type);
            const uint32_t resolutionFactorFromPreferences = VRSettings::values.resolution_factor;
            // add a couple factors to resolution with immersive mode so users
            // aren't resetting their default settings to get higher res. min
            // resolution factor for immersive is 3x.
            const uint32_t immersiveModeOffset = (VRSettings::values.vr_immersive_mode > 0) ? 2 : 0;
            const uint32_t resolutionFactor =
                (resolutionFactorFromPreferences > 0 ? resolutionFactorFromPreferences
                                                     : defaultResolutionFactor) +
                immersiveModeOffset;

            if (resolutionFactor != defaultResolutionFactor) {
                ALOGI("Using resolution factor of {}x instead of HMD default {}x", resolutionFactor,
                      defaultResolutionFactor);
            }
            mGameSurfaceLayer = std::make_unique<GameSurfaceLayer>(
                XrVector3f{0, 0, VRSettings::values.vr_immersive_mode > 0 ? -0.5f : -2.0f}, jni,
                mActivityObject, gOpenXr->mSession, resolutionFactor);
        }

        mRibbonLayer = std::make_unique<RibbonLayer>(mGameSurfaceLayer->GetLowerPanelPose(), jni,
                                                     mActivityObject, gOpenXr->mSession);

        mKeyboardLayer = std::make_unique<UILayer>(
            VR::JniGlobalRef::gVrKeyboardLayerClass, XrVector3f{0, -0.4f, -0.5f},
            XrMath::Quatf::FromEuler(-MATH_FLOAT_PI / 4.0f, 0.0f, 0.0f), jni, mActivityObject,
            gOpenXr->mSession);

        mErrorMessageLayer = std::make_unique<UILayer>(
            VR::JniGlobalRef::gVrErrorMessageLayerClass, XrVector3f{0, -0.1f, -1.0f},
            XrQuaternionf{0, 0, 0, 1}, jni, mActivityObject, gOpenXr->mSession);

        // Game list shown after "Close Game", in place of the game screens.
        mGameListLayer = std::make_unique<UILayer>(
            VR::JniGlobalRef::gVrGameListLayerClass, XrVector3f{0, -0.1f, -1.4f},
            XrQuaternionf{0, 0, 0, 1}, jni, mActivityObject, gOpenXr->mSession);

        // Controller diagram, labelled with the current mapping: 90 degrees to the
        // left, facing the user, to check the mapping by turning one's head.
        mControllerDiagramLayer = std::make_unique<ControllerDiagramLayer>(
            XrPosef{XrQuaternionf{0, 0.70710678f, 0, 0.70710678f}, XrVector3f{-1.3f, -0.1f, 0}},
            jni, mActivityObject, gOpenXr->mSession);
        // Opposite it, 90 degrees to the right: the 3DS-style button layout, to compare.
        mButtonLayoutLayer = std::make_unique<UILayer>(
            VR::JniGlobalRef::gVrButtonLayoutLayerClass, XrVector3f{1.3f, -0.1f, 0},
            XrQuaternionf{0, -0.70710678f, 0, 0.70710678f}, jni, mActivityObject,
            gOpenXr->mSession);

        // Create the cursor layer.
        mCursorLayer = std::make_unique<CursorLayer>(gOpenXr->mSession);

        // Controllers and pointer ray, drawn in 3D over the panels (VR settings).
        if (VRSettings::values.vr_show_controllers || VRSettings::values.vr_show_pointer_ray) {
            mControllerLayer = std::make_unique<ControllerLayer>(gOpenXr->mSession);
        }

        //////////////////////////////////////////////////
        // Intialize JNI methods
        //////////////////////////////////////////////////

        mForwardVRInputMethodID =
            jni->GetMethodID(jni->GetObjectClass(mActivityObject), "forwardVRInput", "(IZ)V");
        if (mForwardVRInputMethodID == nullptr) { FAIL("could not get forwardVRInputMethodID"); }
        mOnVrControlPressedMethodID =
            jni->GetMethodID(jni->GetObjectClass(mActivityObject), "onVrControlPressed", "(I)V");
        if (mOnVrControlPressedMethodID == nullptr) { FAIL("could not get onVrControlPressed"); }
        mForwardVRJoystickMethodID =
            jni->GetMethodID(jni->GetObjectClass(mActivityObject), "forwardVRJoystick", "(FFI)V");
        if (mForwardVRJoystickMethodID == nullptr) {
            FAIL("could not get forwardVRJoystickMethodID");
        }

        mSendClickToWindowMethodID =
            jni->GetMethodID(jni->GetObjectClass(mActivityObject), "sendClickToWindow", "(FFI)V");
        if (mSendClickToWindowMethodID == nullptr) {
            FAIL("could not get sendClickToWindowMethodID");
        }
        mResumeGameMethodID =
            jni->GetMethodID(jni->GetObjectClass(mActivityObject), "resumeGame", "()V");
        if (mResumeGameMethodID == nullptr) { FAIL("could not get resumeGameMethodID"); }
        mPauseGameMethodID =
            jni->GetMethodID(jni->GetObjectClass(mActivityObject), "pauseGame", "()V");
        if (mPauseGameMethodID == nullptr) { FAIL("could not get pauseGameMethodID"); }
        mOpenSettingsMethodID =
            jni->GetMethodID(jni->GetObjectClass(mActivityObject), "openSettingsMenu", "()V");
        if (mOpenSettingsMethodID == nullptr) { FAIL("could not get openSettingsMenuMethodID"); }

        if (VRSettings::values.vr_immersive_mode != 0) {
            mLastAppState.mIsLowerMenuToggledOn = false;
        }
    }

    // Creates or removes the passthrough layer to match the requested state, and
    // tells the menu bar (its Passthrough button) when the state changes.
    void UpdatePassthrough(const bool isRequested) {
        const bool isSupported =
            OpenXr::GetExtensions().mPassthroughFB || gOpenXr->mAlphaBlendSupported;
        const bool shouldBeOn = isRequested && isSupported;
        const bool isOn = mPassthroughLayer != nullptr || mAlphaBlendPassthroughLayer != nullptr;
        if (shouldBeOn && !isOn) {
            if (OpenXr::GetExtensions().mPassthroughFB) {
                mPassthroughLayer = std::make_unique<PassthroughLayer>(gOpenXr->mSession);
            } else {
                mAlphaBlendPassthroughLayer =
                    std::make_unique<AlphaBlendPassthroughLayer>(gOpenXr->mSession);
            }
        } else if (!shouldBeOn && isOn) {
            mPassthroughLayer.reset();
            mAlphaBlendPassthroughLayer.reset();
        }
        if (!mHasReportedPassthrough || shouldBeOn != mReportedPassthroughState) {
            if (isRequested && !isSupported) {
                ALOGW("Passthrough is not supported by this runtime; using the void");
            }
            mRibbonLayer->SetPassthroughState(shouldBeOn, isSupported);
            mReportedPassthroughState = shouldBeOn;
            mHasReportedPassthrough = true;
        }
    }

    void Frame(JNIEnv* jni, const AppState& appState) {

        const auto frameLoopStart = std::chrono::steady_clock::now();

        ////////////////////////////////
        // XrWaitFrame()
        ////////////////////////////////

        // XrWaitFrame returns the predicted display time.
        XrFrameState frameState = {XR_TYPE_FRAME_STATE, nullptr};

        {
            XrFrameWaitInfo wfi = {XR_TYPE_FRAME_WAIT_INFO, nullptr};
            OXR(xrWaitFrame(gOpenXr->mSession, &wfi, &frameState));
        }

        const auto waitFrameEnd = std::chrono::steady_clock::now();

        ////////////////////////////////
        // XrBeginFrame()
        ////////////////////////////////

        {
            XrFrameBeginInfo bfd = {XR_TYPE_FRAME_BEGIN_INFO, nullptr};
            OXR(xrBeginFrame(gOpenXr->mSession, &bfd));
        }

        ///////////////////////////////////////////////////
        // Get tracking, space, projection info for frame.
        ///////////////////////////////////////////////////

        // Re-initialize the reference spaces on the first frame so
        // it is in-sync with user
        if (mFrameIndex == 1) {
            CreateRuntimeInitatedReferenceSpaces(frameState.predictedDisplayTime);
        }

        gOpenXr->headLocation = {XR_TYPE_SPACE_LOCATION};
        OXR(xrLocateSpace(gOpenXr->mViewSpace, gOpenXr->mHeadSpace, frameState.predictedDisplayTime,
                          &gOpenXr->headLocation));

        mInputStateFrame.SyncHandPoses(gOpenXr->mSession, mInputStateStatic, gOpenXr->mLocalSpace,
                                       frameState.predictedDisplayTime);

        ///////////////////////////////////////////////////
        // Super Immersive Mode update and computation.
        //////////////////////////////////////////////////

        // enable toggle when menu is set to main. Otherwise, always on (super immersive disabled).
        // The game list replaces the game screens and menu bar while it's shown.
        const PanelVisibility visibility = GetPanelVisibility(appState);
        const bool showGameList = visibility.mShowGameList;
        const bool showUIRibbon = visibility.mShowUIRibbon;

        float      immersiveModeFactor    = (VRSettings::values.vr_immersive_mode < 2)
                                                ? immersiveScaleFactor[VRSettings::values.vr_immersive_mode]
                                                : immersiveScaleFactor[2];
        const bool isImmersiveModeEnabled = [this](const bool showUIRibbon,
                                                   float&     immersiveModeFactor) -> bool {
            if (VRSettings::values.vr_immersive_mode != 0 &&
                (!showUIRibbon || VRSettings::values.vr_immersive_mode == 1)) {
                UpdateImmersiveModeIfNeeded(immersiveModeFactor);
                return true;
            } else {
                immersiveModeFactor = 1.0f;
                DisableImmersiveMode();
                return false;
            }
        }(showUIRibbon, immersiveModeFactor);

        //////////////////////////////////////////////////
        //  Set the compositor layers for this frame.
        //////////////////////////////////////////////////

        // Copy any new UI frames into the UI layers' swapchains (if the runtime needs
        // that), whether or not they are shown, so Android never waits on them.
        mRibbonLayer->UpdateSurface();
        mKeyboardLayer->UpdateSurface();
        mErrorMessageLayer->UpdateSurface();
        mGameListLayer->UpdateSurface();
        mControllerDiagramLayer->UpdateSurface();
        mButtonLayoutLayer->UpdateSurface();
        mControllerDiagramLayer->UpdateMapping(VRSettings::values.vr_button_map);

        UpdatePassthrough(appState.mIsPassthroughOn);
        uint32_t                        layerCount = 0;
        std::vector<XrCompositionLayer> layers(gOpenXr->mMaxLayerCount, XrCompositionLayer{});
        XrEnvironmentBlendMode environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        {

            // First, add the passthrough layer.
            if (mPassthroughLayer != nullptr) {

                XrCompositionLayerPassthroughFB passthroughLayer = {};
                mPassthroughLayer->Frame(passthroughLayer);
                layers[layerCount++].mPassthrough = passthroughLayer;
            } else if (mAlphaBlendPassthroughLayer != nullptr) {
                XrCompositionLayerProjection passthroughLayer = {};
                if (mAlphaBlendPassthroughLayer->Frame(gOpenXr->mSession, gOpenXr->mLocalSpace,
                                                       frameState.predictedDisplayTime,
                                                       passthroughLayer)) {
                    layers[layerCount++].mProjection = passthroughLayer;
                    environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
                }
            }

            // Game surface (upper and lower panels) are in front of the passthrough layer.
            // FrameTopPanel also latches new game frames, so call it either way.
            const uint32_t layerCountBeforeTopPanel = layerCount;
            mGameSurfaceLayer->FrameTopPanel(gOpenXr->mLocalSpace, layers, layerCount,
                                             gOpenXr->headLocation.pose, isImmersiveModeEnabled,
                                             immersiveModeFactor);
            if (showGameList) {
                layerCount = layerCountBeforeTopPanel;
                mGameListLayer->Frame(gOpenXr->mLocalSpace, layers, layerCount);
            }

            if (showUIRibbon) { mRibbonLayer->Frame(gOpenXr->mLocalSpace, layers, layerCount); }
            // The controller diagram and the 3DS layout would stand in front of the
            // room: they're hidden while passthrough shows it.
            const bool isPassthroughShown =
                mPassthroughLayer != nullptr || mAlphaBlendPassthroughLayer != nullptr;
            if (VRSettings::values.vr_show_controller_diagram && !isPassthroughShown) {
                if (layerCount < layers.size()) {
                    mControllerDiagramLayer->Frame(gOpenXr->mLocalSpace, layers, layerCount);
                }
                if (layerCount < layers.size()) {
                    mButtonLayoutLayer->Frame(gOpenXr->mLocalSpace, layers, layerCount);
                }
            }
            const bool showLowerPanel = visibility.mShowLowerPanel;
            if (showLowerPanel) {
                mGameSurfaceLayer->FrameLowerPanel(gOpenXr->mLocalSpace, layers, layerCount,
                                                   immersiveModeFactor);
            }

            // If active, the keyboard layer is in front of the game surface.
            if (appState.mIsKeyboardActive) {
                mKeyboardLayer->Frame(gOpenXr->mLocalSpace, layers, layerCount);
            }

            // If visible, error messsage appears in front of all other panels.
            if (appState.mShouldShowErrorMessage) {
                mErrorMessageLayer->Frame(gOpenXr->mLocalSpace, layers, layerCount);
            }

            // Cursor visibility will depend on hit-test but will be in front
            // of all other panels. This is because precedence lines up with depth order.
            ControllerLayer::PointerRay pointerRay;
            HandleCursorLayer(jni, appState, showLowerPanel, showUIRibbon, showGameList, layers,
                              layerCount, pointerRay);

            // Controllers and pointer ray (quad layers, placed by depth among the panels).
            if (mControllerLayer != nullptr) {
                if (!VRSettings::values.vr_show_pointer_ray) {
                    pointerRay.mIsVisible = false;
                }
                mControllerLayer->Frame(gOpenXr->mSession, gOpenXr->mLocalSpace,
                                        frameState.predictedDisplayTime, mInputStateFrame,
                                        VRSettings::values.vr_show_controllers, pointerRay, layers,
                                        layerCount);
            }
        }

        std::vector<const XrCompositionLayerBaseHeader*> layerHeaders;
        for (uint32_t i = 0; i < layerCount; i++) {
            layerHeaders.push_back((const XrCompositionLayerBaseHeader*)&layers[i]);
        }

        ////////////////////////////////
        // XrEndFrame()
        ////////////////////////////////

        const XrFrameEndInfo endFrameInfo = {XR_TYPE_FRAME_END_INFO,
                                             nullptr,
                                             frameState.predictedDisplayTime,
                                             environmentBlendMode,
                                             static_cast<uint32_t>(layerHeaders.size()),
                                             layerHeaders.data()};
        OXR(xrEndFrame(gOpenXr->mSession, &endFrameInfo));

        TrackVrFramePacing(frameState, frameLoopStart, waitFrameEnd,
                           std::chrono::steady_clock::now());
    }

    // Detects VR frames that missed the compositor deadline. The gap between
    // successive xrEndFrame completions also covers the event/input/JNI work
    // done in MainLoop before Frame(), so stalls anywhere on this thread are
    // caught. xrWaitFrame blocking is normal pacing; a long *gap* with a short
    // wait means this thread (or the JNI calls it makes) was slow, while a
    // long wait means the runtime throttled us (e.g. GPU running behind).
    void TrackVrFramePacing(const XrFrameState&                         frameState,
                            const std::chrono::steady_clock::time_point frameLoopStart,
                            const std::chrono::steady_clock::time_point waitFrameEnd,
                            const std::chrono::steady_clock::time_point frameEnd) {
        using FloatMs = std::chrono::duration<double, std::milli>;
        auto&      telemetry = Common::StallTelemetry::Get();
        const auto ToUs      = [](const auto duration) -> uint64_t {
            return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        };

        const auto waitDuration = waitFrameEnd - frameLoopStart;
        Common::UpdateMax(telemetry.vr_worst_wait_frame_us, ToUs(waitDuration));

        const bool hasPreviousFrame =
            mLastFrameEnd.time_since_epoch().count() != 0 && mFrameIndex > 1;
        if (hasPreviousFrame && frameState.predictedDisplayPeriod > 0) {
            const auto   gap      = frameEnd - mLastFrameEnd;
            const double gapMs    = FloatMs(gap).count();
            const double periodMs = static_cast<double>(frameState.predictedDisplayPeriod) / 1e6;
            // Gaps over a second are session-lifecycle discontinuities
            // (pause/resume), not stalls; don't let them poison the stats.
            if (gapMs < 1000.0) {
                Common::UpdateMax(telemetry.vr_worst_frame_gap_us, ToUs(gap));
                if (gapMs > periodMs * 1.5) {
                    telemetry.vr_missed_frame_count.fetch_add(1, std::memory_order_relaxed);
                    if (frameEnd - mLastVrStallLog > std::chrono::seconds(1)) {
                        mLastVrStallLog = frameEnd;
                        ALOGW("[STALL][VR] frame {}: {:.1f} ms between frames (display period "
                              "{:.1f} ms): wait={:.1f} ms render+submit={:.1f} ms",
                              mFrameIndex, gapMs, periodMs, FloatMs(waitDuration).count(),
                              FloatMs(frameEnd - waitFrameEnd).count());
                    }
                }
            }
        }
        mLastFrameEnd = frameEnd;
    }

    struct PanelVisibility {
        bool mShowGameList = false;
        bool mShowUIRibbon = false;
        bool mShowLowerPanel = false;
    };

    // Which panels show, from the app state. The game list replaces the game
    // screens and menu bar while it's shown; the menu bar shows when toggled on (or
    // in the positional menu), with the lower panel in its main menu.
    static PanelVisibility GetPanelVisibility(const AppState& appState) {
        PanelVisibility visibility;
        visibility.mShowGameList = appState.mShouldShowGameList;
        visibility.mShowUIRibbon = !visibility.mShowGameList &&
                                   (appState.mLowerMenuType == LowerMenuType::POSITIONAL_MENU ||
                                    appState.mIsLowerMenuToggledOn);
        visibility.mShowLowerPanel = visibility.mShowUIRibbon &&
                                     appState.mLowerMenuType == LowerMenuType::MAIN_MENU;
        return visibility;
    }

    // Whether a controller at this aim pose points at a panel it can click on.
    // `includeGameScreens`: whether the game screens (the touchscreen, and the top
    // screen in the positional menu) count, or only the menus.
    bool IsPointingAtPanel(const XrPosef& aimPose, const AppState& appState,
                           const bool includeGameScreens) const {
        const PanelVisibility visibility = GetPanelVisibility(appState);
        const XrVector3f start = XrMath::Posef::Transform(aimPose, XrVector3f{0, 0, 0});
        const XrVector3f end = XrMath::Posef::Transform(aimPose, XrVector3f{0, 0, -3.5f});
        XrVector2f pos2d;
        XrPosef pose3d;
        return (appState.mShouldShowErrorMessage && mErrorMessageLayer->IsRayOnPanel(start, end)) ||
               (visibility.mShowGameList && mGameListLayer->IsRayOnPanel(start, end)) ||
               (appState.mIsKeyboardActive && mKeyboardLayer->IsRayOnPanel(start, end)) ||
               (includeGameScreens && visibility.mShowLowerPanel &&
                mGameSurfaceLayer->GetRayIntersectionWithPanel(start, end, pos2d, pose3d)) ||
               (visibility.mShowUIRibbon && mRibbonLayer->IsRayOnPanel(start, end)) ||
               (includeGameScreens && !visibility.mShowGameList &&
                appState.mLowerMenuType == LowerMenuType::POSITIONAL_MENU &&
                mGameSurfaceLayer->GetRayIntersectionWithPanelTopPanel(start, end, pos2d, pose3d));
    }

    // When a trigger is pulled, decides what it does until it's released: while
    // its controller points at a panel, it clicks (and that controller becomes the
    // pointer); otherwise it's a game button, pressing the 3DS button it's mapped
    // to in the controller mapping (if any). While the settings wait for a button
    // to map, only the menus are clicked, so a trigger pulled at anything else
    // (the game screens included) is the button to map.
    void UpdateTriggerRoles(const AppState& appState) {
        for (int hand = 0; hand < InputStateFrame::NUM_CONTROLLERS; hand++) {
            const XrActionStateBoolean& trigger = mInputStateFrame.mIndexTriggerState[hand];
            if (trigger.currentState == XR_FALSE) {
                mIsTriggerButton[hand] = false;
                continue;
            }
            if (!trigger.changedSinceLastSync) {
                continue; // keeps the role chosen when it was pulled
            }
            const bool isPointingAtPanel =
                mInputStateFrame.mIsHandActive[hand] &&
                IsPointingAtPanel(mInputStateFrame.mHandPositions[hand].pose, appState,
                                  !appState.mIsCapturingControl);
            mIsTriggerButton[hand] = !isPointingAtPanel;
            ALOGI("{} trigger pulled: {}", hand == InputStateFrame::LEFT_CONTROLLER ? "Left" : "Right",
                  isPointingAtPanel ? "click" : "game button");
            if (isPointingAtPanel) {
                mInputStateFrame.mPreferredHand =
                    hand == InputStateFrame::LEFT_CONTROLLER ? InputStateFrame::LEFT_CONTROLLER
                                                             : InputStateFrame::RIGHT_CONTROLLER;
            }
        }
    }

    void HandleInput(JNIEnv* jni, const InputStateFrame& inputState, AppState& newState) const {
        assert(gOpenXr != nullptr);

        // User stall marker: clicking a thumbstick that isn't mapped to a game
        // button stamps a marker in the log for the user to flag "I just saw a
        // stall/artifact here".
        for (const auto hand :
             {InputStateFrame::LEFT_CONTROLLER, InputStateFrame::RIGHT_CONTROLLER}) {
            const auto control = hand == InputStateFrame::LEFT_CONTROLLER
                                     ? VRSettings::VrControl::LEFT_STICK_CLICK
                                     : VRSettings::VrControl::RIGHT_STICK_CLICK;
            const auto& clickState = inputState.mThumbStickClickState[hand];
            if (VRSettings::values.vr_button_map[static_cast<size_t>(control)] == 0 &&
                clickState.changedSinceLastSync && clickState.currentState == XR_TRUE) {
                LogUserStallMarker(mFrameIndex);
                break;
            }
        }

        // Forward the controls to the emulated gamepad, each as the 3DS button it
        // is mapped to in the settings. A button is held while any control mapped
        // to it is (e.g. L on both the left bumper and the left grip).
        {
            using VRSettings::VrControl;
            std::array<const XrActionStateBoolean*, VRSettings::kNumVrControls> controls{};
            controls[static_cast<size_t>(VrControl::A)] = &inputState.mAButtonState;
            controls[static_cast<size_t>(VrControl::B)] = &inputState.mBButtonState;
            controls[static_cast<size_t>(VrControl::X)] = &inputState.mXButtonState;
            controls[static_cast<size_t>(VrControl::Y)] = &inputState.mYButtonState;
            controls[static_cast<size_t>(VrControl::DPAD_UP)] = &inputState.mDpadUpState;
            controls[static_cast<size_t>(VrControl::DPAD_DOWN)] = &inputState.mDpadDownState;
            controls[static_cast<size_t>(VrControl::DPAD_LEFT)] = &inputState.mDpadLeftState;
            controls[static_cast<size_t>(VrControl::DPAD_RIGHT)] = &inputState.mDpadRightState;
            controls[static_cast<size_t>(VrControl::LEFT_BUMPER)] =
                &inputState.mBumperState[InputStateFrame::LEFT_CONTROLLER];
            controls[static_cast<size_t>(VrControl::RIGHT_BUMPER)] =
                &inputState.mBumperState[InputStateFrame::RIGHT_CONTROLLER];
            controls[static_cast<size_t>(VrControl::LEFT_GRIP)] =
                &inputState.mSqueezeTriggerState[InputStateFrame::LEFT_CONTROLLER];
            controls[static_cast<size_t>(VrControl::RIGHT_GRIP)] =
                &inputState.mSqueezeTriggerState[InputStateFrame::RIGHT_CONTROLLER];
            controls[static_cast<size_t>(VrControl::LEFT_STICK_CLICK)] =
                &inputState.mThumbStickClickState[InputStateFrame::LEFT_CONTROLLER];
            controls[static_cast<size_t>(VrControl::RIGHT_STICK_CLICK)] =
                &inputState.mThumbStickClickState[InputStateFrame::RIGHT_CONTROLLER];
            // Triggers count as buttons only when pulled away from the panels.
            XrActionStateBoolean triggersAsButtons[InputStateFrame::NUM_CONTROLLERS];
            for (int hand = 0; hand < InputStateFrame::NUM_CONTROLLERS; hand++) {
                triggersAsButtons[hand] = inputState.mIndexTriggerState[hand];
                if (!mIsTriggerButton[hand]) {
                    triggersAsButtons[hand].currentState = XR_FALSE;
                    triggersAsButtons[hand].changedSinceLastSync = XR_FALSE;
                }
            }
            controls[static_cast<size_t>(VrControl::LEFT_TRIGGER)] =
                &triggersAsButtons[InputStateFrame::LEFT_CONTROLLER];
            controls[static_cast<size_t>(VrControl::RIGHT_TRIGGER)] =
                &triggersAsButtons[InputStateFrame::RIGHT_CONTROLLER];
            controls[static_cast<size_t>(VrControl::LEFT_VIEW)] = &inputState.mLeftMenuButtonState;
            controls[static_cast<size_t>(VrControl::RIGHT_MENU)] = &inputState.mRightMenuButtonState;

            // While the settings wait for a button to map, report presses to them
            // instead of the game (whose buttons are released meanwhile).
            if (newState.mIsCapturingControl) {
                for (size_t i = 0; i < VRSettings::kNumVrControls; i++) {
                    if (controls[i]->changedSinceLastSync && controls[i]->currentState == XR_TRUE) {
                        ALOGI("Controller mapping: {} pressed",
                              VRSettings::kVrControlMappings[i].key);
                        jni->CallVoidMethod(mActivityObject, mOnVrControlPressedMethodID,
                                            static_cast<jint>(i));
                        break;
                    }
                }
            }

#if !defined(USE_INGAME_MENU)
            // The menu bar: toggled by its button, if any. Without one, it stays
            // shown, as it holds the settings (and the mapping).
            bool hasMenuBarButton = false;
            for (size_t i = 0; i < VRSettings::kNumVrControls; i++) {
                if (VRSettings::values.vr_button_map[i] != VRSettings::kVrActionToggleMenuBar) {
                    continue;
                }
                hasMenuBarButton = true;
                if (!newState.mIsCapturingControl && controls[i]->changedSinceLastSync &&
                    controls[i]->currentState == XR_TRUE) {
                    newState.mIsLowerMenuToggledOn = !newState.mIsLowerMenuToggledOn;
                }
            }
            if (!hasMenuBarButton) {
                newState.mIsLowerMenuToggledOn = true;
            }
#endif

            std::map<int32_t, bool> isButtonHeld;
            for (const auto& [button, wasHeld] : mForwardedButtonStates) {
                isButtonHeld[button] = false;
            }
            for (size_t i = 0; i < VRSettings::kNumVrControls; i++) {
                const int32_t button = VRSettings::values.vr_button_map[i];
                if (button == 0 || button == VRSettings::kVrActionToggleMenuBar ||
                    newState.mIsCapturingControl) {
                    continue;
                }
                isButtonHeld[button] |= controls[i]->currentState == XR_TRUE;
            }
            // Held buttons are sent every frame, released ones once.
            for (const auto& [button, isHeld] : isButtonHeld) {
                bool& wasHeld = mForwardedButtonStates[button];
                if (isHeld || wasHeld) {
                    ForwardButtonStateChangeToCitra(jni, mActivityObject, mForwardVRInputMethodID,
                                                    button, isHeld ? XR_TRUE : XR_FALSE);
                }
                wasHeld = isHeld;
            }
        }

        {
            // Right is circlepad, left is leftstick/circlepad, dpad is whatever
            // has thumbstick pressed.
            const auto leftStickHand = InputStateFrame::LEFT_CONTROLLER;
            const auto cStickHand    = InputStateFrame::RIGHT_CONTROLLER;

            const auto& leftThumbrestTouchState =
                inputState.mThumbrestTouchState[InputStateFrame::LEFT_CONTROLLER];
            const auto& rightThumbrestTouchState =
                inputState.mThumbrestTouchState[InputStateFrame::RIGHT_CONTROLLER];
            const int dpadHand =
                leftThumbrestTouchState.currentState    ? InputStateFrame::RIGHT_CONTROLLER
                : rightThumbrestTouchState.currentState ? InputStateFrame::LEFT_CONTROLLER
                                                        : InputStateFrame::NUM_CONTROLLERS;

            {
                static constexpr float kThumbStickDirectionThreshold = 0.5f;
                // Doing it this way helps ensure we don't leave the dpad
                // pressed if the thumbstick is released while still
                // pointing in the same direction.
                // I hope this is right, I didn't test it very much.
                static bool wasDpadLeft  = false;
                static bool wasDpadRight = false;
                static bool wasDpadUp    = false;
                static bool wasDpadDown  = false;

                const bool  hasDpad = dpadHand != InputStateFrame::NUM_CONTROLLERS;
                const auto& dpadThumbstickState =
                    inputState.mThumbStickState[hasDpad ? dpadHand : leftStickHand];

                if (hasDpad && dpadThumbstickState.currentState.y > kThumbStickDirectionThreshold) {
                    jni->CallVoidMethod(mActivityObject, mForwardVRInputMethodID, 19 /* DPAD_UP */,
                                        1);
                    wasDpadUp = true;
                } else if (wasDpadUp) {
                    jni->CallVoidMethod(mActivityObject, mForwardVRInputMethodID, 19 /* DPAD_UP */,
                                        0);
                    wasDpadUp = false;
                }
                if (hasDpad &&
                    dpadThumbstickState.currentState.y < -kThumbStickDirectionThreshold) {
                    jni->CallVoidMethod(mActivityObject, mForwardVRInputMethodID,
                                        20 /* DPAD_DOWN */, 1);
                    wasDpadDown = true;
                } else if (wasDpadDown) {
                    jni->CallVoidMethod(mActivityObject, mForwardVRInputMethodID,
                                        20 /* DPAD_DOWN */, 0);
                    wasDpadDown = false;
                }
                if (hasDpad &&
                    dpadThumbstickState.currentState.x < -kThumbStickDirectionThreshold) {
                    jni->CallVoidMethod(mActivityObject, mForwardVRInputMethodID,
                                        21 /* DPAD_LEFT */, 1);
                    wasDpadLeft = true;
                } else if (wasDpadLeft) {
                    jni->CallVoidMethod(mActivityObject, mForwardVRInputMethodID,
                                        21 /* DPAD_LEFT */, 0);
                    wasDpadLeft = false;
                }

                if (hasDpad && dpadThumbstickState.currentState.x > kThumbStickDirectionThreshold) {
                    jni->CallVoidMethod(mActivityObject, mForwardVRInputMethodID,
                                        22 /* DPAD_RIGHT */, 1);
                    wasDpadRight = true;
                } else if (wasDpadRight) {
                    jni->CallVoidMethod(mActivityObject, mForwardVRInputMethodID,
                                        22 /* DPAD_RIGHT */, 0);
                    wasDpadRight = false;
                }
            }

            // While the cursor hovers the settings menu, the pointing hand's
            // thumbstick scrolls the menu (see HandleCursorLayer); forward
            // zeroes to the game for that stick so scrolling doesn't also
            // move the game's camera/character.
            if (dpadHand != cStickHand) {
                const auto cStickThumbstickState = inputState.mThumbStickState[cStickHand];
                const bool suppress = sIsCursorHoveringSettingsMenu &&
                                      cStickHand == inputState.mPreferredHand;
                if (cStickThumbstickState.currentState.y != 0 ||
                    cStickThumbstickState.currentState.x != 0 ||
                    cStickThumbstickState.changedSinceLastSync) {
                    jni->CallVoidMethod(mActivityObject, mForwardVRJoystickMethodID,
                                        suppress ? 0.0f : cStickThumbstickState.currentState.x,
                                        suppress ? 0.0f : cStickThumbstickState.currentState.y, 0);
                }
            }
            if (dpadHand != leftStickHand) {
                const auto leftStickThumbstickState = inputState.mThumbStickState[leftStickHand];
                const bool suppress = sIsCursorHoveringSettingsMenu &&
                                      leftStickHand == inputState.mPreferredHand;
                if (leftStickThumbstickState.currentState.y != 0 ||
                    leftStickThumbstickState.currentState.x != 0 ||
                    leftStickThumbstickState.changedSinceLastSync) {
                    jni->CallVoidMethod(mActivityObject, mForwardVRJoystickMethodID,
                                        suppress ? 0.0f : leftStickThumbstickState.currentState.x,
                                        suppress ? 0.0f : leftStickThumbstickState.currentState.y,
                                        1);
                }
            }
        }

    }

    /** Handle the cursor and any hand-tracked/layer-dependent input
     *  interactions.
     **/
    void HandleCursorLayer(JNIEnv* jni, const AppState& appState, const bool showLowerPanel,
                           const bool showUIRibbon, const bool showGameList,
                           std::vector<XrCompositionLayer>& layers, uint32_t& layerCount,
                           ControllerLayer::PointerRay& pointerRay) const {

        bool                    shouldRenderCursor = false;
        XrPosef                 cursorPose3d       = XrMath::Posef::Identity();
        XrVector2f              cursorPos2d        = {0, 0};
        float                   scaleFactor        = 0.01f;
        // Re-derived below when the ribbon is hit-tested; cleared here so it
        // can't go stale on frames where the ribbon isn't tested at all.
        sIsCursorHoveringSettingsMenu              = false;
        CursorLayer::CursorType cursorType =
            appState.mLowerMenuType == LowerMenuType::POSITIONAL_MENU
                ? CursorLayer::CursorType::CURSOR_TYPE_POSITIONAL_MENU
                : CursorLayer::CursorType::CURSOR_TYPE_NORMAL;

        [[maybe_unused]] const auto nonPreferredController =
            mInputStateFrame.mPreferredHand == InputStateFrame::LEFT_CONTROLLER
                ? InputStateFrame::RIGHT_CONTROLLER
                : InputStateFrame::LEFT_CONTROLLER;
        // assert that we do not choose an inactive controller
        // unless neither controller is available.
        assert(mInputStateFrame.mIsHandActive[mInputStateFrame.mPreferredHand] ||
               !mInputStateFrame.mIsHandActive[nonPreferredController]);
        {
            const bool isPreferredControllerActive =
                mInputStateFrame.mIsHandActive[mInputStateFrame.mPreferredHand];

            static bool sIsLowerPanelBeingPositioned = false;

            sIsLowerPanelBeingPositioned &=
                appState.mLowerMenuType == LowerMenuType::POSITIONAL_MENU &&
                isPreferredControllerActive;
            if (isPreferredControllerActive) {
                const XrPosef pose =
                    mInputStateFrame.mHandPositions[mInputStateFrame.mPreferredHand].pose;
                // A trigger pulled while pointing elsewhere is a game button, not a click.
                static const XrActionStateBoolean kReleasedTrigger = {
                    XR_TYPE_ACTION_STATE_BOOLEAN, nullptr, XR_FALSE, XR_FALSE, 0, XR_TRUE};
                const auto& triggerState =
                    mIsTriggerButton[mInputStateFrame.mPreferredHand]
                        ? kReleasedTrigger
                        : mInputStateFrame.mIndexTriggerState[mInputStateFrame.mPreferredHand];

                const XrVector3f start = XrMath::Posef::Transform(
                    mInputStateFrame.mHandPositions[mInputStateFrame.mPreferredHand].pose,
                    XrVector3f{0, 0, 0});
                const XrVector3f end = XrMath::Posef::Transform(
                    mInputStateFrame.mHandPositions[mInputStateFrame.mPreferredHand].pose,
                    XrVector3f{0, 0, -3.5f});

                sIsLowerPanelBeingPositioned &= triggerState.currentState > 0;

                // Hit-test panels in order of priority (and known depth)

                // 1. Error message layer
                if (appState.mShouldShowErrorMessage) {
                    shouldRenderCursor = mErrorMessageLayer->GetRayIntersectionWithPanel(
                        start, end, cursorPos2d, cursorPose3d);
                    if (triggerState.changedSinceLastSync) {
                        mErrorMessageLayer->SendClickToUI(cursorPos2d, triggerState.currentState);
                    }
                }

                // 2. Game list (replaces the game panels while shown)
                if (!shouldRenderCursor && showGameList) {
                    shouldRenderCursor = mGameListLayer->GetRayIntersectionWithPanel(
                        start, end, cursorPos2d, cursorPose3d);
                    // Same press/drag/cancel handling as the menu bar, so the list
                    // can be dragged to scroll.
                    static bool sIsGameListPressed = false;
                    if (shouldRenderCursor) {
                        if (triggerState.changedSinceLastSync) {
                            mGameListLayer->SendClickToUI(cursorPos2d, triggerState.currentState);
                            sIsGameListPressed = triggerState.currentState == 1;
                        } else if (triggerState.currentState == 1 && sIsGameListPressed) {
                            mGameListLayer->SendClickToUI(cursorPos2d, 2 /* move */);
                        } else if (triggerState.currentState == 0) {
                            sIsGameListPressed = false;
                        }
                        // The thumbstick scrolls the list under the cursor.
                        const XrActionStateVector2f& thumbstickState =
                            mInputStateFrame.mThumbStickState[mInputStateFrame.mPreferredHand];
                        static constexpr float kScrollDeadZone    = 0.3f;
                        static constexpr float kScrollSpeedFactor = 0.5f;
                        if (std::abs(thumbstickState.currentState.y) > kScrollDeadZone) {
                            mGameListLayer->SendScrollToUI(
                                cursorPos2d,
                                {0.0f, thumbstickState.currentState.y * kScrollSpeedFactor});
                        }
                    } else if (sIsGameListPressed) {
                        mGameListLayer->SendClickToUI(cursorPos2d, 3 /* cancel */);
                        sIsGameListPressed = false;
                    }
                }

                // 3. Keyboard layer
                if (!shouldRenderCursor && appState.mIsKeyboardActive) {
                    shouldRenderCursor = mKeyboardLayer->GetRayIntersectionWithPanel(
                        start, end, cursorPos2d, cursorPose3d);
                    if (triggerState.changedSinceLastSync) {
                        mKeyboardLayer->SendClickToUI(cursorPos2d, triggerState.currentState);
                    }
                }
                // No dialogs/popups that should impede normal cursor interaction with
                // applicable panels

                // 4. Lower panel
                if (!shouldRenderCursor && showLowerPanel) {
                    shouldRenderCursor = mGameSurfaceLayer->GetRayIntersectionWithPanel(
                        start, end, cursorPos2d, cursorPose3d);
                    if (appState.mLowerMenuType != LowerMenuType::POSITIONAL_MENU) {
                        SendTriggerStateToWindow(jni, mActivityObject, mSendClickToWindowMethodID,
                                                 triggerState, cursorPos2d);
                    }
                }

                // 5. Ribbon layer
                if (!shouldRenderCursor && showUIRibbon) {
                    shouldRenderCursor = mRibbonLayer->GetRayIntersectionWithPanel(
                        start, end, cursorPos2d, cursorPose3d);
                    // Tracks whether a press started on the panel, so drags
                    // stream move events and a press that slides off the
                    // panel is cancelled rather than left stuck.
                    static bool sIsRibbonPressed = false;
                    if (shouldRenderCursor) {
                        if (triggerState.changedSinceLastSync) {
                            mRibbonLayer->SendClickToUI(cursorPos2d, triggerState.currentState);
                            sIsRibbonPressed = triggerState.currentState == 1;
                        } else if (triggerState.currentState == 1 && sIsRibbonPressed) {
                            // Move event while the trigger is held, so drag
                            // gestures (e.g. scrolling the settings list) work.
                            mRibbonLayer->SendClickToUI(cursorPos2d, 2);
                        } else if (triggerState.currentState == 0) {
                            // Release happened while off-panel; nothing to
                            // cancel anymore.
                            sIsRibbonPressed = false;
                        }
                    } else if (sIsRibbonPressed) {
                        mRibbonLayer->SendClickToUI(cursorPos2d, 3 /* cancel */);
                        sIsRibbonPressed = false;
                    }
                    // If trigger is pressed, thumbstick controls
                    // the depth
                    const XrActionStateVector2f& thumbstickState =
                        mInputStateFrame.mThumbStickState[mInputStateFrame.mPreferredHand];
                    static constexpr float kThumbStickDirectionThreshold = 0.5f;
                    const bool             hasThumbstickMotion =
                        std::abs(thumbstickState.currentState.y) > kThumbStickDirectionThreshold;

                    // In the settings menu, the thumbstick scrolls the
                    // content under the cursor (settings list or an open
                    // dialog).
                    sIsCursorHoveringSettingsMenu =
                        shouldRenderCursor &&
                        appState.mLowerMenuType == LowerMenuType::SETTINGS_MENU;
                    if (sIsCursorHoveringSettingsMenu) {
                        static constexpr float kScrollDeadZone = 0.3f;
                        // Wheel notches per frame; scales thumbstick
                        // deflection to a comfortable scroll speed.
                        static constexpr float kScrollSpeedFactor = 0.5f;
                        if (std::abs(thumbstickState.currentState.y) > kScrollDeadZone) {
                            mRibbonLayer->SendScrollToUI(
                                cursorPos2d,
                                {0.0f, thumbstickState.currentState.y * kScrollSpeedFactor});
                        }
                    }

                    if (appState.mLowerMenuType == LowerMenuType::POSITIONAL_MENU &&
                        (sIsLowerPanelBeingPositioned ||
                         (shouldRenderCursor && mRibbonLayer->IsMenuBackgroundSelected())) &&
                        (triggerState.currentState || hasThumbstickMotion)) {
                        if (hasThumbstickMotion) {
                            mRibbonLayer->SetPanelFromThumbstick(thumbstickState.currentState.y);
                        } else {
                            mRibbonLayer->SetPanelFromController(
                                {appState.mIsHorizontalAxisLocked ? 0.0f : cursorPose3d.position.x,
                                 cursorPose3d.position.y, cursorPose3d.position.z});
                        }

                        mGameSurfaceLayer->SetLowerPanelWithPose(mRibbonLayer->GetPose());
                        sIsLowerPanelBeingPositioned = true;
                    }
                }

                // 6. Top panel (only if positional menu is active)
                if (!shouldRenderCursor && !showGameList &&
                    appState.mLowerMenuType == LowerMenuType::POSITIONAL_MENU) {
                    shouldRenderCursor = mGameSurfaceLayer->GetRayIntersectionWithPanelTopPanel(
                        start, end, cursorPos2d, cursorPose3d);
                    // If top panel is hit, trigger controls the
                    // position/rotation
                    if (shouldRenderCursor && triggerState.currentState) {
                        // null out X component -- screen should stay
                        // center
                        mGameSurfaceLayer->SetTopPanelFromController(XrVector3f{
                            appState.mIsHorizontalAxisLocked ? 0.0f : cursorPose3d.position.x,
                            cursorPose3d.position.y, cursorPose3d.position.z});
                        // If trigger is pressed, thumbstick controls
                        // the depth
                        const XrActionStateVector2f& thumbstickState =
                            mInputStateFrame.mThumbStickState[mInputStateFrame.mPreferredHand];

                        static constexpr float kThumbStickDirectionThreshold = 0.5f;
                        if (std::abs(thumbstickState.currentState.y) >
                            kThumbStickDirectionThreshold) {
                            mGameSurfaceLayer->SetTopPanelFromThumbstick(
                                thumbstickState.currentState.y);
                        }
                    }
                }

                // Add a scale factor so the cursor doesn't scale as
                // quickly as the panel(s) with distance. This may be
                // mildly  unsettling, but it helps to ensure the cursor
                // isn't invisisble at the furthest distance.
                // It's just eyeballed -- no fantastic formula.
                {
                    const float distance =
                        XrMath::Vector3f::Length(pose.position - cursorPose3d.position);
                    scaleFactor = 0.01f + 0.003f * distance;
                }
            }
            if (sIsLowerPanelBeingPositioned) { shouldRenderCursor = true; }
        }

        // The pointer ray: from the preferred controller to the cursor, or a short
        // ray fading out when it points at nothing.
        if (mInputStateFrame.mIsHandActive[mInputStateFrame.mPreferredHand]) {
            constexpr float kFreeRayLengthInMeters = 0.6f;
            pointerRay.mIsVisible = true;
            pointerRay.mAimPose =
                mInputStateFrame.mHandPositions[mInputStateFrame.mPreferredHand].pose;
            pointerRay.mHitsPanel = shouldRenderCursor;
            pointerRay.mLength =
                shouldRenderCursor
                    ? XrMath::Vector3f::Length(cursorPose3d.position - pointerRay.mAimPose.position)
                    : kFreeRayLengthInMeters;
        }

        if (shouldRenderCursor) {
            ALOG_INPUT_VERBOSE("Cursor 2D coords: {} {}", cursorPos2d.x, cursorPos2d.y);
            // Draw the cursor just in front of the panel it points at (its pose has the
            // panel's orientation, whose +Z faces the viewer): SteamVR depth-tests
            // layers, and a cursor in the panel's plane can end up hidden behind it.
            constexpr float kCursorOffsetFromPanelInMeters = 0.005f;
            const XrPosef shownCursorPose = {
                cursorPose3d.orientation,
                XrMath::Posef::Transform(cursorPose3d,
                                         XrVector3f{0.0f, 0.0f, kCursorOffsetFromPanelInMeters})};
            XrCompositionLayerQuad quadLayer = {};
            mCursorLayer->Frame(gOpenXr->mLocalSpace, quadLayer, shownCursorPose, scaleFactor,
                                cursorType);
            layers[layerCount++].mQuad = quadLayer;
        }
        // FIXME Don't do this right now, because it's confusing to the
        // user. When you exit, the audio is muted until you doff/don,
        // and I don't love that it doesn't close with a single
        // action. So instead, we'll map this to start/select

#if defined(USE_INGAME_MENU)
        if (mInputStateFrame.mLeftMenuButtonState.changedSinceLastSync &&
            mInputStateFrame.mLeftMenuButtonState.currentState == XR_TRUE) {
            jni->CallVoidMethod(mActivityObject, mOpenSettingsMethodID);
        }
#endif
    }

    AppState HandleEvents(JNIEnv* jni) const {
        AppState newState = mLastAppState;
        OXRPollEvents(jni, newState);
        HandleMessageQueueEvents(jni, newState);
        return newState;
    }

    void HandleStateChanges(JNIEnv* jni, AppState& newState) const {
        {
            const bool shouldPauseEmulation =
                !newState.mHasFocus || newState.mShouldShowErrorMessage ||
                newState.mShouldShowGameList ||
                newState.mLowerMenuType == LowerMenuType::POSITIONAL_MENU;
            if (shouldPauseEmulation != mLastAppState.mIsEmulationPaused ||
                newState.mLowerMenuType != mLastAppState.mLowerMenuType) {
                ALOGI("State change: Emulation paused: {} -> {} (F={}, E={}, MP={})",
                      mLastAppState.mIsEmulationPaused, shouldPauseEmulation, newState.mHasFocus,
                      newState.mShouldShowErrorMessage,
                      newState.mLowerMenuType == LowerMenuType::POSITIONAL_MENU ? "P"
                      : newState.mLowerMenuType == LowerMenuType::MAIN_MENU     ? "M"
                      : newState.mLowerMenuType == LowerMenuType::STATS_MENU    ? "S"
                      : newState.mLowerMenuType == LowerMenuType::SETTINGS_MENU ? "Set"
                      : newState.mLowerMenuType == LowerMenuType::CONTROLS_MENU ? "C"
                                                                                : "U");
                if (shouldPauseEmulation) {
                    PauseEmulation(jni);
                    newState.mIsEmulationPaused = true;
                } else {
                    ResumeEmulation(jni);
                    newState.mIsEmulationPaused = false;
                }
            }
        }

        if (newState.mNumPanelResets > mLastAppState.mNumPanelResets) {
            mGameSurfaceLayer->ResetPanelPositions();
            mRibbonLayer->SetPanelWithPose(mGameSurfaceLayer->GetLowerPanelPose());
        }

        if (newState.mIsHorizontalAxisLocked && !mLastAppState.mIsHorizontalAxisLocked) {
            mGameSurfaceLayer->ResetPanelPositions();
            mRibbonLayer->SetPanelWithPose(mGameSurfaceLayer->GetLowerPanelPose());
        }
    }

    void OXRPollEvents(JNIEnv* jni, AppState& newAppState) const {
        XrEventDataBuffer eventDataBuffer = {};

        // Process all pending messages.
        for (;;) {
            XrEventDataBaseHeader* baseEventHeader = (XrEventDataBaseHeader*)(&eventDataBuffer);
            baseEventHeader->type                  = XR_TYPE_EVENT_DATA_BUFFER;
            baseEventHeader->next                  = NULL;
            XrResult r;
            OXR(r = xrPollEvent(gOpenXr->mInstance, &eventDataBuffer));
            if (r != XR_SUCCESS) { break; }

            switch (baseEventHeader->type) {
                case XR_TYPE_EVENT_DATA_EVENTS_LOST:
                    ALOGV("{}(): Received "
                          "XR_TYPE_EVENT_DATA_EVENTS_LOST "
                          "event",
                          __func__);
                    break;
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    ALOGV("{}(): Received "
                          "XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING event",
                          __func__);
                    break;
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    const XrEventDataSessionStateChanged* ssce =
                        (XrEventDataSessionStateChanged*)(baseEventHeader);
                    if (ssce != nullptr) {
                        ALOGV("{}(): Received "
                              "XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED",
                              __func__);
                        OXRHandleSessionStateChangedEvent(jni, newAppState, *ssce);
                    } else {
                        ALOGE("{}(): Received "
                              "XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: nullptr",
                              __func__);
                    }
                } break;
                case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                    ALOGV("{}(): Received "
                          "XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED event",
                          __func__);
                    break;
                case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT: {
                    const XrEventDataPerfSettingsEXT* pfs =
                        (XrEventDataPerfSettingsEXT*)(baseEventHeader);
                    // These events are the runtime reporting clock-level
                    // changes (e.g. thermal throttling), a common cause of
                    // otherwise-unexplained stutter -- log degradations at
                    // warning level so they are visible in release builds.
                    if (pfs->toLevel > pfs->fromLevel) {
                        ALOGW("XR perf settings: {} {} degraded {} -> {}",
                              XrPerfSettingsDomainToString(pfs->domain),
                              XrPerfSettingsSubDomainToString(pfs->subDomain),
                              XrPerfSettingsNotificationLevelToString(pfs->fromLevel),
                              XrPerfSettingsNotificationLevelToString(pfs->toLevel));
                    } else {
                        ALOGI("XR perf settings: {} {} recovered {} -> {}",
                              XrPerfSettingsDomainToString(pfs->domain),
                              XrPerfSettingsSubDomainToString(pfs->subDomain),
                              XrPerfSettingsNotificationLevelToString(pfs->fromLevel),
                              XrPerfSettingsNotificationLevelToString(pfs->toLevel));
                    }
                } break;
                case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                    ALOGV("{}(): Received "
                          "XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING "
                          "event",
                          __func__);
                    break;
                default:
                    ALOGV("{}(): Unknown event", __func__);
                    break;
            }
        }
    }

    void OXRHandleSessionStateChangedEvent(JNIEnv*                               jni,
                                           AppState&                             newAppState,
                                           const XrEventDataSessionStateChanged& newState) const {
        static XrSessionState lastState = XR_SESSION_STATE_UNKNOWN;
        if (newState.state != lastState) {
            ALOGV("{}(): Received XR_SESSION_STATE_CHANGED state {}->{} "
                  "session={} time={}",
                  __func__, XrSessionStateToString(lastState),
                  XrSessionStateToString(newState.state), (void*)newState.session, newState.time);
        }
        lastState = newState.state;
        switch (newState.state) {
            case XR_SESSION_STATE_FOCUSED:
                ALOGV("{}(): Received XR_SESSION_STATE_FOCUSED event", __func__);
                newAppState.mHasFocus = true;
                break;
            case XR_SESSION_STATE_VISIBLE:
                ALOGV("{}(): Received XR_SESSION_STATE_VISIBLE event", __func__);
                newAppState.mHasFocus = false;
                break;
            case XR_SESSION_STATE_READY:
            case XR_SESSION_STATE_STOPPING:
                OXRHandleSessionStateChanges(newState.state, newAppState);
                break;
            case XR_SESSION_STATE_EXITING:
                newAppState.mIsStopRequested = true;
                break;
            default:
                break;
        }
    }

    void OXRHandleSessionStateChanges(const XrSessionState state, AppState& newAppState) const {
        if (state == XR_SESSION_STATE_READY) {
            assert(mLastAppState.mIsXrSessionActive == false);

            XrSessionBeginInfo sbi           = {};
            sbi.type                         = XR_TYPE_SESSION_BEGIN_INFO;
            sbi.next                         = nullptr;
            sbi.primaryViewConfigurationType = gOpenXr->mViewportConfig.viewConfigurationType;

            XrResult result;
            OXR(result = xrBeginSession(gOpenXr->mSession, &sbi));

            newAppState.mIsXrSessionActive = (result == XR_SUCCESS);

            // Set session state once we have entered VR mode and have a valid
            // session object.
            if (newAppState.mIsXrSessionActive) {
                ALOGI("{}(): Entered XR_SESSION_STATE_READY", __func__);
                if (OpenXr::GetExtensions().mPerformanceSettings) {
                    PFN_xrPerfSettingsSetPerformanceLevelEXT pfnPerfSettingsSetPerformanceLevelEXT =
                        NULL;
                    OXR(xrGetInstanceProcAddr(
                        gOpenXr->mInstance, "xrPerfSettingsSetPerformanceLevelEXT",
                        (PFN_xrVoidFunction*)(&pfnPerfSettingsSetPerformanceLevelEXT)));

                    OXR(pfnPerfSettingsSetPerformanceLevelEXT(gOpenXr->mSession,
                                                              XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
                                                              VRSettings::values.cpu_level));
                    OXR(pfnPerfSettingsSetPerformanceLevelEXT(
                        gOpenXr->mSession, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, kGpuPerfLevel));
                    ALOGI("{}(): Set clock levels to CPU:{}, GPU:{}", __FUNCTION__,
                          VRSettings::values.cpu_level, kGpuPerfLevel);
                }

                if (OpenXr::GetExtensions().mAndroidThreadSettings) {
                    PFN_xrSetAndroidApplicationThreadKHR pfnSetAndroidApplicationThreadKHR = NULL;
                    OXR(xrGetInstanceProcAddr(
                        gOpenXr->mInstance, "xrSetAndroidApplicationThreadKHR",
                        (PFN_xrVoidFunction*)(&pfnSetAndroidApplicationThreadKHR)));

                    if (vr::gPriorityTid > 0) {
                        ALOGD("Setting prio tid from main {}", vr::gPriorityTid);
                        OXR(pfnSetAndroidApplicationThreadKHR(
                            gOpenXr->mSession, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR,
                            vr::gPriorityTid));
                    } else {
                        ALOGD("Not setting prio tid from main");
                    }
                    OXR(pfnSetAndroidApplicationThreadKHR(
                        gOpenXr->mSession, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, gettid()));
                }

                if (mGameSurfaceLayer) {
                    ALOGD("SetSurface");
                    mGameSurfaceLayer->SetSurface(mActivityObject);
                }
            }
        } else if (state == XR_SESSION_STATE_STOPPING) {
            assert(mLastAppState.mIsXrSessionActive);
            ALOGI("{}(): Entered XR_SESSION_STATE_STOPPING", __func__);
            OXR(xrEndSession(gOpenXr->mSession));
            newAppState.mIsXrSessionActive = false;
        }
    }

    void HandleMessageQueueEvents(JNIEnv* jni, AppState& newAppState) const {
        // Arbitrary limit to prevent the render thread from blocking too long
        // on a single frame. This may happen if the app is paused in an edge
        // case. We should try to avoid these cases as it will result in a
        // glitchy UX.
        constexpr size_t kMaxNumMessagesPerFrame = 20;

        size_t  numMessagesHandled = 0;
        Message message;
        while (numMessagesHandled < kMaxNumMessagesPerFrame) {
            if (!gMessageQueue.Poll(message)) { break; }
            numMessagesHandled++;

            switch (message.mType) {
                case Message::Type::SHOW_KEYBOARD: {
                    const bool shouldShowKeyboard = message.mPayload == 1;
                    if (shouldShowKeyboard != mLastAppState.mIsKeyboardActive) {
                        ALOGD("Keyboard status changed: {} -> {}", mLastAppState.mIsKeyboardActive,
                              shouldShowKeyboard);
                    }

                    ALOGD("Received SHOW_KEYBOARD message: {}, state change {} -> {}",
                          shouldShowKeyboard, mLastAppState.mIsKeyboardActive, shouldShowKeyboard);
                    newAppState.mIsKeyboardActive = shouldShowKeyboard;

                    break;
                }
                case Message::Type::SHOW_ERROR_MESSAGE: {
                    const bool shouldShowErrorMessage = message.mPayload == 1;
                    ALOGD("Received SHOW_ERROR_MESSAGE message: {}, state change {} -> {}",
                          shouldShowErrorMessage, mLastAppState.mShouldShowErrorMessage,
                          shouldShowErrorMessage);
                    newAppState.mShouldShowErrorMessage = shouldShowErrorMessage;
                    if (newAppState.mShouldShowErrorMessage && !newAppState.mIsEmulationPaused) {
                        ALOGD("Pausing emulation due to error message");
                    }
                    if (!newAppState.mShouldShowErrorMessage && newAppState.mIsEmulationPaused &&
                        newAppState.mHasFocus) {
                        ALOGD("Resuming emulation after error message");
                    }
                    break;
                }
                case Message::Type::SHOW_GAME_LIST: {
                    ALOGI("Received SHOW_GAME_LIST message: {}", message.mPayload);
                    newAppState.mShouldShowGameList = message.mPayload == 1;
                    break;
                }
                case Message::Type::SET_PASSTHROUGH: {
                    ALOGI("Received SET_PASSTHROUGH message: {}", message.mPayload);
                    newAppState.mIsPassthroughOn = message.mPayload == 1;
                    VRSettings::values.vr_environment = static_cast<int32_t>(
                        newAppState.mIsPassthroughOn ? VRSettings::VREnvironmentType::PASSTHROUGH
                                                     : VRSettings::VREnvironmentType::VOID);
                    break;
                }
                case Message::Type::CAPTURE_CONTROL: {
                    ALOGI("Received CAPTURE_CONTROL message: {}", message.mPayload);
                    newAppState.mIsCapturingControl = message.mPayload == 1;
                    break;
                }
                case Message::Type::EXIT_NEEDED: {
                    ALOGD("Received EXIT_NEEDED message");
                    newAppState.mIsStopRequested = true;
                    break;
                }
                case Message::Type::CHANGE_LOWER_MENU: {
                    newAppState.mLowerMenuType = static_cast<LowerMenuType>(message.mPayload);
                    ALOGD("Received CHANGE_LOWER_MENU message: {}, state change {} -> {}",
                          message.mPayload, mLastAppState.mLowerMenuType,
                          newAppState.mLowerMenuType);
                    break;
                }
                case Message::Type::CHANGE_LOCK_HORIZONTAL_AXIS: {
                    ALOGD("Received CHANGE_LOCK_HORIZONTAL_AXIS message: {}, state change {} -> {}",
                          message.mPayload, mLastAppState.mIsHorizontalAxisLocked,
                          message.mPayload == 1);
                    newAppState.mIsHorizontalAxisLocked = message.mPayload == 1;
                    break;
                }
                case Message::Type::RESET_PANEL_POSITIONS: {
                    ALOGD("Received RESET_PANEL_POSITIONS message");
                    newAppState.mNumPanelResets++;
                    break;
                }
                default:
                    ALOGE("Unknown message type: %d", message.mType);
                    break;
            }
        }
    }

    void PauseEmulation(JNIEnv* jni) const {
        if (!vr::gCitraReady) return;
        assert(jni != nullptr);
        jni->CallVoidMethod(mActivityObject, mPauseGameMethodID);
    }

    void ResumeEmulation(JNIEnv* jni) const {
        if (!vr::gCitraReady) return;
        assert(jni != nullptr);
        jni->CallVoidMethod(mActivityObject, mResumeGameMethodID);
    }

    void UpdateImmersiveModeIfNeeded(const float immersiveModeFactor) const {
        // This block is for testing which uinform offset is needed
        // for a given game to implement new super-immersive profiles if needed
        static bool increase = false;
        static int  uoffset  = -1;
        {
            if (VRSettings::values.vr_immersive_mode > 90) {
                if (mInputStateFrame.mThumbrestTouchState[InputStateFrame::RIGHT_CONTROLLER]
                        .currentState) {
                    if (increase) {
                        ++uoffset;
                        increase = false;
                    }

                    // There are 96 Vec4f; since we are applying 4 of them at a time we need to
                    // loop
                    //  after 92
                    if (uoffset > 92) { uoffset = 0; }
                } else {
                    increase = true;
                }
            }
        }

        if (!Core::System::GetInstance().IsPoweredOn() ||
            !Core::System::GetInstance().GPU().Renderer().Rasterizer()) {
            return;
        }

        XrVector4f transform[4] = {};
        XrMath::Quatf::ToRotationMatrix(gOpenXr->headLocation.pose.orientation, (float*)transform);

        // Calculate the inverse
        XrVector4f inv_transform[4];
        XrMath::Matrixf::ToInverse(transform, inv_transform);

        XrQuaternionf invertedOrientation =
            XrMath::Quatf::Inverted(gOpenXr->headLocation.pose.orientation);
        XrVector3f position =
            XrMath::Quatf::Rotate(invertedOrientation, gOpenXr->headLocation.pose.position);

        const float gamePosScaler =
            powf(10.f, VRSettings::values.vr_immersive_positional_game_scaler) *
            VRSettings::values.vr_factor_3d;

        inv_transform[3].x = -position.x * gamePosScaler;
        inv_transform[3].y = -position.y * gamePosScaler;
        inv_transform[3].z = -position.z * gamePosScaler;

        UpdateImmersiveMode(immersiveModeFactor, uoffset, gamePosScaler, inv_transform);
    }

    void UpdateImmersiveMode(const float immersiveModeFactor, const float uoffset,
                             const float gamePosScaler, const XrVector4f* inv_transform) const {
        Core::System::GetInstance().GPU().Renderer().Rasterizer()->SetVRData(
            VRSettings::values.vr_immersive_mode, immersiveModeFactor, uoffset, -gamePosScaler,
            (float*)inv_transform);
    }

    void DisableImmersiveMode() const {
        if (Core::System::GetInstance().IsPoweredOn() &&
            Core::System::GetInstance().GPU().Renderer().Rasterizer()) {
            Core::System::GetInstance().GPU().Renderer().Rasterizer()->SetVRData(
                1, 1.0f, -1, 0.f, (float*)XrMath::Matrixf::Identity);
        }
    }

    uint64_t    mFrameIndex = 0;
    std::thread mThread;
    jobject     mActivityObject;
    // Stall diagnostics (see TrackVrFramePacing).
    std::chrono::steady_clock::time_point mLastFrameEnd{};
    std::chrono::steady_clock::time_point mLastVrStallLog{};

    class AppState {
    public:
        LowerMenuType mLowerMenuType          = LowerMenuType::MAIN_MENU;
        int32_t       mNumPanelResets         = 0;
        bool          mIsHorizontalAxisLocked = true;
        bool          mIsLowerMenuToggledOn   = true;

        bool mIsKeyboardActive       = false;
        bool mShouldShowErrorMessage = false;
        bool mShouldShowGameList     = false;
        bool mIsEmulationPaused      = false;
        // The settings are waiting for a controller button to map (see HandleInput()).
        bool mIsCapturingControl     = false;
        // The room around the game (passthrough) rather than the void, if supported.
        bool mIsPassthroughOn        = false;

        bool mIsStopRequested   = false;
        bool mIsXrSessionActive = false;
        bool mHasFocus          = false;
    };

    // App state from previous frame.
    AppState mLastAppState;

    std::unique_ptr<CursorLayer> mCursorLayer;
    std::unique_ptr<ControllerDiagramLayer> mControllerDiagramLayer;
    std::unique_ptr<UILayer> mButtonLayoutLayer;
    std::unique_ptr<ControllerLayer> mControllerLayer;
    // The 3DS buttons (Android key codes) last sent to the emulated gamepad, and
    // whether they were held; see HandleInput().
    mutable std::map<int32_t, bool> mForwardedButtonStates;
    // Whether each trigger, while pulled, is a game button rather than a click; see
    // UpdateTriggerRoles().
    bool mIsTriggerButton[InputStateFrame::NUM_CONTROLLERS] = {false, false};
    std::unique_ptr<UILayer> mErrorMessageLayer;
    std::unique_ptr<GameSurfaceLayer> mGameSurfaceLayer;
    std::unique_ptr<PassthroughLayer> mPassthroughLayer;
    std::unique_ptr<AlphaBlendPassthroughLayer> mAlphaBlendPassthroughLayer;
    // The passthrough state last reported to the menu bar (see UpdatePassthrough()).
    bool mReportedPassthroughState = false;
    bool mHasReportedPassthrough = false;
    std::unique_ptr<UILayer> mKeyboardLayer;
    std::unique_ptr<UILayer> mGameListLayer;
    std::unique_ptr<RibbonLayer> mRibbonLayer;

    std::unique_ptr<InputStateStatic> mInputStateStatic;
    InputStateFrame                   mInputStateFrame;

    jmethodID mForwardVRInputMethodID    = nullptr;
    jmethodID mOnVrControlPressedMethodID = nullptr;
    jmethodID mForwardVRJoystickMethodID = nullptr;
    jmethodID mSendClickToWindowMethodID = nullptr;
    jmethodID mResumeGameMethodID        = nullptr;
    jmethodID mPauseGameMethodID         = nullptr;
    jmethodID mOpenSettingsMethodID      = nullptr;
};

//-----------------------------------------------------------------------------
// VRApp

class VRAppThread {
public:
    VRAppThread(JavaVM* jvm, JNIEnv* jni, jobject activityObject)
        : mVm(jvm)
        , mActivityObjectGlobalRef(jni->NewGlobalRef(activityObject)) {
        assert(jvm != nullptr);
        assert(activityObject != nullptr);
        mThread = std::thread([this]() { ThreadFn(); });
    }

    ~VRAppThread() {
        gMessageQueue.Post(Message(Message::Type::EXIT_NEEDED, 0));
        // Note: this is in most cases already going to be true by the time the
        // destructor is called, because it is set to true in onStop()
        ALOGI("Waiting for VRAppThread to join");
        if (mThread.joinable()) { mThread.join(); }
        ALOGI("VRAppThread joined");
    }

private:
    void ThreadFn() {
        assert(mVm != nullptr);
        ALOGI("VRAppThread: starting");
        JNIEnv* jni = nullptr;
        if (mVm->AttachCurrentThread(&jni, nullptr) != JNI_OK) {
            FAIL("%s(): Could not attach to mVm", __FUNCTION__);
        }
        // Gotta set this after the JNIEnv is attached, or else it'll be
        // overwritten
        prctl(PR_SET_NAME, (long)"CVR::Main", 0, 0, 0);

        ThreadFnJNI(jni);

        mVm->DetachCurrentThread();
        ALOGI("VRAppThread: exited");
    }

    // All operations assume that the JNIEnv is attached
    void ThreadFnJNI(JNIEnv* jni) {
        assert(jni != nullptr);
        assert(mActivityObjectGlobalRef != nullptr);
        if (gOpenXr == nullptr) {
            gOpenXr           = std::make_unique<OpenXr>();
            const int32_t ret = gOpenXr->Init(mVm, mActivityObjectGlobalRef);
            if (ret < 0) { FAIL("OpenXR::Init() failed: error code %d", ret); }
        }
        vr::gSession = gOpenXr->mSession;

        { std::make_unique<VRApp>(mActivityObjectGlobalRef)->MainLoop(jni); }

        ALOGI("::MainLoop() exited");

        gOpenXr->Shutdown();

        jni->DeleteGlobalRef(mActivityObjectGlobalRef);
        mActivityObjectGlobalRef = nullptr;
    }

    JavaVM*     mVm;
    jobject     mActivityObjectGlobalRef;
    std::thread mThread;
};

struct VRAppHandle {
    VRAppHandle(VRAppThread* _p)
        : p(_p) {}
    VRAppHandle(jlong _l)
        : l(_l) {}

    union {
        VRAppThread* p = nullptr;
        jlong        l;
    };
};

//-----------------------------------------------------------------------------
// JNI functions

extern "C" JNIEXPORT jlong JNICALL
Java_org_citra_citra_1emu_vr_VrActivity_nativeOnCreate(JNIEnv* env, jobject thiz) {
    // Log the creat start time, which will be used to calculate the total
    // time to first frame.
    gOnCreateStartTime = std::chrono::steady_clock::now();

    JavaVM* jvm;
    env->GetJavaVM(&jvm);
    auto ret = VRAppHandle(new VRAppThread(jvm, env, thiz)).l;
    ALOGI("nativeOnCreate {}", ret);
    return ret;
}
extern "C" JNIEXPORT void JNICALL
Java_org_citra_citra_1emu_vr_VrActivity_nativeOnDestroy(JNIEnv* env, jobject thiz, jlong handle) {
    ALOGI("nativeOnDestroy {}", static_cast<long>(handle));
    if (handle != 0) { delete VRAppHandle(handle).p; }
    VR::JNI::CleanupJNI(env);
}
extern "C" JNIEXPORT jint JNICALL
Java_org_citra_citra_1emu_vr_utils_VRUtils_getHMDType(JNIEnv* env, jclass clazz) {
    return static_cast<jint>(VRSettings::HmdTypeFromStr(VRSettings::GetHMDTypeStr()));
}
extern "C" JNIEXPORT jint JNICALL
Java_org_citra_citra_1emu_vr_utils_VRUtils_getDefaultResolutionFactor(JNIEnv* env, jclass clazz) {
    const VRSettings::HMDType hmdType = VRSettings::HmdTypeFromStr(VRSettings::GetHMDTypeStr());
    return GetDefaultGameResolutionFactorForHmd(hmdType);
}
extern "C" JNIEXPORT void JNICALL Java_org_citra_citra_1emu_vr_utils_VrMessageQueue_nativePost(
    JNIEnv* env, jobject thiz, jint message_type, jlong payload) {
    ALOGI("{}(): message_type: {}, payload: {}", __FUNCTION__, message_type, payload);
    gMessageQueue.Post(Message(static_cast<Message::Type>(message_type), payload));
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_org_citra_citra_1emu_vr_ui_VrRibbonLayer_nativeGetStatsOXR(JNIEnv* env, jobject thiz) {

    if (vr::gSession == XR_NULL_HANDLE || OpenXr::GetInstance() == XR_NULL_HANDLE) {
        return nullptr;
    }

    // The runtime's metrics, where it has XR_META_performance_metrics. Without
    // it, they are all NaN and the stall telemetry is still returned.
    static PFN_xrQueryPerformanceMetricsCounterMETA xrQueryPerformanceMetricsCounterMETA = nullptr;
    static bool sHasSetUpMetrics = false;
    if (!sHasSetUpMetrics && OpenXr::GetExtensions().mPerformanceMetrics) {
        sHasSetUpMetrics = true;
        PFN_xrSetPerformanceMetricsStateMETA xrSetPerformanceMetricsStateMETA = nullptr;
        xrGetInstanceProcAddr(OpenXr::GetInstance(), "xrSetPerformanceMetricsStateMETA",
                              (PFN_xrVoidFunction*)&xrSetPerformanceMetricsStateMETA);
        XrPerformanceMetricsStateMETA perfMetricsState{XR_TYPE_PERFORMANCE_METRICS_STATE_META};
        perfMetricsState.enabled = XR_TRUE;
        const XrResult stateResult =
            xrSetPerformanceMetricsStateMETA == nullptr
                ? XR_ERROR_FUNCTION_UNSUPPORTED
                : xrSetPerformanceMetricsStateMETA(vr::gSession, &perfMetricsState);
        const XrResult queryResult =
            xrGetInstanceProcAddr(OpenXr::GetInstance(), "xrQueryPerformanceMetricsCounterMETA",
                                  (PFN_xrVoidFunction*)&xrQueryPerformanceMetricsCounterMETA);
        ALOGI("Performance metrics: enabling {}, query function {}", static_cast<int>(stateResult),
              static_cast<int>(queryResult));
        if (stateResult != XR_SUCCESS || queryResult != XR_SUCCESS) {
            xrQueryPerformanceMetricsCounterMETA = nullptr;
        }
    }

    // A counter's value, or NaN where the runtime doesn't provide it (the
    // query fails or flags no valid value). Runtimes report a counter as a
    // float or as an integer (counterFlags says which), so both are read.
    // Each counter's outcome is logged once.
    const auto query = [&](const char* pathString) -> float {
        if (xrQueryPerformanceMetricsCounterMETA == nullptr) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        XrPath path = XR_NULL_PATH;
        XrPerformanceMetricsCounterMETA counter = {XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};
        XrResult result = xrStringToPath(OpenXr::GetInstance(), pathString, &path);
        if (result == XR_SUCCESS) {
            result = xrQueryPerformanceMetricsCounterMETA(vr::gSession, path, &counter);
        }
        float value = std::numeric_limits<float>::quiet_NaN();
        if (result == XR_SUCCESS &&
            (counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META)) {
            value = counter.floatValue;
        } else if (result == XR_SUCCESS &&
                   (counter.counterFlags &
                    XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META)) {
            value = static_cast<float>(counter.uintValue);
        }
        static std::set<std::string> sLoggedPaths;
        if (sLoggedPaths.insert(pathString).second) {
            ALOGI("Performance metric {}: result {}, flags {:#x}, value {}", pathString,
                  static_cast<int>(result), static_cast<uint64_t>(counter.counterFlags), value);
        }
        return value;
    };
    const float cpuUtilization = query("/perfmetrics_meta/device/cpu_utilization_average");
    const float gpuUtilization = query("/perfmetrics_meta/device/gpu_utilization");
    const float appCpuFrameTime = query("/perfmetrics_meta/app/cpu_frametime");
    const float appGpuFrameTime = query("/perfmetrics_meta/app/gpu_frametime");
    const float appVrLatency = query("/perfmetrics_meta/app/motion_to_photon_latency");
    const float compTears = query("/perfmetrics_meta/compositor/dropped_frame_count");
    const float compCpuFrameTime = query("/perfmetrics_meta/compositor/cpu_frametime");
    const float compGpuFrameTime = query("/perfmetrics_meta/compositor/gpu_frametime");

    // Stall telemetry (cumulative since process start; see
    // common/stall_telemetry.h). Appended after the runtime perf metrics.
    auto&         telemetry = Common::StallTelemetry::Get();
    const int64_t lastCompileUs =
        telemetry.last_shader_compile_steady_us.load(std::memory_order_relaxed);
    const jfloat secondsSinceLastCompile =
        lastCompileUs > 0 ? static_cast<jfloat>((Common::SteadyNowUs() - lastCompileUs) / 1e6)
                          : -1.0f;

    const jfloat metricsArray[17] = {
        // The runtime's metrics (NaN where it doesn't provide them).
        cpuUtilization,                            // Device CPU Utilization %
        gpuUtilization,                            // Device GPU Utilization %
        appCpuFrameTime,                           // App CPU Frametime (ms)
        appGpuFrameTime,                           // App GPU Frametime (ms)
        appVrLatency,                              // App VR Latency (ms)
        compCpuFrameTime,                          // Compositor CPU Frametime (ms)
        compGpuFrameTime,                          // Compositor GPU Frametime (ms)
        compTears,                                 // Compositor tear count
        static_cast<jfloat>(
            telemetry.emu_slow_frame_count.load(std::memory_order_relaxed)),  // Emu slow frames
        static_cast<jfloat>(
            telemetry.emu_stall_frame_count.load(std::memory_order_relaxed)), // Emu stall frames
        static_cast<jfloat>(telemetry.emu_worst_frame_time_us.load(std::memory_order_relaxed)) /
            1000.0f,                               // Emu worst frame (ms)
        static_cast<jfloat>(
            telemetry.shader_compile_count.load(std::memory_order_relaxed)),  // Shader compiles
        static_cast<jfloat>(telemetry.shader_compile_time_us.load(std::memory_order_relaxed)) /
            1000.0f,                               // Shader compile total (ms)
        secondsSinceLastCompile,                   // Secs since last compile (-1 = never)
        static_cast<jfloat>(
            telemetry.vr_missed_frame_count.load(std::memory_order_relaxed)), // VR missed frames
        static_cast<jfloat>(telemetry.vr_worst_frame_gap_us.load(std::memory_order_relaxed)) /
            1000.0f,                               // VR worst frame gap (ms)
        static_cast<jfloat>(telemetry.vr_worst_wait_frame_us.load(std::memory_order_relaxed)) /
            1000.0f,                               // VR worst wait (ms)
    };

    const jfloatArray result = env->NewFloatArray(17);
    env->SetFloatArrayRegion(result, 0, 17, metricsArray);

    return result;
}
