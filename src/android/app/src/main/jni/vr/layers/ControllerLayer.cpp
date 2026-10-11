/*******************************************************************************

Filename    :   ControllerLayer.cpp

Content     :   Draws the controllers and the pointer ray. See
                ControllerLayer.h.

Authors     :   CitraVR contributors
License     :   Licensed under GPLv3 or any later version.
                Refer to the license.txt file included.

*******************************************************************************/

#include "ControllerLayer.h"

#include "../utils/Common.h"
#include "../utils/LogUtils.h"
#include "../utils/XrMath.h"

#include <json.hpp>
#include <lodepng.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {

// Where Lepton mounts the SteamVR runtime, and the Steam Frame controllers' render
// models in it (SteamVR's IVRRenderModels reads the same files).
constexpr char kRenderModelDirectoryFormat[] =
    "/data/steamvr/runtime/drivers/frame_controller/resources/rendermodels/frame_controller_%s/";

// Size of each controller impostor (per hand and eye), in pixels.
constexpr int32_t kImpostorSize = 384;

// Pointer ray look.
constexpr uint32_t kRayTextureWidth = 64;
constexpr uint32_t kRayTextureHeight = 8;
constexpr float kRayWidthInMeters = 0.004f;
constexpr float kRayColor[3] = {0.55f, 0.85f, 1.0f};
constexpr float kRayAlpha = 0.85f;

constexpr char kModelVertexShader[] = R"(#version 300 es
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
uniform mat4 uMvp;
uniform mat3 uRotation;
out vec3 vNormal;
out vec2 vUv;
void main() {
    vNormal = uRotation * aNormal;
    vUv = aUv;
    gl_Position = uMvp * vec4(aPosition, 1.0);
}
)";

constexpr char kModelFragmentShader[] = R"(#version 300 es
precision mediump float;
uniform sampler2D uTexture;
in vec3 vNormal;
in vec2 vUv;
out vec4 outColor;
void main() {
    // A soft key light from above and in front, plus ambient.
    vec3 lightDirection = normalize(vec3(0.3, 1.0, 0.6));
    float light = 0.45 + 0.55 * max(dot(normalize(vNormal), lightDirection), 0.0);
    outColor = vec4(texture(uTexture, vUv).rgb * light, 1.0);
}
)";

GLuint CompileShader(const GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint isCompiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &isCompiled);
    if (isCompiled != GL_TRUE) {
        char infoLog[1024] = {};
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        FAIL("ControllerLayer: shader compilation failed: %s", infoLog);
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
        FAIL("ControllerLayer: program link failed: %s", infoLog);
    }
    return program;
}

//============================
// Column-major 4x4 matrices.

void Multiply(const float a[16], const float b[16], float result[16]) {
    float r[16];
    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) {
            r[column * 4 + row] = a[0 * 4 + row] * b[column * 4 + 0] +
                                  a[1 * 4 + row] * b[column * 4 + 1] +
                                  a[2 * 4 + row] * b[column * 4 + 2] +
                                  a[3 * 4 + row] * b[column * 4 + 3];
        }
    }
    std::copy(r, r + 16, result);
}

void FromPose(const XrPosef& pose, float result[16]) {
    XrMath::Quatf::ToRotationMatrix(pose.orientation, result);
    result[12] = pose.position.x;
    result[13] = pose.position.y;
    result[14] = pose.position.z;
    result[15] = 1.0f;
}

XrPosef Compose(const XrPosef& a, const XrPosef& b) {
    return XrPosef{a.orientation * b.orientation, XrMath::Posef::Transform(a, b.position)};
}

//============================
// Render model files.

std::string ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

