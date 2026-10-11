/*******************************************************************************

Filename    :   ControllerDiagramLayer.cpp

Content     :   The controller diagram panel. See ControllerDiagramLayer.h.

Authors     :   CitraVR contributors
License     :   Licensed under GPLv3 or any later version.
                Refer to the license.txt file included.

*******************************************************************************/

#include "ControllerDiagramLayer.h"

#include "../utils/Common.h"
#include "../utils/JniClassNames.h"
#include "../utils/LogUtils.h"

ControllerDiagramLayer::ControllerDiagramLayer(const XrPosef& pose, JNIEnv* jni,
                                               jobject activityObject, const XrSession& session)
    : UILayer(VR::JniGlobalRef::gVrControllerDiagramLayerClass, XrVector3f(pose.position),
              XrQuaternionf(pose.orientation), jni, activityObject, session) {
    mSetMappingMethodId = jni->GetMethodID(GetVrUILayerClass(), "setMapping", "([I)V");
    if (mSetMappingMethodId == nullptr) {
        FAIL("Could not find setMapping()");
    }
}

void ControllerDiagramLayer::UpdateMapping(
    const std::array<int32_t, VRSettings::kNumVrControls>& mapping) {
    if (mHasShownMapping && mapping == mShownMapping) {
        return;
    }
    JNIEnv* env = GetEnv();
    jintArray array = env->NewIntArray(static_cast<jsize>(mapping.size()));
    env->SetIntArrayRegion(array, 0, static_cast<jsize>(mapping.size()), mapping.data());
    env->CallVoidMethod(GetVrUILayerObject(), mSetMappingMethodId, array);
    env->DeleteLocalRef(array);
    mShownMapping = mapping;
    mHasShownMapping = true;
}
