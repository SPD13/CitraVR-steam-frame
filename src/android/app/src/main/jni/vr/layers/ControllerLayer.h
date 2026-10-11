/*******************************************************************************

Filename    :   ControllerLayer.h

Content     :   Draws the controllers and the pointer ray.

                Both are quad layers: SteamVR on Steam Frame draws quad layers
                over projection layers whatever their order and depth, so 3D
                content in a projection layer would end up behind the panels.
                - The ray is a thin quad along the ray, turned towards the head.
                - Each controller is drawn per eye into a quad at the
                  controller, facing that eye and shown to it only (an
                  impostor), rendered from that eye's position so the stereo
                  pair is exact.

                The controller models are SteamVR's render models for the
                controllers in use, read from the SteamVR runtime (on Steam
                Frame, Lepton mounts it at /data/steamvr/runtime) and placed
                with their `openxr_aim` component, the same way SteamVR places
                them. Where no model is found (e.g. on Quest), only the ray is
                drawn.

Authors     :   CitraVR contributors
License     :   Licensed under GPLv3 or any later version.
                Refer to the license.txt file included.

*******************************************************************************/

#pragma once

#include "../OpenXR.h"
#include "../XrController.h"
#include "../utils/Common.h"

#include <GLES3/gl3.h>

#include <array>
#include <future>
#include <memory>
#include <vector>

class ControllerLayer {
public:
    /** The pointer ray of the preferred controller, from its aim pose. */
    struct PointerRay {
        bool mIsVisible = false;
        XrPosef mAimPose = {};
        // Length of the ray, in meters.
        float mLength = 0.0f;
        // Whether the ray ends on a panel (drawn up to it) or in the air (fades out).
        bool mHitsPanel = false;
    };

    /** Must be called on the thread that owns the EGL context used by the session. */
    ControllerLayer(const XrSession& session);
    ~ControllerLayer();

    ControllerLayer(const ControllerLayer&) = delete;
    ControllerLayer& operator=(const ControllerLayer&) = delete;

    /** Draws the visible controllers and the ray, and appends their layers to
     *  `layers` (as long as there is room). The layers point into this object,
     *  so they stay valid until the next call.
     */
    void Frame(const XrSession& session, const XrSpace& space, const XrTime predictedDisplayTime,
               const InputStateFrame& input, const bool showControllers, const PointerRay& ray,
               std::vector<XrCompositionLayer>& layers, uint32_t& layerCount);

private:
    // A controller model as read from disk (on a background thread).
    struct ModelData {
        std::vector<float> mVertices; // position (3), normal (3), uv (2)
        std::vector<uint32_t> mIndices;
        std::vector<uint8_t> mTextureRgba;
        uint32_t mTextureWidth = 0;
        uint32_t mTextureHeight = 0;
        // The model's pose in the controller's aim space.
        XrPosef mAimFromModel = {};
        // Bounding sphere, in model space.
        XrVector3f mCenter = {};
        float mRadius = 0.0f;
    };
    // A controller model ready to draw.
    struct Model {
        GLuint mVertexArray = 0;
        GLuint mVertexBuffer = 0;
        GLuint mIndexBuffer = 0;
        GLuint mTexture = 0;
        GLsizei mIndexCount = 0;
        XrPosef mAimFromModel = {};
        XrVector3f mCenter = {};
        float mRadius = 0.0f;
    };

    static std::unique_ptr<ModelData> LoadModel(const char* side);
    void UploadModels();
    XrSwapchain CreateRayTexture(const XrSession& session, const bool fades);
    void DrawModel(const Model& model, const XrPosef& aimPose, const float viewProjection[16]);

    bool mIsSrgb = false;
    int64_t mColorFormat = 0;

    // Controller impostors: one cell per hand and eye.
    XrSwapchain mImpostorSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> mImpostorImages;
    GLuint mDepthRenderbuffer = 0;
    GLuint mFramebuffer = 0;

    // Ray textures, drawn once: solid (ends on a panel) and fading out.
    XrSwapchain mSolidRaySwapchain = XR_NULL_HANDLE;
    XrSwapchain mFadingRaySwapchain = XR_NULL_HANDLE;

    GLuint mModelProgram = 0;
    GLint mModelMvpUniform = -1;
    GLint mModelRotationUniform = -1;

    std::future<std::array<std::unique_ptr<ModelData>, 2>> mModelsLoading;
    // Left, right; mIndexCount is 0 for a missing model.
    std::array<Model, 2> mModels = {};
}; // class ControllerLayer
