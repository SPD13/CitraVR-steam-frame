/***************************************************************************************************

Filename    :   UILayer.h

Content     :   Utility class for creating interactive Android UI windows that
                are displayed in the VR environment.

Authors     :   Amanda M. Watson
License     :   Licensed under GPLv3 or any later version.
                Refer to the license.txt file included.

***************************************************************************************************/

#include "UILayer.h"

#include "../vr_settings.h"

#include "../utils/JniUtils.h"
#include "../utils/LogUtils.h"
#include "../utils/SyspropUtils.h"
#include "../utils/XrMath.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <chrono>

#include <stdlib.h>

namespace {

/** Used to translate texture coordinates into the corresponding coordinates
 * on the virtual Window.
 */
class AndroidWindowBounds {
public:
    AndroidWindowBounds() = default;
    AndroidWindowBounds(const float widthInDp, const float heightInDp)
        : mLeftInDp(0.0f)
        , mRightInDp(widthInDp)
        , mTopInDp(0.0f)
        , mBottomInDp(heightInDp) {}

    float Width() const { return mRightInDp - mLeftInDp; }
    float Height() const { return mBottomInDp - mTopInDp; }

    // "DP" refers to display pixels, which are the same as Android's
    // "density-independent pixels" (dp).
    // Note
    float mLeftInDp   = 0;
    float mRightInDp  = 0;
    float mTopInDp    = 0;
    float mBottomInDp = 0;

    // given a 2D point in worldspace 'point2d', returns the transformed
    // coordinate in DP, written to 'result'
    void Transform(const XrVector2f& point2d, XrVector2f& result) const {
        const float left = mLeftInDp;
        const float top  = mTopInDp;

        const float width  = Width();
        const float height = Height();

        result.x = (point2d.x * width) + left + (width / 2.0);
        // Android has a flipped vertical axis from OpenXR
        result.y = ((1.0 - point2d.y) * height) + top - (height / 2.0);
    }
};

struct BoundsHandle {
    BoundsHandle(AndroidWindowBounds* _p)
        : p(_p) {}
    BoundsHandle(jlong _l)
        : l(_l) {}

    union {
        AndroidWindowBounds* p = nullptr;
        jlong                l;
    };
};

//-----------------------------------------------------------------------------
// JNI functions

extern "C" JNIEXPORT void JNICALL
Java_org_citra_citra_1emu_vr_ui_VrUILayer_00024Companion_nativeSetBounds(JNIEnv* env, jobject thiz,
                                                                         jlong handle, jint left,
                                                                         jint top, jint right,
                                                                         jint bottom) {
    AndroidWindowBounds* b = BoundsHandle(handle).p;
    b->mLeftInDp           = left;
    b->mTopInDp            = top;
    b->mRightInDp          = right;
    b->mBottomInDp         = bottom;
}

//-----------------------------------------------------------------------------
// Local sysprops

static constexpr std::chrono::milliseconds kMinTimeBetweenChecks(500);

// How long a trigger pull takes, and how much cursor history is kept to place
// presses where the user aimed (see UILayer::SendClickToUI()).
constexpr std::chrono::milliseconds kTriggerPullDuration(120);
constexpr std::chrono::milliseconds kCursorHistoryDuration(500);

// Get density on an interval
float GetDensitySysprop() {
    const float                                               kDefaultDensity = 1200;
    static float                                              lastDensity     = kDefaultDensity;
    static std::chrono::time_point<std::chrono::steady_clock> lastTime        = {};
    // Only check the sysprop every 500ms
    if ((std::chrono::steady_clock::now() - lastTime) >= kMinTimeBetweenChecks) {
        lastTime    = std::chrono::steady_clock::now();
        lastDensity = SyspropUtils::GetSysPropAsFloat("debug.citra.density", kDefaultDensity);
    }
    return lastDensity;
}

//-----------------------------------------------------------------------------
// Panel math

bool GetRayIntersectionWithPanel(const XrPosef& worldFromPanel, const uint32_t panelWidth,
                                 const uint32_t panelHeight, const XrVector2f& scaleFactor,
                                 const XrVector3f& start, const XrVector3f& end,
                                 XrVector2f& result2d, XrPosef& result3d)

{
    const XrPosef    panelFromWorld = XrMath::Posef::Inverted(worldFromPanel);
    const XrVector3f localStart     = XrMath::Posef::Transform(panelFromWorld, start);
    const XrVector3f localEnd       = XrMath::Posef::Transform(panelFromWorld, end);
    // Note: intersection is solved in panel-local space, where the panel plane is z = 0.
    // Panel translation/rotation are already accounted for by transforming the ray with panelFromWorld.
    const float tan = localStart.z / (localStart.z - localEnd.z);

    // Check for backwards controller
    if (tan < 0) {
        ALOGD("Backwards controller");
        return false;
    }
    result3d.position    = start + (end - start) * tan;
    result3d.orientation = worldFromPanel.orientation;

    const XrVector2f result2dNDC = {
        (localStart.x + (localEnd.x - localStart.x) * tan) / (scaleFactor.x),
        (localStart.y + (localEnd.y - localStart.y) * tan) / (scaleFactor.y)};

    const AndroidWindowBounds viewBounds(panelWidth, panelHeight);
    viewBounds.Transform(result2dNDC, result2d);
    const bool isInBounds = result2d.x >= 0 && result2d.y >= 0 && result2d.x < viewBounds.Width() &&
                            result2d.y < viewBounds.Height();
    if (!isInBounds) { return false; }

    return true;
}

// Uses a density for scaling and sets aspect ratio
XrVector2f GetDensityScaleForSize(const int32_t texWidth, const int32_t texHeight,
                                  const float scaleFactor) {
    const float density = GetDensitySysprop();
    return XrVector2f{static_cast<float>(texWidth) / density,
                      (static_cast<float>(texHeight) / density)} *
           scaleFactor;
}

} // anonymous namespace