// The pose of a render model component (e.g. "openxr_aim") in model space, from
// the model's JSON: `component_local` holds its origin and its rotation, as
// X, Y and Z angles in degrees.
bool ReadComponentPose(const std::string& jsonPath, const char* component, XrPosef& pose) {
    const std::string text = ReadFile(jsonPath);
    if (text.empty()) {
        return false;
    }
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.contains("components") ||
        !json["components"].contains(component)) {
        return false;
    }
    const nlohmann::json& local = json["components"][component]["component_local"];
    const auto origin = local.value("origin", std::vector<float>{0.0f, 0.0f, 0.0f});
    const auto rotation = local.value("rotate_xyz", std::vector<float>{0.0f, 0.0f, 0.0f});
    if (origin.size() != 3 || rotation.size() != 3) {
        return false;
    }
    constexpr float kRadiansPerDegree = static_cast<float>(M_PI / 180.0);
    const XrQuaternionf rotateX =
        XrMath::Quatf::FromAxisAngle({1.0f, 0.0f, 0.0f}, rotation[0] * kRadiansPerDegree);
    const XrQuaternionf rotateY =
        XrMath::Quatf::FromAxisAngle({0.0f, 1.0f, 0.0f}, rotation[1] * kRadiansPerDegree);
    const XrQuaternionf rotateZ =
        XrMath::Quatf::FromAxisAngle({0.0f, 0.0f, 1.0f}, rotation[2] * kRadiansPerDegree);
    pose.orientation = rotateZ * rotateY * rotateX;
    pose.position = {origin[0], origin[1], origin[2]};
    return true;
}

// Reads a triangulated (or polygonal) OBJ with positions, normals and texture
// coordinates into interleaved vertices (position, normal, uv) and indices.
bool ReadObj(const std::string& path, std::vector<float>& vertices,
             std::vector<uint32_t>& indices) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::vector<XrVector3f> positions;
    std::vector<XrVector3f> normals;
    std::vector<XrVector2f> uvs;
    std::unordered_map<std::string, uint32_t> vertexIndices;
    std::string line;
    while (std::getline(file, line)) {
        if (line.size() < 2) {
            continue;
        }
        std::istringstream stream(line);
        std::string type;
        stream >> type;
        if (type == "v") {
            XrVector3f p{};
            stream >> p.x >> p.y >> p.z;
            positions.push_back(p);
        } else if (type == "vn") {
            XrVector3f n{};
            stream >> n.x >> n.y >> n.z;
            normals.push_back(n);
        } else if (type == "vt") {
            XrVector2f uv{};
            stream >> uv.x >> uv.y;
            uvs.push_back(uv);
        } else if (type == "f") {
            std::vector<uint32_t> face;
            std::string corner;
            while (stream >> corner) {
                const auto found = vertexIndices.find(corner);
                if (found != vertexIndices.end()) {
                    face.push_back(found->second);
                    continue;
                }
                // "v/vt/vn", "v//vn" or "v/vt" (1-based).
                int v = 0, vt = 0, vn = 0;
                if (std::sscanf(corner.c_str(), "%d/%d/%d", &v, &vt, &vn) != 3 &&
                    std::sscanf(corner.c_str(), "%d//%d", &v, &vn) != 2) {
                    std::sscanf(corner.c_str(), "%d/%d", &v, &vt);
                }
                if (v < 1 || v > static_cast<int>(positions.size())) {
                    return false;
                }
                const XrVector3f& p = positions[v - 1];
                const XrVector3f n = (vn >= 1 && vn <= static_cast<int>(normals.size()))
                                         ? normals[vn - 1]
                                         : XrVector3f{0.0f, 1.0f, 0.0f};
                const XrVector2f uv = (vt >= 1 && vt <= static_cast<int>(uvs.size()))
                                          ? uvs[vt - 1]
                                          : XrVector2f{0.0f, 0.0f};
                const uint32_t index = static_cast<uint32_t>(vertices.size() / 8);
                // OBJ texture coordinates start at the bottom of the image, which
                // is uploaded top row first.
                vertices.insert(vertices.end(), {p.x, p.y, p.z, n.x, n.y, n.z, uv.x, 1.0f - uv.y});
                vertexIndices.emplace(corner, index);
                face.push_back(index);
            }
            for (size_t i = 2; i < face.size(); i++) {
                indices.insert(indices.end(), {face[0], face[i - 1], face[i]});
            }
        }
    }
    return !indices.empty();
}

