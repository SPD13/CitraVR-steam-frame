/*******************************************************************************

Filename    :   SurfaceSwapchain.cpp

Content     :   A swapchain whose contents are drawn by Android through an
                android.view.Surface. See SurfaceSwapchain.h.

Authors     :   CitraVR contributors
License     :   Licensed under GPLv3 or any later version.
                Refer to the license.txt file included.

*******************************************************************************/

#include "SurfaceSwapchain.h"

#include "utils/Common.h"
#include "utils/JniUtils.h"
#include "utils/SyspropUtils.h"

#include <fmt/format.h>
#include "utils/LogUtils.h"

#include <GLES2/gl2ext.h> // GL_TEXTURE_EXTERNAL_OES

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <cstring>

#include <assert.h>

namespace {

// Android-surface-backed swapchains hold content that is vertically flipped
// relative to OpenXR's UV convention. Chain this on a layer's `next` (with
// XR_FB_composition_layer_image_layout enabled) so the compositor samples it
// flipped. This replaces the out-of-spec negative size.height/aspectRatio
// trick, which newer Horizon OS compositors render as nothing.
constexpr XrCompositionLayerImageLayoutFB kVerticalFlipLayout{
    XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB, nullptr,
    XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB};

// Draws the SurfaceTexture over the whole target. The SurfaceTexture's
// transform matrix maps GL-convention texture coordinates (origin at the
// bottom left) to the buffer Android produced, so the result is upright in GL
// convention, which is also OpenXR's convention for GLES swapchain images.
constexpr char kVertexShader[] = R"(#version 300 es
layout(location = 0) in vec2 aPosition;
uniform mat4 uTexMatrix;
out vec2 vTexCoord;
void main() {
    vec2 uv = aPosition * 0.5 + 0.5;
    vTexCoord = (uTexMatrix * vec4(uv, 0.0, 1.0)).xy;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

constexpr char kFragmentShader[] = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES uTexture;
uniform bool uDecodeSrgb;
in vec2 vTexCoord;
out vec4 outColor;
vec3 SrgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
void main() {
    vec4 color = texture(uTexture, vTexCoord);
    outColor = uDecodeSrgb ? vec4(SrgbToLinear(color.rgb), color.a) : color;
}
)";

constexpr GLfloat kFullScreenQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};

GLuint CompileShader(const GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint isCompiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &isCompiled);
    if (isCompiled != GL_TRUE) {
        char infoLog[1024] = {};
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        FAIL("SurfaceSwapchain: shader compilation failed: %s", infoLog);
    }
    return shader;
}

GLuint LinkProgram(const char* vertexSource, const char* fragmentSource) {
    const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    GLint isLinked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &isLinked);
    if (isLinked != GL_TRUE) {
        char infoLog[1024] = {};
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        FAIL("SurfaceSwapchain: program link failed: %s", infoLog);
    }
    return program;
}

void CheckJniException(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        FAIL("SurfaceSwapchain: exception in %s", what);
    }
}

// Picks an 8-bit RGBA format for the copy target, preferring sRGB so that the
// compositor treats Android's (sRGB-encoded) content as such.
int64_t ChooseSwapchainFormat(const XrSession& session) {
    uint32_t numFormats = 0;
    OXR(xrEnumerateSwapchainFormats(session, 0, &numFormats, nullptr));
    std::vector<int64_t> formats(numFormats);
    OXR(xrEnumerateSwapchainFormats(session, numFormats, &numFormats, formats.data()));
    for (const int64_t preferredFormat :
         {static_cast<int64_t>(GL_SRGB8_ALPHA8), static_cast<int64_t>(GL_RGBA8)}) {
        if (std::find(formats.begin(), formats.end(), preferredFormat) != formats.end()) {
            return preferredFormat;
        }
    }
    BAIL_ON_COND(formats.empty(), "no swapchain formats", 0);
    ALOGW("SurfaceSwapchain: neither GL_SRGB8_ALPHA8 nor GL_RGBA8 offered, using format {}",
          formats[0]);
    return formats[0];
}

} // anonymous namespace

