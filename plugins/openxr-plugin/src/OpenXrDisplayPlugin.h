//
//  OpenXrDisplayPlugin.h
//  plugins/openxr-plugin/src
//
//  Created on 2026-10-16 for Quest 3 hand tracking over Link (Meta's PC OpenXR runtime).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_OpenXrDisplayPlugin_h
#define hifi_OpenXrDisplayPlugin_h

#include <memory>
#include <mutex>
#include <vector>

#include <display-plugins/hmd/HmdDisplayPlugin.h>

#include "OpenXrRuntime.h"

// Renders side by side into the composite framebuffer, as the OpenVR plugin does, and copies each half into that eye's
// OpenXR swapchain image. The OpenXR frame (xrWaitFrame, xrBeginFrame, xrEndFrame) runs on the present thread: it
// begins in updatePresentPose(), so the present pose is the one the runtime predicts for this frame, and ends in
// hmdPresent().
class OpenXrDisplayPlugin : public HmdDisplayPlugin {
    using Parent = HmdDisplayPlugin;

public:
    OpenXrDisplayPlugin(std::shared_ptr<OpenXrRuntime> runtime);

    bool isSupported() const override;
    const QString getName() const override;
    bool getSupportsAutoSwitch() override final { return false; }

    glm::mat4 getEyeProjection(Eye eye, const glm::mat4& baseProjection) const override;
    glm::mat4 getCullingProjection(const glm::mat4& baseProjection) const override;

    void init() override;

    float getTargetFrameRate() const override;
    bool hasAsyncReprojection() const override { return true; }

    void customizeContext() override;
    void uncustomizeContext() override;

    bool beginFrameRender(uint32_t frameIndex) override;

protected:
    bool internalActivate() override;
    void internalDeactivate() override;
    void updatePresentPose() override;

    void hmdPresent() override;
    bool isHmdMounted() const override;
    void postPreview() override;

private:
    bool createSwapchains();
    void destroySwapchains();
    bool beginXrFrame();
    void endXrFrame(bool submitLayer);

    std::shared_ptr<OpenXrRuntime> _runtime;

    std::array<XrViewConfigurationView, 2> _viewConfigs;
    std::array<XrSwapchain, 2> _swapchains {{ XR_NULL_HANDLE, XR_NULL_HANDLE }};
    std::array<std::vector<XrSwapchainImageOpenGLKHR>, 2> _swapchainImages;
    std::array<XrCompositionLayerProjectionView, 2> _layerViews;

    // the field of view of each eye, from the first located views
    mutable std::mutex _fovMutex;
    bool _haveFov { false };
    std::array<XrFovf, 2> _fov;

    XrFrameState _frameState { XR_TYPE_FRAME_STATE };
    bool _frameBegun { false };
    bool _layerViewsValid { false };
    bool _hmdMounted { false };
};

#endif // hifi_OpenXrDisplayPlugin_h
