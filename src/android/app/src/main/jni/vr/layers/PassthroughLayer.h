/*******************************************************************************

Filename    :   PassthroughLayer.h
Authors     :   Amanda M. Watson
License     :   Licensed under GPLv3 or any later version.
                Refer to the license.txt file included.

*******************************************************************************/
#pragma once

#include <openxr/openxr.h>

class PassthroughLayer {
public:
    PassthroughLayer(const XrSession& session);
    ~PassthroughLayer();
    void Frame(XrCompositionLayerPassthroughFB& layer) const;

private:
    XrPassthroughFB      mPassthrough      = XR_NULL_HANDLE;
    XrPassthroughLayerFB mPassthroughLayer = XR_NULL_HANDLE;
}; // class PassthroughLayer

/** Passthrough for runtimes without XR_FB_passthrough that offer
 *  XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND (SteamVR on Steam Frame). Those
 *  runtimes only show the real world behind a projection layer, so this
 *  submits a fully transparent one at the back of the layer stack; frames
 *  using it must be ended with XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND.
 */
class AlphaBlendPassthroughLayer {
public:
    AlphaBlendPassthroughLayer(const XrSession& session);
    ~AlphaBlendPassthroughLayer();
    /** Fills `layer`. Returns false, leaving it unusable, if the views could not
     *  be located for this frame. The layer points into this object, so it stays
     *  valid until the next call.
     */
    bool Frame(const XrSession& session, const XrSpace& space, const XrTime predictedDisplayTime,
               XrCompositionLayerProjection& layer);

private:
    static constexpr uint32_t kImageSize = 16;

    XrSwapchain mSwapchain = XR_NULL_HANDLE;
    XrCompositionLayerProjectionView mViews[2] = {};
}; // class AlphaBlendPassthroughLayer