SurfaceSwapchain::SurfaceSwapchain(const XrSession& session, JNIEnv* env, jobject activityObject,
                                   const uint32_t width, const uint32_t height)
    : mIsAndroidSurfaceSwapchain(OpenXr::GetExtensions().mAndroidSurfaceSwapchain &&
                                 OpenXr::GetExtensions().mCompositionLayerImageLayout),
      mEnv(env) {
    mSwapchain.mWidth = width;
    mSwapchain.mHeight = height;
    if (mIsAndroidSurfaceSwapchain) {
        CreateAndroidSurfaceSwapchain(session);
    } else {
        CreateSurfaceTextureSwapchain(session, activityObject);
    }
    ALOGI("SurfaceSwapchain: created {}x{} {} swapchain", width, height,
          mIsAndroidSurfaceSwapchain ? "surface-backed" : "SurfaceTexture-fed");
}

SurfaceSwapchain::~SurfaceSwapchain() {
    Shutdown();
}

void SurfaceSwapchain::CreateAndroidSurfaceSwapchain(const XrSession& session) {
    XrSwapchainCreateInfo xsci;
    memset(&xsci, 0, sizeof(xsci));
    xsci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    xsci.next = nullptr;
    xsci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    xsci.format = 0;
    xsci.sampleCount = 0;
    xsci.width = mSwapchain.mWidth;
    xsci.height = mSwapchain.mHeight;
    xsci.faceCount = 0;
    xsci.arraySize = 0;
    // Note: you can't have mips when you render directly to a
    // surface-backed swapchain. You just have to scale everything
    // so that you do not need them.
    xsci.mipCount = 0;

    PFN_xrCreateSwapchainAndroidSurfaceKHR pfnCreateSwapchainAndroidSurfaceKHR = nullptr;
    assert(OpenXr::GetInstance() != XR_NULL_HANDLE);
    XrResult xrResult =
        xrGetInstanceProcAddr(OpenXr::GetInstance(), "xrCreateSwapchainAndroidSurfaceKHR",
                              (PFN_xrVoidFunction*)(&pfnCreateSwapchainAndroidSurfaceKHR));
    if (xrResult != XR_SUCCESS || pfnCreateSwapchainAndroidSurfaceKHR == nullptr) {
        FAIL("xrGetInstanceProcAddr failed for "
             "xrCreateSwapchainAndroidSurfaceKHR");
    }

    OXR(pfnCreateSwapchainAndroidSurfaceKHR(session, &xsci, &mSwapchain.mHandle, &mSurface));
}