UILayer::UILayer(const jclass classObject, const XrVector3f&& position,
                 const XrQuaternionf&& orientation, JNIEnv* env, jobject activityObject,
                 const XrSession& session)
    : mWorldFromPanel(XrPosef{orientation, position})
    , mSession(session)
    , mEnv(env) {
    const int32_t initializationStatus = Init(classObject, activityObject, position, session);
    if (initializationStatus < 0) {
        FAIL("Could not initialize %s() -- error '%d'", __FUNCTION__, initializationStatus);
    }
}

UILayer::~UILayer() { Shutdown(); }

void UILayer::Frame(const XrSpace&                   space,
                    std::vector<XrCompositionLayer>& layers,
                    uint32_t&                        layerCount) const

{

    XrCompositionLayerQuad layer = {};

    layer.type       = XR_TYPE_COMPOSITION_LAYER_QUAD;
    layer.next = mSurfaceSwapchain->GetLayerNext();
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.layerFlags |= XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
    layer.layerFlags |= XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    // NOTE: may not want unpremultiplied alpha

    layer.space = space;

    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    memset(&layer.subImage, 0, sizeof(XrSwapchainSubImage));
    const Swapchain& swapchain = mSurfaceSwapchain->GetSwapchain();
    layer.subImage.swapchain = swapchain.mHandle;
    layer.subImage.imageRect.offset.x      = 0;
    layer.subImage.imageRect.offset.y      = 0;
    layer.subImage.imageRect.extent.width = swapchain.mWidth;
    layer.subImage.imageRect.extent.height = swapchain.mHeight;
    layer.subImage.imageArrayIndex         = 0;
    layer.pose                             = mWorldFromPanel;
    const auto scale = GetDensityScaleForSize(swapchain.mWidth, swapchain.mHeight, 1.0f);
    layer.size.width  = scale.x;
    layer.size.height = scale.y;
    layers[layerCount++].mQuad = layer;
    if (!mHasLoggedFirstFrame) {
        mHasLoggedFirstFrame = true;
        ALOGI("UILayer {}x{}: first submitted at ({}, {}, {}), size {}x{} m", swapchain.mWidth,
              swapchain.mHeight, layer.pose.position.x, layer.pose.position.y,
              layer.pose.position.z, layer.size.width, layer.size.height);
    }
}

