/*******************************************************************************

Filename    :   ControllerDiagramLayer.h

Content     :   The controller diagram panel: the controllers, labelled with the
                current mapping (org.citra.citra_emu.vr.ui.VrControllerDiagramLayer).

Authors     :   CitraVR contributors
License     :   Licensed under GPLv3 or any later version.
                Refer to the license.txt file included.

*******************************************************************************/

#pragma once

#include "../vr_settings.h"
#include "UILayer.h"

#include <array>

class ControllerDiagramLayer : public UILayer {
public:
    ControllerDiagramLayer(const XrPosef& pose, JNIEnv* jni, jobject activityObject,
                           const XrSession& session);

    /** Relabels the diagram if the mapping changed since the last call. */
    void UpdateMapping(const std::array<int32_t, VRSettings::kNumVrControls>& mapping);

private:
    jmethodID mSetMappingMethodId = nullptr;
    std::array<int32_t, VRSettings::kNumVrControls> mShownMapping{};
    bool mHasShownMapping = false;
};
