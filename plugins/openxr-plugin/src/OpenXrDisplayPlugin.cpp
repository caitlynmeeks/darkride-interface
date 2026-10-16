//
//  OpenXrDisplayPlugin.cpp
//  plugins/openxr-plugin/src
//
//  Created on 2026-10-16 for Quest 3 hand tracking over Link (Meta's PC OpenXR runtime).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "OpenXrDisplayPlugin.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <gl/Config.h>
#include <gpu/Frame.h>
#include <gpu/gl/GLBackend.h>
#include <Profile.h>
#include <shared/NsightHelpers.h>
#include <ViewFrustum.h>

#if defined(Q_OS_WIN)
#undef near
#undef far
#endif

// The composite framebuffer is GL_SRGB8_ALPHA8, so an sRGB swapchain takes its bytes as they are.
static const int64_t PREFERRED_SWAPCHAIN_FORMAT = GL_SRGB8_ALPHA8;
static const float DEFAULT_TARGET_RATE = 90.0f;
// used until the runtime has reported each eye's field of view
static const float DEFAULT_HALF_FOV = 0.9f;
// how much wider than the eyes' combined field of view culling is
static const float CULLING_FOV_MARGIN = 1.05f;

static glm::mat4 projectionFromFov(const XrFovf& fov, float nearClip, float farClip) {
    return glm::frustum(tanf(fov.angleLeft) * nearClip, tanf(fov.angleRight) * nearClip,
                        tanf(fov.angleDown) * nearClip, tanf(fov.angleUp) * nearClip, nearClip, farClip);
}

static XrFovf defaultFov() {
    XrFovf fov;
    fov.angleLeft = -DEFAULT_HALF_FOV;
    fov.angleRight = DEFAULT_HALF_FOV;
    fov.angleDown = -DEFAULT_HALF_FOV;
    fov.angleUp = DEFAULT_HALF_FOV;
    return fov;
}

static void getClipPlanes(const glm::mat4& baseProjection, float& nearClip, float& farClip) {
    ViewFrustum baseFrustum;
    baseFrustum.setProjection(baseProjection);
    nearClip = baseFrustum.getNearClip();
    farClip = baseFrustum.getFarClip();
}

OpenXrDisplayPlugin::OpenXrDisplayPlugin(std::shared_ptr<OpenXrRuntime> runtime) : _runtime(runtime) {
    _viewConfigs.fill({ XR_TYPE_VIEW_CONFIGURATION_VIEW });
    _layerViews.fill({ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });
    _fov.fill(defaultFov());
}

bool OpenXrDisplayPlugin::isSupported() const {
    return _runtime->isSupported();
}

const QString OpenXrDisplayPlugin::getName() const {
    return "OpenXR";
}

void OpenXrDisplayPlugin::init() {
    Plugin::init();

    uint32_t viewCount { 0 };
    if (!_runtime->check(xrEnumerateViewConfigurationViews(_runtime->getInstance(), _runtime->getSystemId(),
                                                           XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, (uint32_t)_viewConfigs.size(),
                                                           &viewCount, _viewConfigs.data()),
                         "xrEnumerateViewConfigurationViews") ||
        viewCount != 2) {
        qCWarning(openxr) << "The headset doesn't have a stereo view configuration";
    }
    qCInfo(openxr) << "Recommended eye size:" << _viewConfigs[0].recommendedImageRectWidth << "x"
                   << _viewConfigs[0].recommendedImageRectHeight;

    emit deviceConnected(getName());
}

float OpenXrDisplayPlugin::getTargetFrameRate() const {
    XrDuration period = _runtime->getDisplayPeriod();
    return period > 0 ? (float)(1.0e9 / (double)period) : DEFAULT_TARGET_RATE;
}

glm::mat4 OpenXrDisplayPlugin::getEyeProjection(Eye eye, const glm::mat4& baseProjection) const {
    float nearClip, farClip;
    getClipPlanes(baseProjection, nearClip, farClip);
    std::lock_guard<std::mutex> lock(_fovMutex);
    return projectionFromFov(_fov[eye == Left ? 0 : 1], nearClip, farClip);
}

