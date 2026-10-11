#include "JniClassNames.h"

#include "LogUtils.h"

#include <cassert>

namespace VR {
namespace JniGlobalRef {
jclass gVrKeyboardLayerClass     = nullptr;
jclass gVrErrorMessageLayerClass = nullptr;
jclass gVrRibbonLayerClass       = nullptr;
jclass gVrGameListLayerClass     = nullptr;
jclass gVrControllerDiagramLayerClass = nullptr;
jclass gVrButtonLayoutLayerClass = nullptr;
} // namespace JniGlobalRef
} // namespace VR

void VR::JNI::InitJNI(JNIEnv* jni) {
    assert(jni != nullptr);

    VR::JniGlobalRef::gVrKeyboardLayerClass = static_cast<jclass>(
        jni->NewGlobalRef(jni->FindClass("org/citra/citra_emu/vr/ui/VrKeyboardLayer")));
    if (VR::JniGlobalRef::gVrKeyboardLayerClass == nullptr) {
        FAIL("Could not find VrKeyboardLayer class");
    }
    VR::JniGlobalRef::gVrErrorMessageLayerClass = static_cast<jclass>(
        jni->NewGlobalRef(jni->FindClass("org/citra/citra_emu/vr/ui/VrErrorMessageLayer")));
    if (VR::JniGlobalRef::gVrErrorMessageLayerClass == nullptr) {
        FAIL("Could not find VrErrorMessageLayer class");
    }

    VR::JniGlobalRef::gVrRibbonLayerClass = static_cast<jclass>(
        jni->NewGlobalRef(jni->FindClass("org/citra/citra_emu/vr/ui/VrRibbonLayer")));
    if (VR::JniGlobalRef::gVrRibbonLayerClass == nullptr) {
        FAIL("Could not find VrRibbonLayer class");
    }
    VR::JniGlobalRef::gVrGameListLayerClass = static_cast<jclass>(
        jni->NewGlobalRef(jni->FindClass("org/citra/citra_emu/vr/ui/VrGameListLayer")));
    if (VR::JniGlobalRef::gVrGameListLayerClass == nullptr) {
        FAIL("Could not find VrGameListLayer class");
    }
    VR::JniGlobalRef::gVrControllerDiagramLayerClass = static_cast<jclass>(jni->NewGlobalRef(
        jni->FindClass("org/citra/citra_emu/vr/ui/VrControllerDiagramLayer")));
    if (VR::JniGlobalRef::gVrControllerDiagramLayerClass == nullptr) {
        FAIL("Could not find VrControllerDiagramLayer class");
    }
    VR::JniGlobalRef::gVrButtonLayoutLayerClass = static_cast<jclass>(jni->NewGlobalRef(
        jni->FindClass("org/citra/citra_emu/vr/ui/VrButtonLayoutLayer")));
    if (VR::JniGlobalRef::gVrButtonLayoutLayerClass == nullptr) {
        FAIL("Could not find VrButtonLayoutLayer class");
    }
}

void VR::JNI::CleanupJNI(JNIEnv* jni) {
    assert(jni != nullptr);
    VR::JniGlobalRef::gVrKeyboardLayerClass     = nullptr;
    VR::JniGlobalRef::gVrErrorMessageLayerClass = nullptr;
    VR::JniGlobalRef::gVrRibbonLayerClass       = nullptr;
    VR::JniGlobalRef::gVrGameListLayerClass     = nullptr;
    VR::JniGlobalRef::gVrControllerDiagramLayerClass = nullptr;
    VR::JniGlobalRef::gVrButtonLayoutLayerClass = nullptr;
}