bool UILayer::GetRayIntersectionWithPanel(const XrVector3f& start,
                                          const XrVector3f& end,
                                          XrVector2f&       result2d,
                                          XrPosef&          result3d) const {
    const Swapchain& swapchain = mSurfaceSwapchain->GetSwapchain();
    const XrVector2f scale = GetDensityScaleForSize(swapchain.mWidth, swapchain.mHeight, 1.0f);
    const bool isHit = ::GetRayIntersectionWithPanel(mWorldFromPanel, swapchain.mWidth,
                                                     swapchain.mHeight, scale, start, end,
                                                     result2d, result3d);
    if (!isHit) {
        mCursorHistory.clear();
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    mCursorHistory.push_back({now, result2d});
    while (mCursorHistory.size() > 1 &&
           now - mCursorHistory.front().mTime > kCursorHistoryDuration) {
        mCursorHistory.pop_front();
    }
    return true;
}

// Next error code: -9
int32_t UILayer::Init(const jclass classObject, const jobject activityObject,
                      const XrVector3f& position, const XrSession& session) {
    mVrUILayerClass = classObject;

    jmethodID vrUILayerConstructor =
        mEnv->GetMethodID(mVrUILayerClass, "<init>", "(Lorg/citra/citra_emu/vr/VrActivity;)V");
    BAIL_ON_COND(vrUILayerConstructor == nullptr, "no java window constructor", -2);

    mVrUILayerObject = mEnv->NewObject(mVrUILayerClass, vrUILayerConstructor, activityObject);
    BAIL_ON_COND(mVrUILayerObject == nullptr, "Could not construct java window", -3);

    mGetBoundsMethodID = mEnv->GetMethodID(mVrUILayerClass, "getBoundsForView", "(J)I");
    BAIL_ON_COND(mGetBoundsMethodID == nullptr, "could not find getBoundsForView()", -4);

    mSetSurfaceMethodId =
        mEnv->GetMethodID(mVrUILayerClass, "setSurface", "(Landroid/view/Surface;II)I");
    BAIL_ON_COND(mSetSurfaceMethodId == nullptr, "could not find setSurface()", -5);

    mSendClickToUIMethodID = mEnv->GetMethodID(mVrUILayerClass, "sendClickToUI", "(FFI)I");

    BAIL_ON_COND(mSendClickToUIMethodID == nullptr, "could not find sendClickToUI()", -6);

    mSendScrollToUIMethodID = mEnv->GetMethodID(mVrUILayerClass, "sendScrollToUI", "(FFFF)I");

    BAIL_ON_COND(mSendScrollToUIMethodID == nullptr, "could not find sendScrollToUI()", -8);

    BAIL_ON_ERR(CreateSwapchain(activityObject), -7);

    return 0;
}

void UILayer::Shutdown() {
    // Stop the virtual display drawing into the surface before it goes away.
    if (mVrUILayerObject != nullptr) {
        const jmethodID releaseMethodID = mEnv->GetMethodID(mVrUILayerClass, "release", "()V");
        if (releaseMethodID != nullptr) {
            mEnv->CallVoidMethod(mVrUILayerObject, releaseMethodID);
        }
        if (mEnv->ExceptionCheck()) {
            mEnv->ExceptionDescribe();
            mEnv->ExceptionClear();
        }
    }
    mSurfaceSwapchain.reset();
    mVrUILayerClass    = nullptr;
    // These steps are not strictly necessary for app shutdown, as references are cleaned up when
    // the JVM is destroyed, but memory-saving if this class is destroyed/re-initialized at runtime.
    // mEnv->DeleteLocalRef(mVrUILayerObject);
    mVrUILayerObject = nullptr;
}

int UILayer::CreateSwapchain(const jobject activityObject) {
    uint32_t width = 0;
    uint32_t height = 0;
    {
        AndroidWindowBounds viewBounds;
        if (mEnv->ExceptionCheck()) { mEnv->ExceptionClear(); }
        const jint ret =
            mEnv->CallIntMethod(mVrUILayerObject, mGetBoundsMethodID, BoundsHandle(&viewBounds).l);
        // Check for exceptions (and log them).
        if (mEnv->ExceptionCheck()) {
            mEnv->ExceptionDescribe();
            mEnv->ExceptionClear();
            FAIL("Exception in getBoundsForView()");
        }
        if (ret < 0) {
            ALOGE("{}() returned error {}", __FUNCTION__, ret);
            return -1;
        }
        if (viewBounds.Width() == 0 || viewBounds.Height() == 0) {
            ALOGE("{}() returned invalid bounds {} x {}", __FUNCTION__, viewBounds.Width(),
                  viewBounds.Height());
            return -2;
        }
        width = viewBounds.Width();
        height = viewBounds.Height();
    }
    // Initialize swapchain.
    {
        mSurfaceSwapchain = std::make_unique<SurfaceSwapchain>(mSession, mEnv, activityObject, width,
                                                               height);

        ALOGD("UILayer: created swapchain {}x{}", width, height);

        mEnv->CallIntMethod(mVrUILayerObject, mSetSurfaceMethodId, mSurfaceSwapchain->GetSurface(),
                            (int)width, (int)height);
        if (mEnv->ExceptionCheck()) {
            mEnv->ExceptionDescribe();
            mEnv->ExceptionClear();
            FAIL("Exception in setSurface()");
        }
    }
    return 0;
}

void UILayer::UpdateSurface() {
    mSurfaceSwapchain->Update();
}

void UILayer::SendClickToUI(const XrVector2f& pos2d, const int type) {
    // Pulling the trigger tilts the controller, and the press only registers at
    // the end of the pull: by then the cursor has dropped by a few centimeters,
    // enough to miss a small button. Place the press where the cursor was just
    // before the pull (or where it entered the panel, if more recently).
    XrVector2f position = pos2d;
    if (type == 1 && !mCursorHistory.empty()) {
        const auto aimedTime = std::chrono::steady_clock::now() - kTriggerPullDuration;
        position = mCursorHistory.front().mPosition;
        for (const CursorSample& sample : mCursorHistory) {
            if (sample.mTime > aimedTime) {
                break;
            }
            position = sample.mPosition;
        }
    }
    mEnv->CallIntMethod(mVrUILayerObject, mSendClickToUIMethodID, position.x, position.y, type);
}

void UILayer::SendScrollToUI(const XrVector2f& pos2d, const XrVector2f& scroll2d) {
    mEnv->CallIntMethod(mVrUILayerObject, mSendScrollToUIMethodID, pos2d.x, pos2d.y, scroll2d.x,
                        scroll2d.y);
}