// Symmetric OpenGL projection (clip z in [-1, 1]) through a square of half-size
// `halfExtent` at `distance` in front of the camera.
void SquareProjection(const float halfExtent, const float distance, const float nearZ,
                      const float farZ, float result[16]) {
    std::fill(result, result + 16, 0.0f);
    result[0] = distance / halfExtent;
    result[5] = distance / halfExtent;
    result[10] = -(farZ + nearZ) / (farZ - nearZ);
    result[11] = -1.0f;
    result[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

// Runtimes reject layer poses whose orientation isn't a unit quaternion.
XrQuaternionf Normalized(const XrQuaternionf& q) {
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return length > 0.0f ? XrQuaternionf{q.x / length, q.y / length, q.z / length, q.w / length}
                         : XrMath::Quatf::Identity();
}

bool IsOrientationValid(const XrSpaceLocation& location) {
    return (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
}

// An orientation whose +Z axis is `forward` and whose +Y axis is as close to
// `up` as possible.
XrQuaternionf LookRotation(const XrVector3f& forward, const XrVector3f& up) {
    const XrVector3f z = XrMath::Vector3f::Normalized(forward);
    XrVector3f x = XrMath::Vector3f::Cross(up, z);
    if (XrMath::Vector3f::LengthSq(x) < 1e-8f) {
        x = XrMath::Vector3f::Cross(XrVector3f{0.0f, 0.0f, 1.0f}, z);
    }
    x = XrMath::Vector3f::Normalized(x);
    const XrVector3f y = XrMath::Vector3f::Cross(z, x);
    return Normalized(XrMath::Quatf::FromThreeVectors(z, y, x));
}

} // anonymous namespace

ControllerLayer::ControllerLayer(const XrSession& session) {
    uint32_t numFormats = 0;
    OXR(xrEnumerateSwapchainFormats(session, 0, &numFormats, nullptr));
    std::vector<int64_t> formats(numFormats);
    OXR(xrEnumerateSwapchainFormats(session, numFormats, &numFormats, formats.data()));
    mIsSrgb = std::find(formats.begin(), formats.end(), GL_SRGB8_ALPHA8) != formats.end();
    mColorFormat = mIsSrgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;

    // Controller impostors: hands across, eyes up.
    {
        XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = mColorFormat;
        info.sampleCount = 1;
        info.width = kImpostorSize * 2;
        info.height = kImpostorSize * 2;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        OXR(xrCreateSwapchain(session, &info, &mImpostorSwapchain));
        uint32_t numImages = 0;
        OXR(xrEnumerateSwapchainImages(mImpostorSwapchain, 0, &numImages, nullptr));
        mImpostorImages.resize(numImages, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        OXR(xrEnumerateSwapchainImages(mImpostorSwapchain, numImages, &numImages,
                                       (XrSwapchainImageBaseHeader*)mImpostorImages.data()));
    }
    glGenRenderbuffers(1, &mDepthRenderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, mDepthRenderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kImpostorSize * 2,
                          kImpostorSize * 2);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glGenFramebuffers(1, &mFramebuffer);

    mSolidRaySwapchain = CreateRayTexture(session, false);
    mFadingRaySwapchain = CreateRayTexture(session, true);

    mModelProgram = LinkProgram(kModelVertexShader, kModelFragmentShader);
    mModelMvpUniform = glGetUniformLocation(mModelProgram, "uMvp");
    mModelRotationUniform = glGetUniformLocation(mModelProgram, "uRotation");
    glUseProgram(mModelProgram);
    glUniform1i(glGetUniformLocation(mModelProgram, "uTexture"), 0);
    glUseProgram(0);

    // The models take a moment to read and decode: do it off the render thread,
    // and upload them once they are ready.
    mModelsLoading = std::async(std::launch::async, [] {
        return std::array<std::unique_ptr<ModelData>, 2>{LoadModel("left"), LoadModel("right")};
    });
}

ControllerLayer::~ControllerLayer() {
    if (mModelsLoading.valid()) {
        mModelsLoading.wait();
    }
    for (Model& model : mModels) {
        glDeleteVertexArrays(1, &model.mVertexArray);
        glDeleteBuffers(1, &model.mVertexBuffer);
        glDeleteBuffers(1, &model.mIndexBuffer);
        glDeleteTextures(1, &model.mTexture);
    }
    glDeleteProgram(mModelProgram);
    glDeleteFramebuffers(1, &mFramebuffer);
    glDeleteRenderbuffers(1, &mDepthRenderbuffer);
    for (const XrSwapchain swapchain : {mImpostorSwapchain, mSolidRaySwapchain, mFadingRaySwapchain}) {
        if (swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(swapchain);
        }
    }
}

XrSwapchain ControllerLayer::CreateRayTexture(const XrSession& session, const bool fades) {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = mColorFormat;
    info.sampleCount = 1;
    info.width = kRayTextureWidth;
    info.height = kRayTextureHeight;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    OXR(xrCreateSwapchain(session, &info, &swapchain));
    uint32_t numImages = 0;
    OXR(xrEnumerateSwapchainImages(swapchain, 0, &numImages, nullptr));
    std::vector<XrSwapchainImageOpenGLESKHR> images(numImages,
                                                    {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    OXR(xrEnumerateSwapchainImages(swapchain, numImages, &numImages,
                                   (XrSwapchainImageBaseHeader*)images.data()));

    // Premultiplied colour: the ray's colour, opaque in the middle of its width
    // and soft at its edges, and (if it fades) from opaque at the controller to
    // transparent at the far end. Left is the controller end.
    std::vector<uint8_t> pixels(kRayTextureWidth * kRayTextureHeight * 4);
    for (uint32_t y = 0; y < kRayTextureHeight; y++) {
        const float across = (y + 0.5f) / kRayTextureHeight * 2.0f - 1.0f;
        const float edge = std::clamp((1.0f - std::abs(across)) * 2.0f, 0.0f, 1.0f);
        for (uint32_t x = 0; x < kRayTextureWidth; x++) {
            const float along = (x + 0.5f) / kRayTextureWidth;
            const float alpha = kRayAlpha * edge * (fades ? 1.0f - along : 1.0f);
            uint8_t* pixel = &pixels[(y * kRayTextureWidth + x) * 4];
            for (int c = 0; c < 3; c++) {
                // Stored as sRGB when the swapchain is: premultiply in linear light.
                const float linear = kRayColor[c] * alpha;
                const float encoded =
                    mIsSrgb ? (linear <= 0.0031308f ? linear * 12.92f
                                                    : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f)
                            : linear;
                pixel[c] = static_cast<uint8_t>(std::lround(encoded * 255.0f));
            }
            pixel[3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));
        }
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    OXR(xrAcquireSwapchainImage(swapchain, &acquireInfo, &index));
    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr,
                                         XR_INFINITE_DURATION};
    OXR(xrWaitSwapchainImage(swapchain, &waitInfo));
    glBindTexture(GL_TEXTURE_2D, images[index].image);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kRayTextureWidth, kRayTextureHeight, GL_RGBA,
                    GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    OXR(xrReleaseSwapchainImage(swapchain, &releaseInfo));
    return swapchain;
}

std::unique_ptr<ControllerLayer::ModelData> ControllerLayer::LoadModel(const char* side) {
    char directory[256];
    std::snprintf(directory, sizeof(directory), kRenderModelDirectoryFormat, side);
    const std::string name = std::string("frame_controller_") + side;
    auto model = std::make_unique<ModelData>();

    XrPosef modelFromAim;
    if (!ReadComponentPose(directory + name + ".json", "openxr_aim", modelFromAim)) {
        ALOGW("ControllerLayer: no render model at {}", directory);
        return nullptr;
    }
    model->mAimFromModel = XrMath::Posef::Inverted(modelFromAim);
    if (!ReadObj(directory + name + ".obj", model->mVertices, model->mIndices)) {
        ALOGW("ControllerLayer: could not read {}{}.obj", directory, name);
        return nullptr;
    }
    unsigned width = 0, height = 0;
    if (lodepng::decode(model->mTextureRgba, width, height, directory + name + "_color.png") !=
        0) {
        ALOGW("ControllerLayer: could not read {}{}_color.png", directory, name);
        return nullptr;
    }
    model->mTextureWidth = width;
    model->mTextureHeight = height;

    // Bounding sphere: the centre of the bounding box, and the farthest vertex.
    XrVector3f minimum{1e9f, 1e9f, 1e9f};
    XrVector3f maximum{-1e9f, -1e9f, -1e9f};
    for (size_t i = 0; i < model->mVertices.size(); i += 8) {
        const float* p = &model->mVertices[i];
        minimum = {std::min(minimum.x, p[0]), std::min(minimum.y, p[1]), std::min(minimum.z, p[2])};
        maximum = {std::max(maximum.x, p[0]), std::max(maximum.y, p[1]), std::max(maximum.z, p[2])};
    }
    model->mCenter = (minimum + maximum) * 0.5f;
    for (size_t i = 0; i < model->mVertices.size(); i += 8) {
        const float* p = &model->mVertices[i];
        model->mRadius = std::max(
            model->mRadius, XrMath::Vector3f::Length(XrVector3f{p[0], p[1], p[2]} - model->mCenter));
    }
    ALOGI("ControllerLayer: loaded {} ({} triangles, {}x{} texture, radius {} m)", name,
          model->mIndices.size() / 3, width, height, model->mRadius);
    return model;
}

void ControllerLayer::UploadModels() {
    if (!mModelsLoading.valid() ||
        mModelsLoading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    auto loaded = mModelsLoading.get();
    for (size_t i = 0; i < loaded.size(); i++) {
        const ModelData* data = loaded[i].get();
        if (data == nullptr) {
            continue;
        }
        Model& model = mModels[i];
        model.mAimFromModel = data->mAimFromModel;
        model.mCenter = data->mCenter;
        model.mRadius = data->mRadius;
        model.mIndexCount = static_cast<GLsizei>(data->mIndices.size());

        glGenVertexArrays(1, &model.mVertexArray);
        glGenBuffers(1, &model.mVertexBuffer);
        glGenBuffers(1, &model.mIndexBuffer);
        glBindVertexArray(model.mVertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, model.mVertexBuffer);
        glBufferData(GL_ARRAY_BUFFER, data->mVertices.size() * sizeof(float),
                     data->mVertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model.mIndexBuffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, data->mIndices.size() * sizeof(uint32_t),
                     data->mIndices.data(), GL_STATIC_DRAW);
        constexpr GLsizei kStride = 8 * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kStride,
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, kStride,
                              reinterpret_cast<void*>(6 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        glGenTextures(1, &model.mTexture);
        glBindTexture(GL_TEXTURE_2D, model.mTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, mIsSrgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, data->mTextureWidth,
                     data->mTextureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     data->mTextureRgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void ControllerLayer::DrawModel(const Model& model, const XrPosef& aimPose,
                                const float viewProjection[16]) {
    const XrPosef worldFromModel = Compose(aimPose, model.mAimFromModel);
    float worldFromModelMatrix[16];
    FromPose(worldFromModel, worldFromModelMatrix);
    float mvp[16];
    Multiply(viewProjection, worldFromModelMatrix, mvp);
    const float rotation[9] = {
        worldFromModelMatrix[0], worldFromModelMatrix[1], worldFromModelMatrix[2],
        worldFromModelMatrix[4], worldFromModelMatrix[5], worldFromModelMatrix[6],
        worldFromModelMatrix[8], worldFromModelMatrix[9], worldFromModelMatrix[10]};

    glUseProgram(mModelProgram);
    glUniformMatrix4fv(mModelMvpUniform, 1, GL_FALSE, mvp);
    glUniformMatrix3fv(mModelRotationUniform, 1, GL_FALSE, rotation);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, model.mTexture);
    glBindVertexArray(model.mVertexArray);
    glDrawElements(GL_TRIANGLES, model.mIndexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ControllerLayer::Frame(const XrSession& session, const XrSpace& space,
                            const XrTime predictedDisplayTime, const InputStateFrame& input,
                            const bool showControllers, const PointerRay& ray,
                            std::vector<XrCompositionLayer>& layers, uint32_t& layerCount) {
    UploadModels();

    bool drawModel[2] = {false, false};
    for (int hand = 0; hand < 2; hand++) {
        drawModel[hand] = showControllers && input.mIsHandActive[hand] &&
                          IsOrientationValid(input.mHandPositions[hand]) &&
                          mModels[hand].mIndexCount > 0;
    }
    if (!drawModel[0] && !drawModel[1] && !ray.mIsVisible) {
        return;
    }

    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = OpenXr::VIEW_CONFIG_TYPE;
    locateInfo.displayTime = predictedDisplayTime;
    locateInfo.space = space;
    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t numViews = 0;
    if (XR_FAILED(xrLocateViews(session, &locateInfo, &viewState, 2, &numViews, views)) ||
        numViews < 2 || (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
        return;
    }
    const XrVector3f headPosition = (views[0].pose.position + views[1].pose.position) * 0.5f;

    // Ray: a quad along the ray (its X axis), turned to face the head.
    if (ray.mIsVisible && ray.mLength > 0.0f && layerCount < layers.size() &&
        IsOrientationValid(input.mHandPositions[input.mPreferredHand])) {
        const XrVector3f direction =
            XrMath::Quatf::Rotate(ray.mAimPose.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
        const XrVector3f center = ray.mAimPose.position + direction * (ray.mLength * 0.5f);
        const XrVector3f toHead = headPosition - center;
        XrVector3f facing = toHead - direction * XrMath::Vector3f::Dot(toHead, direction);
        if (XrMath::Vector3f::LengthSq(facing) > 1e-8f) {
            facing = XrMath::Vector3f::Normalized(facing);
            const XrVector3f up = XrMath::Vector3f::Cross(facing, direction);
            XrCompositionLayerQuad layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            layer.space = space;
            layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            layer.subImage.swapchain = ray.mHitsPanel ? mSolidRaySwapchain : mFadingRaySwapchain;
            layer.subImage.imageRect = {{0, 0},
                                        {static_cast<int32_t>(kRayTextureWidth),
                                         static_cast<int32_t>(kRayTextureHeight)}};
            layer.pose = {Normalized(XrMath::Quatf::FromThreeVectors(facing, up, direction)),
                          center};
            layer.size = {ray.mLength, kRayWidthInMeters};
            layers[layerCount++].mQuad = layer;
        }
    }

    if (!drawModel[0] && !drawModel[1]) {
        return;
    }

    // Controllers: per hand and eye, a quad at the controller facing the eye,
    // with the model drawn as that eye sees it through the quad.
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    OXR(xrAcquireSwapchainImage(mImpostorSwapchain, &acquireInfo, &index));
    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr,
                                         XR_INFINITE_DURATION};
    OXR(xrWaitSwapchainImage(mImpostorSwapchain, &waitInfo));

    glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           mImpostorImages[index].image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              mDepthRenderbuffer);
    glViewport(0, 0, kImpostorSize * 2, kImpostorSize * 2);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    // No face culling: the models' winding order isn't something to rely on.
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    for (int hand = 0; hand < 2; hand++) {
        if (!drawModel[hand]) {
            continue;
        }
        const Model& model = mModels[hand];
        const XrPosef& aimPose = input.mHandPositions[hand].pose;
        const XrVector3f center =
            XrMath::Posef::Transform(Compose(aimPose, model.mAimFromModel), model.mCenter);
        // A little margin around the bounding sphere.
        const float halfExtent = model.mRadius * 1.05f;
        for (uint32_t eye = 0; eye < 2; eye++) {
            const XrVector3f eyePosition = views[eye].pose.position;
            const float distance = XrMath::Vector3f::Length(center - eyePosition);
            if (distance < halfExtent * 1.5f || layerCount >= layers.size()) {
                continue; // too close to draw this way
            }
            const XrQuaternionf orientation = LookRotation(eyePosition - center, {0.0f, 1.0f, 0.0f});

            // The eye looking through the quad: the quad fills its square view.
            float projection[16];
            SquareProjection(halfExtent, distance, std::max(0.005f, distance - halfExtent * 1.5f),
                             distance + halfExtent * 1.5f, projection);
            float view[16];
            FromPose(XrMath::Posef::Inverted(XrPosef{orientation, eyePosition}), view);
            float viewProjection[16];
            Multiply(projection, view, viewProjection);

            const int32_t x = hand * kImpostorSize;
            const int32_t y = eye * kImpostorSize;
            glViewport(x, y, kImpostorSize, kImpostorSize);
            DrawModel(model, aimPose, viewProjection);

            XrCompositionLayerQuad layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            layer.space = space;
            layer.eyeVisibility = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
            layer.subImage.swapchain = mImpostorSwapchain;
            layer.subImage.imageRect = {{x, y}, {kImpostorSize, kImpostorSize}};
            layer.pose = {orientation, center};
            layer.size = {halfExtent * 2.0f, halfExtent * 2.0f};
            layers[layerCount++].mQuad = layer;
        }
    }

    glDisable(GL_DEPTH_TEST);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    OXR(xrReleaseSwapchainImage(mImpostorSwapchain, &releaseInfo));
}
