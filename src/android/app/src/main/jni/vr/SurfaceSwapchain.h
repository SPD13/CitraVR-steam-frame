/*******************************************************************************

Filename    :   SurfaceSwapchain.h

Content     :   A swapchain whose contents are drawn by Android through an
                android.view.Surface.

                On runtimes with XR_KHR_android_surface_swapchain (and
                XR_FB_composition_layer_image_layout to undo its vertical flip),
                this is a surface-backed swapchain, as on Quest: Android draws
                straight into the swapchain and there is nothing to do per frame.

                Runtimes without them (SteamVR on Steam Frame, via Lepton) get an
                ordinary swapchain fed from a SurfaceTexture: Android draws into
                the SurfaceTexture, and Update() copies each new frame into the
                swapchain with a full-screen draw.

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
    /** Creates the swapchain and the surface Android draws into. Must be called
     *  on the thread that owns the EGL context used by the XrSession.
     *
     *  @param session a valid XrSession
     *  @param env the JNI environment of the calling thread
     *  @param activityObject the VR activity, used to find the app's Java classes
     *  @param width width of the swapchain (and surface) in pixels
     *  @param height height of the swapchain (and surface) in pixels
     */
    SurfaceSwapchain(const XrSession& session, JNIEnv* env, jobject activityObject,
                     const uint32_t width, const uint32_t height);
    ~SurfaceSwapchain();

    SurfaceSwapchain(const SurfaceSwapchain&) = delete;
    SurfaceSwapchain& operator=(const SurfaceSwapchain&) = delete;

    const Swapchain& GetSwapchain() const {
        return mMain.mSwapchain;
    }

    /** The android.view.Surface Android should draw into. */
    jobject GetSurface() const {
        return mSurface;
    }

    /** Copies the latest frame Android produced into the swapchain, if there is
     *  a new one. No-op for surface-backed swapchains. Call once per frame, on
     *  the thread that created this object, before xrEndFrame().
     */
    void Update();

    /** The `next` chain for a layer that samples this swapchain. */
    const void* GetLayerNext() const;

    /** Converts a rectangle in Android surface coordinates (origin at the top
     *  left, y down) to the image rect of this swapchain.
     */
    XrRect2Di ToImageRect(const int32_t x, const int32_t y, const int32_t width,
                          const int32_t height) const;

    /** Adds a second, smaller swapchain that Update() fills with a filtered
     *  downscale of a region of the surface, for a panel shown much smaller than
     *  its texels. Swapchains have a single mip level (SteamVR on Steam Frame
     *  shows the smaller levels of shared images as garbage), so a panel the
     *  compositor has to shrink a lot shimmers; this does the shrinking here,
     *  with a proper filter. SurfaceTexture path only.
     *
     *  @param x, y, width, height the region, in Android surface coordinates
     *  @param outputWidth, outputHeight the size of the region's swapchain
     *  @return false (and nothing is added) for surface-backed swapchains
     */
    bool AddDownscaledRegion(const int32_t x, const int32_t y, const int32_t width,
                             const int32_t height, const uint32_t outputWidth,
                             const uint32_t outputHeight);

    bool HasDownscaledRegion() const {
        return mRegion.mSwapchain.mHandle != XR_NULL_HANDLE;
    }

    const Swapchain& GetDownscaledRegionSwapchain() const {
        return mRegion.mSwapchain;
    }

    /** Like ToImageRect(), for the downscaled region's swapchain. The rectangle
     *  is in Android surface coordinates and should lie within the region.
     */
    XrRect2Di ToDownscaledRegionImageRect(const int32_t x, const int32_t y, const int32_t width,
                                          const int32_t height) const;

private:
    // A swapchain filled by drawing the SurfaceTexture (or a region of it) into it.
    struct CopyTarget {
        Swapchain mSwapchain{XR_NULL_HANDLE, 0, 0};
        std::vector<XrSwapchainImageOpenGLESKHR> mImages;
        std::vector<GLuint> mFramebuffers;
        // The part of the surface copied, in Android surface coordinates.
        XrRect2Di mSourceRect{};
        std::chrono::steady_clock::time_point mLastDumpTime{};
    };

    void CreateAndroidSurfaceSwapchain(const XrSession& session);
    void CreateSurfaceTextureSwapchain(const XrSession& session, jobject activityObject);
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

    // Whether the swapchains store sRGB-encoded values, in which case the copy
    // decodes the (sRGB-encoded) Android content to linear and the GPU encodes it
    // back on write.
    bool mIsSrgbSwapchain = false;
    int64_t mSwapchainFormat = 0;

    // The whole surface (the only swapchain on the surface-backed path), and the
    // optional downscaled region.
    CopyTarget mMain;
    CopyTarget mRegion;

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
