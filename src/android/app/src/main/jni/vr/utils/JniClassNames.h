#pragma once

#include <jni.h>

namespace VR {
namespace JniGlobalRef {
extern jclass gVrKeyboardLayerClass;
extern jclass gVrErrorMessageLayerClass;
extern jclass gVrRibbonLayerClass;
extern jclass gVrGameListLayerClass;

} // namespace JniGlobalRef

namespace JNI {
// Called during JNI_OnLoad
void InitJNI(JNIEnv* env);
// Called during JNI_OnUnload
void CleanupJNI(JNIEnv* env);
} // namespace JNI
} // namespace VR