glm::mat4 OpenXrDisplayPlugin::getCullingProjection(const glm::mat4& baseProjection) const {
    float nearClip, farClip;
    getClipPlanes(baseProjection, nearClip, farClip);
    XrFovf combined;
    {
        std::lock_guard<std::mutex> lock(_fovMutex);
        combined.angleLeft = std::min(_fov[0].angleLeft, _fov[1].angleLeft) * CULLING_FOV_MARGIN;
        combined.angleRight = std::max(_fov[0].angleRight, _fov[1].angleRight) * CULLING_FOV_MARGIN;
        combined.angleDown = std::min(_fov[0].angleDown, _fov[1].angleDown) * CULLING_FOV_MARGIN;
        combined.angleUp = std::max(_fov[0].angleUp, _fov[1].angleUp) * CULLING_FOV_MARGIN;
    }
    return projectionFromFov(combined, nearClip, farClip);
}

bool OpenXrDisplayPlugin::internalActivate() {
    if (!_runtime->isSupported()) {
        return false;
    }

    // side by side, like the OpenVR plugin
    _renderTargetSize.x = _viewConfigs[0].recommendedImageRectWidth * 2;
    _renderTargetSize.y = _viewConfigs[0].recommendedImageRectHeight;

    withNonPresentThreadLock([&] {
        _eyeOffsets[0] = glm::translate(glm::mat4(), glm::vec3(-_ipd / 2.0f, 0.0f, 0.0f));
        _eyeOffsets[1] = glm::translate(glm::mat4(), glm::vec3(_ipd / 2.0f, 0.0f, 0.0f));
        _eyeProjections[0] = projectionFromFov(_fov[0], DEFAULT_NEAR_CLIP, DEFAULT_FAR_CLIP);
        _eyeProjections[1] = projectionFromFov(_fov[1], DEFAULT_NEAR_CLIP, DEFAULT_FAR_CLIP);
        _cullingProjection = _eyeProjections[0];
    });

    return Parent::internalActivate();
}

void OpenXrDisplayPlugin::internalDeactivate() {
    Parent::internalDeactivate();
}

void OpenXrDisplayPlugin::customizeContext() {
    // Display plugins in DLLs must initialize GL locally
    gl::initModuleGl();
    Parent::customizeContext();

    // the session uses the present thread's context, which is current here
    if (!_runtime->createSession() || !createSwapchains()) {
        qCWarning(openxr) << "Couldn't start OpenXR rendering";
    }
}

void OpenXrDisplayPlugin::uncustomizeContext() {
    if (_frameBegun) {
        endXrFrame(false);
    }
    destroySwapchains();
    _runtime->destroySession();
    _layerViewsValid = false;
    Parent::uncustomizeContext();
}

