/*******************************************************************************

Filename    :   PassthroughLayer.cpp
Authors     :   Amanda M. Watson
License     :   Licensed under GPLv3 or any later version.
                Refer to the license.txt file included.

*******************************************************************************/

#include "PassthroughLayer.h"

#include "../OpenXR.h"

#include <GLES3/gl3.h>

#include <array>
#include <cstring>
#include <vector>

#define DECL_PFN(pfn) PFN_##pfn pfn = nullptr
#define INIT_PFN(pfn)                                                                              \
    OXR(xrGetInstanceProcAddr(OpenXr::GetInstance(), #pfn, (PFN_xrVoidFunction*)(&pfn)))

namespace {

DECL_PFN(xrCreatePassthroughFB);
DECL_PFN(xrDestroyPassthroughFB);
DECL_PFN(xrPassthroughStartFB);
DECL_PFN(xrPassthroughPauseFB);
DECL_PFN(xrCreatePassthroughLayerFB);
DECL_PFN(xrDestroyPassthroughLayerFB);
DECL_PFN(xrPassthroughLayerPauseFB);
DECL_PFN(xrPassthroughLayerResumeFB);

void InitOXRFunctions() {
    INIT_PFN(xrCreatePassthroughFB);
    INIT_PFN(xrDestroyPassthroughFB);
    INIT_PFN(xrPassthroughStartFB);
    INIT_PFN(xrPassthroughPauseFB);
    INIT_PFN(xrCreatePassthroughLayerFB);
    INIT_PFN(xrDestroyPassthroughLayerFB);
    INIT_PFN(xrPassthroughLayerPauseFB);
    INIT_PFN(xrPassthroughLayerResumeFB);
}

static bool gAreOXRFunctionsInitialized = false;

} // anonymous namespace

PassthroughLayer::PassthroughLayer(const XrSession& session) {

    if (!gAreOXRFunctionsInitialized) {
        InitOXRFunctions();
        gAreOXRFunctionsInitialized = true;
    }

    const XrPassthroughCreateInfoFB ptci = {XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    OXR(xrCreatePassthroughFB(session, &ptci, &mPassthrough));

    const XrPassthroughLayerCreateInfoFB plci = {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, nullptr,
                                                 mPassthrough,
                                                 XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB,
                                                 XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB};
    OXR(xrCreatePassthroughLayerFB(session, &plci, &mPassthroughLayer));

    OXR(xrPassthroughStartFB(mPassthrough));
    OXR(xrPassthroughLayerResumeFB(mPassthroughLayer));
}

PassthroughLayer::~PassthroughLayer() {
    OXR(xrPassthroughLayerPauseFB(mPassthroughLayer));
    OXR(xrPassthroughPauseFB(mPassthrough));
    OXR(xrDestroyPassthroughLayerFB(mPassthroughLayer));
    OXR(xrDestroyPassthroughFB(mPassthrough));

    mPassthroughLayer = XR_NULL_HANDLE;
    mPassthrough      = XR_NULL_HANDLE;
}

void PassthroughLayer::Frame(XrCompositionLayerPassthroughFB& layer) const {
    layer.type        = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB;
    layer.layerHandle = mPassthroughLayer;
    layer.flags       = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space       = XR_NULL_HANDLE;
}

AlphaBlendPassthroughLayer::AlphaBlendPassthroughLayer(const XrSession& session) {
    XrSwapchainCreateInfo xsci;
    memset(&xsci, 0, sizeof(xsci));
    xsci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    xsci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    xsci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    xsci.format = GL_SRGB8_ALPHA8;
    xsci.sampleCount = 1;
    xsci.width = kImageSize;
    xsci.height = kImageSize;
    xsci.faceCount = 1;
    xsci.arraySize = 1;
    xsci.mipCount = 1;
    OXR(xrCreateSwapchain(session, &xsci, &mSwapchain));

    // Fill the (static) image with transparent black.
    uint32_t numImages = 0;
    OXR(xrEnumerateSwapchainImages(mSwapchain, 0, &numImages, nullptr));
    std::vector<XrSwapchainImageOpenGLESKHR> images(
        numImages, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR, nullptr, 0});
    OXR(xrEnumerateSwapchainImages(mSwapchain, numImages, &numImages,
                                   (XrSwapchainImageBaseHeader*)images.data()));

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, nullptr};
    OXR(xrAcquireSwapchainImage(mSwapchain, &acquireInfo, &index));
    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr,
                                         XR_INFINITE_DURATION};
    OXR(xrWaitSwapchainImage(mSwapchain, &waitInfo));

    static constexpr std::array<uint8_t, kImageSize * kImageSize * 4> kTransparentImage = {};
    glBindTexture(GL_TEXTURE_2D, images[index].image);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kImageSize, kImageSize, GL_RGBA, GL_UNSIGNED_BYTE,
                    kTransparentImage.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, nullptr};
    OXR(xrReleaseSwapchainImage(mSwapchain, &releaseInfo));
}

AlphaBlendPassthroughLayer::~AlphaBlendPassthroughLayer() {
    if (mSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(mSwapchain);
    }
    mSwapchain = XR_NULL_HANDLE;
}

bool AlphaBlendPassthroughLayer::Frame(const XrSession& session, const XrSpace& space,
                                       const XrTime predictedDisplayTime,
                                       XrCompositionLayerProjection& layer) {
    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = OpenXr::VIEW_CONFIG_TYPE;
    locateInfo.displayTime = predictedDisplayTime;
    locateInfo.space = space;
    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t numViews = 0;
    if (XR_FAILED(xrLocateViews(session, &locateInfo, &viewState, 2, &numViews, views)) ||
        numViews < 2) {
        return false;
    }

    for (uint32_t eye = 0; eye < 2; eye++) {
        mViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        mViews[eye].pose = views[eye].pose;
        mViews[eye].fov = views[eye].fov;
        mViews[eye].subImage.swapchain = mSwapchain;
        mViews[eye].subImage.imageRect.extent.width = kImageSize;
        mViews[eye].subImage.imageRect.extent.height = kImageSize;
    }

    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = space;
    layer.viewCount = 2;
    layer.views = mViews;
    return true;
}
