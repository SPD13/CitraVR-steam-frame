/*******************************************************************************

Filename    :   SurfaceSwapchain.h

Content     :   A swapchain whose contents are drawn by Android through an
                android.view.Surface.

                On runtimes with XR_KHR_android_surface_swapchain (and
                XR_FB_composition_layer_image_layout to undo its vertical flip),
                this is a surface-backed swapchain, as on Quest: Android draws
                straight into the swapchain and there is nothing to do per frame.

                Runtimes without them (SteamVR on Steam Frame, via Lepton) get
                ordinary swapchains fed from a SurfaceTexture: Android draws into
                the SurfaceTexture, and Update() copies each new frame into the
                swapchain of the whole surface and/or into the swapchains of
                regions of it.

Authors     :   CitraVR contributors
License     :   Licensed under GPLv3 or any later version.
                Refer to the license.txt file included.

*******************************************************************************/

#pragma once

#include "OpenXR.h"
#include "Swapchain.h"

#include <GLES3/gl3.h>

#include <chrono>
#include <vector>

class SurfaceSwapchain {
public:
    /** Whether surfaces are copied into ordinary swapchains (the SurfaceTexture
     *  path), which is what makes regions available. Valid once the OpenXR
     *  instance exists.
     */
    static bool UsesCopies();

    /** Creates the surface Android draws into and, unless told otherwise, the
     *  swapchain of the whole surface. Must be called on the thread that owns
     *  the EGL context used by the XrSession.
     *
     *  @param session a valid XrSession
     *  @param env the JNI environment of the calling thread
     *  @param activityObject the VR activity, used to find the app's Java classes
     *  @param width width of the surface (and its swapchain) in pixels
     *  @param height height of the surface (and its swapchain) in pixels
     *  @param withWholeSurfaceSwapchain false to only copy regions (AddRegion());
     *         ignored, and always true, for surface-backed swapchains
     */
    SurfaceSwapchain(const XrSession& session, JNIEnv* env, jobject activityObject,
                     const uint32_t width, const uint32_t height,
                     const bool withWholeSurfaceSwapchain = true);
    ~SurfaceSwapchain();

    SurfaceSwapchain(const SurfaceSwapchain&) = delete;
    SurfaceSwapchain& operator=(const SurfaceSwapchain&) = delete;

    /** The swapchain of the whole surface (none if created without it). */
    const Swapchain& GetSwapchain() const {
        return mMain.mSwapchain;
    }

    /** The android.view.Surface Android should draw into. */
    jobject GetSurface() const {
        return mSurface;
    }

    /** Copies the latest frame Android produced into the swapchains, if there is
     *  a new one. No-op for surface-backed swapchains. Call once per frame, on
     *  the thread that created this object, before xrEndFrame().
     */
    void Update();

    /** The `next` chain for a layer that samples these swapchains. */
    const void* GetLayerNext() const;

    /** Converts a rectangle in Android surface coordinates (origin at the top
     *  left, y down) to the image rect of the whole surface's swapchain.
     */
    XrRect2Di ToImageRect(const int32_t x, const int32_t y, const int32_t width,
                          const int32_t height) const;

    /** Adds a swapchain that Update() fills with a region of the surface, scaled
     *  to the swapchain's size (with a box filter when shrinking). Lets a panel
     *  that shows part of the surface get a swapchain of its own size: smaller
     *  than its texels, so the compositor doesn't shrink (and shimmer) it, or
     *  smaller than the whole surface, see GameSurfaceLayer. SurfaceTexture
     *  path only.
     *
     *  @param x, y, width, height the region, in Android surface coordinates
     *  @param outputWidth, outputHeight the size of the region's swapchain
     *  @return the region's index, or -1 (and nothing is added) for
     *          surface-backed swapchains
     */
    int AddRegion(const int32_t x, const int32_t y, const int32_t width, const int32_t height,
                  const uint32_t outputWidth, const uint32_t outputHeight);