void SurfaceSwapchain::CreateSurfaceTextureSwapchain(const XrSession& session,
                                                     jobject activityObject) {
    // Swapchain
    {
        const int64_t format = ChooseSwapchainFormat(session);
        mIsSrgbSwapchain = (format == GL_SRGB8_ALPHA8);

        XrSwapchainCreateInfo xsci;
        memset(&xsci, 0, sizeof(xsci));
        xsci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        xsci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        xsci.format = format;
        xsci.sampleCount = 1;
        xsci.width = mSwapchain.mWidth;
        xsci.height = mSwapchain.mHeight;
        xsci.faceCount = 1;
        xsci.arraySize = 1;
        xsci.mipCount = 1;
        OXR(xrCreateSwapchain(session, &xsci, &mSwapchain.mHandle));

        uint32_t numImages = 0;
        OXR(xrEnumerateSwapchainImages(mSwapchain.mHandle, 0, &numImages, nullptr));
        mSwapchainImages.resize(numImages, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR, nullptr, 0});
        OXR(xrEnumerateSwapchainImages(mSwapchain.mHandle, numImages, &numImages,
                                       (XrSwapchainImageBaseHeader*)mSwapchainImages.data()));
    }

    CreateCopyResources();

    // SurfaceTexture and Surface. These are system classes, so FindClass() works
    // from this (non-main) thread.
    {
        jclass surfaceTextureClass = mEnv->FindClass("android/graphics/SurfaceTexture");
        CheckJniException(mEnv, "FindClass(SurfaceTexture)");
        const jmethodID surfaceTextureConstructor =
            mEnv->GetMethodID(surfaceTextureClass, "<init>", "(I)V");
        const jmethodID setDefaultBufferSizeMethodID =
            mEnv->GetMethodID(surfaceTextureClass, "setDefaultBufferSize", "(II)V");
        mUpdateTexImageMethodID = mEnv->GetMethodID(surfaceTextureClass, "updateTexImage", "()V");
        mGetTransformMatrixMethodID =
            mEnv->GetMethodID(surfaceTextureClass, "getTransformMatrix", "([F)V");
        CheckJniException(mEnv, "SurfaceTexture method lookup");

        jobject surfaceTexture = mEnv->NewObject(surfaceTextureClass, surfaceTextureConstructor,
                                                 static_cast<jint>(mExternalTexture));
        CheckJniException(mEnv, "new SurfaceTexture()");
        mEnv->CallVoidMethod(surfaceTexture, setDefaultBufferSizeMethodID,
                             static_cast<jint>(mSwapchain.mWidth),
                             static_cast<jint>(mSwapchain.mHeight));
        CheckJniException(mEnv, "SurfaceTexture.setDefaultBufferSize()");
        mSurfaceTexture = mEnv->NewGlobalRef(surfaceTexture);

        // An app class: FindClass() can't see it from this thread, so look it up
        // through the activity's class loader.
        jclass frameListenerClass = JniUtils::GetGlobalClassReference(
            mEnv, activityObject, "org/citra/citra_emu/vr/utils/SurfaceTextureFrameListener");
        if (frameListenerClass == nullptr) {
            FAIL("SurfaceSwapchain: SurfaceTextureFrameListener class not found");
        }
        const jmethodID frameListenerConstructor = mEnv->GetMethodID(
            frameListenerClass, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
        mGetFrameCountMethodID = mEnv->GetMethodID(frameListenerClass, "getFrameCount", "()J");
        CheckJniException(mEnv, "SurfaceTextureFrameListener method lookup");
        jobject frameListener =
            mEnv->NewObject(frameListenerClass, frameListenerConstructor, surfaceTexture);
        CheckJniException(mEnv, "new SurfaceTextureFrameListener()");
        mFrameListener = mEnv->NewGlobalRef(frameListener);
        mEnv->DeleteLocalRef(frameListener);
        mEnv->DeleteGlobalRef(frameListenerClass);

        jclass surfaceClass = mEnv->FindClass("android/view/Surface");
        const jmethodID surfaceConstructor =
            mEnv->GetMethodID(surfaceClass, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
        CheckJniException(mEnv, "Surface method lookup");
        jobject surface = mEnv->NewObject(surfaceClass, surfaceConstructor, surfaceTexture);
        CheckJniException(mEnv, "new Surface()");
        mSurface = mEnv->NewGlobalRef(surface);

        jfloatArray transformMatrixArray = mEnv->NewFloatArray(16);
        mTransformMatrixArray = static_cast<jfloatArray>(mEnv->NewGlobalRef(transformMatrixArray));

        mEnv->DeleteLocalRef(transformMatrixArray);
        mEnv->DeleteLocalRef(surface);
        mEnv->DeleteLocalRef(surfaceClass);
        mEnv->DeleteLocalRef(surfaceTexture);
        mEnv->DeleteLocalRef(surfaceTextureClass);
    }

    // A swapchain must have released an image before a layer can reference it,
    // and Android may not draw anything for a while.
    ClearSwapchainImage();
}

void SurfaceSwapchain::CreateCopyResources() {
    glGenTextures(1, &mExternalTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mExternalTexture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    mProgram = LinkProgram(kVertexShader, kFragmentShader);
    mTexMatrixUniform = glGetUniformLocation(mProgram, "uTexMatrix");
    mDecodeSrgbUniform = glGetUniformLocation(mProgram, "uDecodeSrgb");
    glUseProgram(mProgram);
    glUniform1i(glGetUniformLocation(mProgram, "uTexture"), 0);
    glUseProgram(0);

    glGenVertexArrays(1, &mVertexArray);
    glGenBuffers(1, &mVertexBuffer);
    glBindVertexArray(mVertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kFullScreenQuad), kFullScreenQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    mFramebuffers.resize(mSwapchainImages.size(), 0);
    glGenFramebuffers(static_cast<GLsizei>(mFramebuffers.size()), mFramebuffers.data());
    for (size_t i = 0; i < mFramebuffers.size(); i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffers[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               mSwapchainImages[i].image, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            FAIL("SurfaceSwapchain: incomplete framebuffer (0x%x)", status);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SurfaceSwapchain::ClearSwapchainImage() {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, nullptr};
    OXR(xrAcquireSwapchainImage(mSwapchain.mHandle, &acquireInfo, &index));
    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr,
                                         XR_INFINITE_DURATION};
    OXR(xrWaitSwapchainImage(mSwapchain.mHandle, &waitInfo));

    glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffers[index]);
    glViewport(0, 0, mSwapchain.mWidth, mSwapchain.mHeight);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, nullptr};
    OXR(xrReleaseSwapchainImage(mSwapchain.mHandle, &releaseInfo));
}

void SurfaceSwapchain::Update() {
    if (mIsAndroidSurfaceSwapchain) {
        return;
    }

    // Latch every buffer Android queued since the last call (each updateTexImage()
    // latches one), then copy the newest. Frames are counted by a listener because
    // buffer timestamps are not set by every producer.
    const jlong frameCount = mEnv->CallLongMethod(mFrameListener, mGetFrameCountMethodID);
    if (frameCount == mLatchedFrameCount) {
        return;
    }
    while (mLatchedFrameCount < frameCount) {
        mEnv->CallVoidMethod(mSurfaceTexture, mUpdateTexImageMethodID);
        if (mEnv->ExceptionCheck()) {
            mEnv->ExceptionDescribe();
            mEnv->ExceptionClear();
            ALOGE("SurfaceSwapchain: exception in SurfaceTexture.updateTexImage()");
            return;
        }
        mLatchedFrameCount++;
    }

    float transformMatrix[16];
    mEnv->CallVoidMethod(mSurfaceTexture, mGetTransformMatrixMethodID, mTransformMatrixArray);
    mEnv->GetFloatArrayRegion(mTransformMatrixArray, 0, 16, transformMatrix);

    CopySurfaceTextureToSwapchain(transformMatrix);
    if (!mHasCopiedFrame) {
        mHasCopiedFrame = true;
        ALOGI("SurfaceSwapchain: first frame copied into the {}x{} swapchain", mSwapchain.mWidth,
              mSwapchain.mHeight);
    }
}

void SurfaceSwapchain::CopySurfaceTextureToSwapchain(const float transformMatrix[16]) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, nullptr};
    OXR(xrAcquireSwapchainImage(mSwapchain.mHandle, &acquireInfo, &index));
    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr,
                                         XR_INFINITE_DURATION};
    OXR(xrWaitSwapchainImage(mSwapchain.mHandle, &waitInfo));

    glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffers[index]);
    glViewport(0, 0, mSwapchain.mWidth, mSwapchain.mHeight);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(mProgram);
    glUniformMatrix4fv(mTexMatrixUniform, 1, GL_FALSE, transformMatrix);
    glUniform1i(mDecodeSrgbUniform, mIsSrgbSwapchain ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mExternalTexture);
    glBindVertexArray(mVertexArray);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    glUseProgram(0);
    DumpBoundFramebufferIfRequested(transformMatrix);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, nullptr};
    OXR(xrReleaseSwapchainImage(mSwapchain.mHandle, &releaseInfo));
}