bool OpenXrDisplayPlugin::createSwapchains() {
    XrSession session = _runtime->getSession();

    uint32_t formatCount { 0 };
    if (!_runtime->check(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr), "xrEnumerateSwapchainFormats")) {
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    _runtime->check(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()), "xrEnumerateSwapchainFormats");
    if (std::find(formats.begin(), formats.end(), PREFERRED_SWAPCHAIN_FORMAT) == formats.end()) {
        qCWarning(openxr) << "The runtime has no GL_SRGB8_ALPHA8 swapchains";
        return false;
    }

    for (size_t eye = 0; eye < _swapchains.size(); eye++) {
        XrSwapchainCreateInfo createInfo { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        createInfo.format = PREFERRED_SWAPCHAIN_FORMAT;
        createInfo.sampleCount = 1;
        createInfo.width = _viewConfigs[eye].recommendedImageRectWidth;
        createInfo.height = _viewConfigs[eye].recommendedImageRectHeight;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;
        if (!_runtime->check(xrCreateSwapchain(session, &createInfo, &_swapchains[eye]), "xrCreateSwapchain")) {
            destroySwapchains();
            return false;
        }

        uint32_t imageCount { 0 };
        _runtime->check(xrEnumerateSwapchainImages(_swapchains[eye], 0, &imageCount, nullptr), "xrEnumerateSwapchainImages");
        _swapchainImages[eye].assign(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
        _runtime->check(xrEnumerateSwapchainImages(_swapchains[eye], imageCount, &imageCount,
                                                   (XrSwapchainImageBaseHeader*)_swapchainImages[eye].data()),
                        "xrEnumerateSwapchainImages");

        auto& layerView = _layerViews[eye];
        layerView = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        layerView.subImage.swapchain = _swapchains[eye];
        layerView.subImage.imageRect.offset = { 0, 0 };
        layerView.subImage.imageRect.extent = { (int32_t)createInfo.width, (int32_t)createInfo.height };
        layerView.subImage.imageArrayIndex = 0;
    }
    return true;
}

void OpenXrDisplayPlugin::destroySwapchains() {
    for (size_t eye = 0; eye < _swapchains.size(); eye++) {
        if (_swapchains[eye] != XR_NULL_HANDLE) {
            xrDestroySwapchain(_swapchains[eye]);
            _swapchains[eye] = XR_NULL_HANDLE;
        }
        _swapchainImages[eye].clear();
    }
}

// Main thread: the head pose this frame is simulated and rendered with.
bool OpenXrDisplayPlugin::beginFrameRender(uint32_t frameIndex) {
    PROFILE_RANGE_EX(render, __FUNCTION__, 0xff7fff00, frameIndex)
    _currentRenderFrameInfo = FrameInfo();

    XrTime displayTime = _runtime->getNextDisplayTime();
    controller::Pose head = _runtime->locate(_runtime->getViewSpace(), displayTime);
    if (head.isValid()) {
        _currentRenderFrameInfo.renderPose = head.getMatrix();
    } else {
        // before the first frame, stand at a typical eye height
        _currentRenderFrameInfo.renderPose = glm::translate(glm::mat4(), glm::vec3(0.0f, 1.7f, 0.0f));
    }
    _currentRenderFrameInfo.predictedDisplayTime = (double)displayTime / 1.0e9;

    withNonPresentThreadLock([&] { _frameInfos[frameIndex] = _currentRenderFrameInfo; });
    return Parent::beginFrameRender(frameIndex);
}

bool OpenXrDisplayPlugin::beginXrFrame() {
    if (_frameBegun) {
        endXrFrame(false);
    }
    if (!_runtime->isSessionRunning() || _swapchains[0] == XR_NULL_HANDLE) {
        // xrWaitFrame paces the present thread; without it, don't spin
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return false;
    }

    XrSession session = _runtime->getSession();
    XrFrameWaitInfo waitInfo { XR_TYPE_FRAME_WAIT_INFO };
    _frameState = { XR_TYPE_FRAME_STATE };
    if (!_runtime->check(xrWaitFrame(session, &waitInfo, &_frameState), "xrWaitFrame")) {
        return false;
    }
    _runtime->setFrameTiming(_frameState.predictedDisplayTime, _frameState.predictedDisplayPeriod);

    XrFrameBeginInfo beginInfo { XR_TYPE_FRAME_BEGIN_INFO };
    if (!_runtime->check(xrBeginFrame(session, &beginInfo), "xrBeginFrame")) {
        return false;
    }
    _frameBegun = true;
    return true;
}

void OpenXrDisplayPlugin::endXrFrame(bool submitLayer) {
    XrCompositionLayerProjection layer { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = _runtime->getReferenceSpace();
    layer.viewCount = (uint32_t)_layerViews.size();
    layer.views = _layerViews.data();
    const XrCompositionLayerBaseHeader* layers[] = { (const XrCompositionLayerBaseHeader*)&layer };

    XrFrameEndInfo endInfo { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = _frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = submitLayer ? 1 : 0;
    endInfo.layers = submitLayer ? layers : nullptr;
    _runtime->check(xrEndFrame(_runtime->getSession(), &endInfo), "xrEndFrame");
    _frameBegun = false;
}

// Present thread: begins the OpenXR frame and takes this frame's predicted head and eye poses.
void OpenXrDisplayPlugin::updatePresentPose() {
    Parent::updatePresentPose();

    if (!_runtime->pollEvents()) {
        qCWarning(openxr) << "The OpenXR session ended; switch to another display";
    }
    if (!_currentFrame || !beginXrFrame()) {
        return;
    }

    XrTime displayTime = _frameState.predictedDisplayTime;
    XrViewLocateInfo locateInfo { XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = displayTime;
    locateInfo.space = _runtime->getReferenceSpace();
    XrViewState viewState { XR_TYPE_VIEW_STATE };
    std::array<XrView, 2> views;
    views.fill({ XR_TYPE_VIEW });
    uint32_t viewCount { 0 };
    if (!_runtime->check(xrLocateViews(_runtime->getSession(), &locateInfo, &viewState, (uint32_t)views.size(), &viewCount,
                                       views.data()),
                         "xrLocateViews") ||
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
        _layerViewsValid = false;
        return;
    }

    controller::Pose head = _runtime->locate(_runtime->getViewSpace(), displayTime);
    if (!head.isValid()) {
        _layerViewsValid = false;
        return;
    }
    glm::mat4 headMat = head.getMatrix();
    _currentPresentFrameInfo.presentPose = headMat;

    if (!_haveFov) {
        // The eyes' offsets and fields of view don't change, so the first located views set them.
        glm::mat4 headInverse = glm::inverse(headMat);
        {
            std::lock_guard<std::mutex> lock(_fovMutex);
            _fov[0] = views[0].fov;
            _fov[1] = views[1].fov;
            _haveFov = true;
        }
        withPresentThreadLock([&] {
            for (int eye = 0; eye < 2; eye++) {
                controller::Pose eyePose(toGlm(views[eye].pose.position), toGlm(views[eye].pose.orientation));
                _eyeOffsets[eye] = headInverse * eyePose.getMatrix();
                _eyeProjections[eye] = projectionFromFov(_fov[eye], DEFAULT_NEAR_CLIP, DEFAULT_FAR_CLIP);
            }
            _ipd = glm::length(glm::vec3(_eyeOffsets[1][3]) - glm::vec3(_eyeOffsets[0][3]));
        });
        qCDebug(openxr) << "IPD" << _ipd;
    }

    // The composite is corrected to the present pose, so the layer goes out with this frame's eye poses. The
    // field of view stays the one the frame was rendered with.
    for (size_t eye = 0; eye < _layerViews.size(); eye++) {
        _layerViews[eye].pose = views[eye].pose;
        _layerViews[eye].fov = _fov[eye];
    }
    _layerViewsValid = true;
}

void OpenXrDisplayPlugin::hmdPresent() {
    PROFILE_RANGE_EX(render, __FUNCTION__, 0xff00ff00, (uint64_t)_currentFrame->frameIndex)
    if (!_frameBegun) {
        return;
    }

    bool submitLayer = false;
    if (_frameState.shouldRender && _layerViewsValid) {
        GLuint compositeTexture = getGLBackend()->getTextureID(_compositeFramebuffer->getRenderBuffer(0));
        submitLayer = true;
        for (size_t eye = 0; eye < _swapchains.size(); eye++) {
            uint32_t imageIndex { 0 };
            XrSwapchainImageAcquireInfo acquireInfo { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (!_runtime->check(xrAcquireSwapchainImage(_swapchains[eye], &acquireInfo, &imageIndex), "xrAcquireSwapchainImage")) {
                submitLayer = false;
                break;
            }
            XrSwapchainImageWaitInfo waitInfo { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            if (_runtime->check(xrWaitSwapchainImage(_swapchains[eye], &waitInfo), "xrWaitSwapchainImage")) {
                const auto& extent = _layerViews[eye].subImage.imageRect.extent;
                glCopyImageSubData(compositeTexture, GL_TEXTURE_2D, 0, (GLint)eye * extent.width, 0, 0,
                                   _swapchainImages[eye][imageIndex].image, GL_TEXTURE_2D, 0, 0, 0, 0,
                                   extent.width, extent.height, 1);
            } else {
                submitLayer = false;
            }
            XrSwapchainImageReleaseInfo releaseInfo { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            _runtime->check(xrReleaseSwapchainImage(_swapchains[eye], &releaseInfo), "xrReleaseSwapchainImage");
        }
    }

    endXrFrame(submitLayer);
    if (submitLayer) {
        _presentRate.increment();
    }
}

bool OpenXrDisplayPlugin::isHmdMounted() const {
    return _runtime->isUserPresent();
}

void OpenXrDisplayPlugin::postPreview() {
    if (isHmdMounted() != _hmdMounted) {
        _hmdMounted = !_hmdMounted;
        emit hmdMountedChanged();
    }
}