    /** Changes how much of a region's swapchain the region is copied into
     *  (from the bottom left; at most the swapchain's size). Lets the
     *  resolution a panel is shown at change without a new swapchain. The next
     *  Update() copies the current frame again at the new size.
     */
    void SetRegionOutputSize(const int region, const uint32_t width, const uint32_t height);

    const Swapchain& GetRegionSwapchain(const int region) const {
        return mRegions[region].mSwapchain;
    }

    /** Like ToImageRect(), for a region's swapchain. The rectangle is in Android
     *  surface coordinates and should lie within the region.
     */
    XrRect2Di ToRegionImageRect(const int region, const int32_t x, const int32_t y,
                                const int32_t width, const int32_t height) const;

private:
    // A swapchain filled by drawing the SurfaceTexture (or a region of it) into it.
    struct CopyTarget {
        Swapchain mSwapchain{XR_NULL_HANDLE, 0, 0};
        std::vector<XrSwapchainImageOpenGLESKHR> mImages;
        std::vector<GLuint> mFramebuffers;
        // The part of the surface copied, in Android surface coordinates.
        XrRect2Di mSourceRect{};
        std::chrono::steady_clock::time_point mLastDumpTime{};
        int mBurstFramesDumped = 0;
        // The part of the swapchain the copy fills, from the bottom left.
        uint32_t mOutputWidth = 0;
        uint32_t mOutputHeight = 0;
    };

    void CreateAndroidSurfaceSwapchain(const XrSession& session);
    void CreateSurfaceTextureSwapchain(const XrSession& session, jobject activityObject,
                                       const bool withWholeSurfaceSwapchain);
    void CreateCopyResources();
    void CreateCopyTarget(const XrSession& session, const uint32_t width, const uint32_t height,
                          const XrRect2Di& sourceRect, CopyTarget& target);
    void DestroyCopyTarget(CopyTarget& target);
    void ClearSwapchainImage(CopyTarget& target);
    void CopySurfaceTexture(CopyTarget& target, const float transformMatrix[16]);
    void DumpBoundFramebufferIfRequested(CopyTarget& target, const float transformMatrix[16]);
    void Shutdown();

    const bool mIsAndroidSurfaceSwapchain;
    JNIEnv* mEnv = nullptr;
    XrSession mSession = XR_NULL_HANDLE;
    jobject mSurface = nullptr;
    // Size of the surface (and of the whole surface's swapchain, if any).
    uint32_t mSurfaceWidth = 0;
    uint32_t mSurfaceHeight = 0;

    //============================
    // SurfaceTexture path only.

    jobject mSurfaceTexture = nullptr;
    jobject mFrameListener = nullptr; // org.citra.citra_emu.vr.utils.SurfaceTextureFrameListener
    jfloatArray mTransformMatrixArray = nullptr;
    jmethodID mUpdateTexImageMethodID = nullptr;
    jmethodID mGetTransformMatrixMethodID = nullptr;
    jmethodID mGetFrameCountMethodID = nullptr;
    // Frames latched with updateTexImage(), compared against the listener's count.
    jlong mLatchedFrameCount = 0;
    bool mHasCopiedFrame = false;
    // Copy the latched frame again on the next Update(), even without a new one.
    bool mShouldRecopy = false;

    // Whether the swapchains store sRGB-encoded values, in which case the copy
    // decodes the (sRGB-encoded) Android content to linear and the GPU encodes it
    // back on write.
    bool mIsSrgbSwapchain = false;
    int64_t mSwapchainFormat = 0;

    // The whole surface (the only swapchain on the surface-backed path; no
    // swapchain if created without it), and the regions.
    CopyTarget mMain;
    std::vector<CopyTarget> mRegions;

    GLuint mExternalTexture = 0;
    GLuint mProgram = 0;
    GLint mTexMatrixUniform = -1;
    GLint mDecodeSrgbUniform = -1;
    GLint mSourceOffsetUniform = -1;
    GLint mSourceScaleUniform = -1;
    GLint mTapsPerAxisUniform = -1;
    GLint mTapStepUniform = -1;
    GLuint mVertexArray = 0;
    GLuint mVertexBuffer = 0;
};