// Debugging aid: with `setprop debug.citra.dump_swapchains 1`, writes what was just
// copied into each swapchain (raw RGBA rows as glReadPixels returns them, bottom
// row first) to the app's files directory, at most once per second per swapchain.
void SurfaceSwapchain::DumpBoundFramebufferIfRequested(const float transformMatrix[16]) {
    if (SyspropUtils::GetSysPropAsInt("debug.citra.dump_swapchains", 0) == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - mLastDumpTime < std::chrono::seconds(1)) {
        return;
    }
    mLastDumpTime = now;
    std::vector<uint8_t> pixels(static_cast<size_t>(mSwapchain.mWidth) * mSwapchain.mHeight * 4);
    glReadPixels(0, 0, mSwapchain.mWidth, mSwapchain.mHeight, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels.data());
    const std::string path = fmt::format("/data/data/org.citra.citra_emu/files/swapchain_{}x{}.rgba",
                                         mSwapchain.mWidth, mSwapchain.mHeight);
    if (FILE* file = fopen(path.c_str(), "wb")) {
        fwrite(pixels.data(), 1, pixels.size(), file);
        fclose(file);
    }
    const float* m = transformMatrix;
    ALOGI("SurfaceSwapchain: dumped {} (srgb={}); texture matrix [{} {} {} {} | {} {} {} {} | {} "
          "{} {} {} | {} {} {} {}]",
          path, mIsSrgbSwapchain, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9],
          m[10], m[11], m[12], m[13], m[14], m[15]);
}

