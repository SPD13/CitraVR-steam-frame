#pragma once

#include "UILayer.h"

class RibbonLayer : public UILayer {
public:
    RibbonLayer(const XrPosef& pose, JNIEnv* jni, jobject activityObject, const XrSession& session);

    bool IsMenuBackgroundSelected() const;
    /** Updates the menu bar's Passthrough button (hidden when not supported). */
    void SetPassthroughState(const bool isOn, const bool isSupported);
    void SetPanelFromController(const XrVector3f& controllerPosition);
    void SetPanelFromThumbstick(const float thumbstickY);
    void SetPanelWithPose(const XrPosef& pose);

    const XrPosef& GetPose() const { return mWorldFromPanel; }

private:
    const XrPosef mInitialPose;
    jmethodID     mIsMenuBackgroundSelectedMethodId = nullptr;
    jmethodID     mSetPassthroughStateMethodId      = nullptr;
};