const void* SurfaceSwapchain::GetLayerNext() const {
    return mIsAndroidSurfaceSwapchain ? &kVerticalFlipLayout : nullptr;
}

XrRect2Di SurfaceSwapchain::ToImageRect(const int32_t x, const int32_t y, const int32_t width,
                                        const int32_t height) const {
    if (mIsAndroidSurfaceSwapchain) {
        // The surface-backed image keeps Android's row order; the layer's
        // vertical flip makes it display upright.
        return XrRect2Di{{x, y}, {width, height}};
    }
    // The copy is upright in GL convention, where row 0 is the bottom row.
    return XrRect2Di{{x, static_cast<int32_t>(mSwapchain.mHeight) - (y + height)}, {width, height}};
}

void SurfaceSwapchain::Shutdown() {
    if (mSurface != nullptr && !mIsAndroidSurfaceSwapchain) {
        jclass surfaceClass = mEnv->GetObjectClass(mSurface);
        const jmethodID releaseMethod = mEnv->GetMethodID(surfaceClass, "release", "()V");
        mEnv->CallVoidMethod(mSurface, releaseMethod);
        mEnv->DeleteLocalRef(surfaceClass);
        mEnv->DeleteGlobalRef(mSurface);
    }
    mSurface = nullptr;
    if (mSurfaceTexture != nullptr) {
        jclass surfaceTextureClass = mEnv->GetObjectClass(mSurfaceTexture);
        const jmethodID releaseMethod = mEnv->GetMethodID(surfaceTextureClass, "release", "()V");
        mEnv->CallVoidMethod(mSurfaceTexture, releaseMethod);
        mEnv->DeleteLocalRef(surfaceTextureClass);
        mEnv->DeleteGlobalRef(mSurfaceTexture);
        mSurfaceTexture = nullptr;
    }
    if (mFrameListener != nullptr) {
        mEnv->DeleteGlobalRef(mFrameListener);
        mFrameListener = nullptr;
    }
    if (mTransformMatrixArray != nullptr) {
        mEnv->DeleteGlobalRef(mTransformMatrixArray);
        mTransformMatrixArray = nullptr;
    }
    if (mEnv->ExceptionCheck()) {
        mEnv->ExceptionDescribe();
        mEnv->ExceptionClear();
    }

    if (!mFramebuffers.empty()) {
        glDeleteFramebuffers(static_cast<GLsizei>(mFramebuffers.size()), mFramebuffers.data());
        mFramebuffers.clear();
    }
    if (mVertexBuffer != 0) {
        glDeleteBuffers(1, &mVertexBuffer);
    }
    if (mVertexArray != 0) {
        glDeleteVertexArrays(1, &mVertexArray);
    }
    if (mProgram != 0) {
        glDeleteProgram(mProgram);
    }
    if (mExternalTexture != 0) {
        glDeleteTextures(1, &mExternalTexture);
    }
    mVertexBuffer = mVertexArray = mProgram = mExternalTexture = 0;

    if (mSwapchain.mHandle != XR_NULL_HANDLE) {
        xrDestroySwapchain(mSwapchain.mHandle);
        mSwapchain.mHandle = XR_NULL_HANDLE;
    }
    mSwapchain.mWidth = 0;
    mSwapchain.mHeight = 0;
}
