//
//  ApplicationFrame.cpp
//  interface/src
//
//  Application's frame loop: idle() (the tick at the display's rate: the timers, the simulation step, the
//  keyboard-focus highlight, the game-loop counter; on Windows the CPU monitor it starts), update() (the simulation
//  step: input, the avatars, physics and its gate, the entities, the audio and octree housekeeping, the post-update
//  work) and the render arguments handed to the graphics engine (updateRenderArgs; the camera's placement and the view
//  frustums are the ViewState's, the physics gate and the queries the DomainSession's),
//  with the small per-frame helpers (updateLOD, pushPostUpdateLambda, updateMyAvatarLookAtPosition, updateThreads,
//  updateDialogs). The members as they stood in Application.cpp, in their order; a home of their own until an
//  owner takes them (ARCHITECTURE.md §5.4 step 4).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "Application.h"
#include "graphics/RenderEventHandler.h"
#include <MainWindow.h>   // the window and the GL canvas handed to the cursor each frame
#include "GLCanvas.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <glm/glm.hpp>
#include <glm/gtx/vector_angle.hpp>
#include <gl/Config.h>
#include <QtQuick/QQuickWindow>
#include <StatTracker.h>
#include <AnimDebugDraw.h>
#include <DebugDraw.h>
#include <EntityScriptingInterface.h>
#include <controllers/UserInputMapper.h>
#include <OffscreenUi.h>
#include <PhysicsEngine.h>
#include <plugins/PluginManager.h>
#include <ResourceCache.h>
#include <display-plugins/hmd/HmdDisplayPlugin.h>
#include "AudioClient.h"
#include "avatar/AvatarManager.h"
#include "LODManager.h"
#include "scripting/HMDScriptingInterface.h"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#endif
#include "ui/DialogsManager.h"
#include "ui/Stats.h"
#include "ui/AnimStats.h"
#include "avatar/GrabManager.h"
#include <PickManager.h>
#include <PointerManager.h>
#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <VersionHelpers.h>
#endif
#if defined(Q_OS_ANDROID)
#include <android/log.h>
#include "AndroidHelper.h"
#endif

using namespace std;

QSharedPointer<OffscreenUi> getOffscreenUI();                        // Application.cpp's
float verticalFieldOfView(float horizontalFov, float aspectRatio);   // Display.cpp's, beside resizeGL

// Application.cpp's constant that only the frame loop reads
static const quint64 TOO_LONG_SINCE_LAST_SEND_DOWNSTREAM_AUDIO_STATS = 1 * USECS_PER_SECOND;

#ifdef Q_OS_WIN
#include <Windows.h>
#include <TCHAR.h>
#include <pdh.h>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "ntdll.lib")

extern "C" {
    enum SYSTEM_INFORMATION_CLASS {
        SystemBasicInformation = 0,
        SystemProcessorPerformanceInformation = 8,
    };

    struct SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
        LARGE_INTEGER IdleTime;
        LARGE_INTEGER KernelTime;
        LARGE_INTEGER UserTime;
        LARGE_INTEGER DpcTime;
        LARGE_INTEGER InterruptTime;
        ULONG InterruptCount;
    };

    struct SYSTEM_BASIC_INFORMATION {
        ULONG Reserved;
        ULONG TimerResolution;
        ULONG PageSize;
        ULONG NumberOfPhysicalPages;
        ULONG LowestPhysicalPageNumber;
        ULONG HighestPhysicalPageNumber;
        ULONG AllocationGranularity;
        ULONG_PTR MinimumUserModeAddress;
        ULONG_PTR MaximumUserModeAddress;
        ULONG_PTR ActiveProcessorsAffinityMask;
        CCHAR NumberOfProcessors;
    };

    NTSYSCALLAPI LONG NTAPI NtQuerySystemInformation(
        _In_ SYSTEM_INFORMATION_CLASS SystemInformationClass,
        _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
        _In_ ULONG SystemInformationLength,
        _Out_opt_ PULONG ReturnLength
    );

}
template <typename T>
LONG NtQuerySystemInformation(SYSTEM_INFORMATION_CLASS SystemInformationClass, T& t) {
    return NtQuerySystemInformation(SystemInformationClass, &t, (ULONG)sizeof(T), nullptr);
}

template <typename T>
LONG NtQuerySystemInformation(SYSTEM_INFORMATION_CLASS SystemInformationClass, std::vector<T>& t) {
    return NtQuerySystemInformation(SystemInformationClass, t.data(), (ULONG)(sizeof(T) * t.size()), nullptr);
}


template <typename T>
void updateValueAndDelta(std::pair<T, T>& pair, T newValue) {
    auto& value = pair.first;
    auto& delta = pair.second;
    delta = (value != 0) ? newValue - value : 0;
    value = newValue;
}

struct MyCpuInfo {
    using ValueAndDelta = std::pair<LONGLONG, LONGLONG>;
    std::string name;
    ValueAndDelta kernel { 0, 0 };
    ValueAndDelta user { 0, 0 };
    ValueAndDelta idle { 0, 0 };
    float kernelUsage { 0.0f };
    float userUsage { 0.0f };

    void update(const SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION& cpuInfo) {
        updateValueAndDelta(kernel, cpuInfo.KernelTime.QuadPart);
        updateValueAndDelta(user, cpuInfo.UserTime.QuadPart);
        updateValueAndDelta(idle, cpuInfo.IdleTime.QuadPart);
        auto totalTime = kernel.second + user.second + idle.second;
        if (totalTime != 0) {
            kernelUsage = (FLOAT)kernel.second / totalTime;
            userUsage = (FLOAT)user.second / totalTime;
        } else {
            kernelUsage = userUsage = 0.0f;
        }
    }
};

void updateCpuInformation() {
    static std::once_flag once;
    static SYSTEM_BASIC_INFORMATION systemInfo {};
    static SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION cpuTotals;
    static std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> cpuInfos;
    static std::vector<MyCpuInfo> myCpuInfos;
    static MyCpuInfo myCpuTotals;
    std::call_once(once, [&] {
        NtQuerySystemInformation( SystemBasicInformation, systemInfo);
        cpuInfos.resize(systemInfo.NumberOfProcessors);
        myCpuInfos.resize(systemInfo.NumberOfProcessors);
        for (size_t i = 0; i < systemInfo.NumberOfProcessors; ++i) {
            myCpuInfos[i].name = "cpu." + std::to_string(i);
        }
        myCpuTotals.name = "cpu.total";
    });
    NtQuerySystemInformation(SystemProcessorPerformanceInformation, cpuInfos);

    // Zero the CPU totals.
    memset(&cpuTotals, 0, sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
    for (size_t i = 0; i < systemInfo.NumberOfProcessors; ++i) {
        auto& cpuInfo = cpuInfos[i];
        // KernelTime includes IdleTime.
        cpuInfo.KernelTime.QuadPart -= cpuInfo.IdleTime.QuadPart;

        // Update totals
        cpuTotals.IdleTime.QuadPart += cpuInfo.IdleTime.QuadPart;
        cpuTotals.KernelTime.QuadPart += cpuInfo.KernelTime.QuadPart;
        cpuTotals.UserTime.QuadPart += cpuInfo.UserTime.QuadPart;

        // Update friendly structure
        auto& myCpuInfo = myCpuInfos[i];
        myCpuInfo.update(cpuInfo);
        PROFILE_COUNTER(app, myCpuInfo.name.c_str(), {
            { "kernel", myCpuInfo.kernelUsage },
            { "user", myCpuInfo.userUsage }
        });
    }

    myCpuTotals.update(cpuTotals);
    PROFILE_COUNTER(app, myCpuTotals.name.c_str(), {
        { "kernel", myCpuTotals.kernelUsage },
        { "user", myCpuTotals.userUsage }
    });
}


static ULARGE_INTEGER lastCPU, lastSysCPU, lastUserCPU;
static int numProcessors;
static HANDLE self;
static PDH_HQUERY cpuQuery;
static PDH_HCOUNTER cpuTotal;

void initCpuUsage() {
    SYSTEM_INFO sysInfo;
    FILETIME ftime, fsys, fuser;

    GetSystemInfo(&sysInfo);
    numProcessors = sysInfo.dwNumberOfProcessors;

    GetSystemTimeAsFileTime(&ftime);
    memcpy(&lastCPU, &ftime, sizeof(FILETIME));

    self = GetCurrentProcess();
    GetProcessTimes(self, &ftime, &ftime, &fsys, &fuser);
    memcpy(&lastSysCPU, &fsys, sizeof(FILETIME));
    memcpy(&lastUserCPU, &fuser, sizeof(FILETIME));

    PdhOpenQuery(NULL, NULL, &cpuQuery);
    PdhAddCounter(cpuQuery, "\\Processor(_Total)\\% Processor Time", NULL, &cpuTotal);
    PdhCollectQueryData(cpuQuery);
}

void getCpuUsage(vec3& systemAndUser) {
    FILETIME ftime, fsys, fuser;
    ULARGE_INTEGER now, sys, user;

    GetSystemTimeAsFileTime(&ftime);
    memcpy(&now, &ftime, sizeof(FILETIME));

    GetProcessTimes(self, &ftime, &ftime, &fsys, &fuser);
    memcpy(&sys, &fsys, sizeof(FILETIME));
    memcpy(&user, &fuser, sizeof(FILETIME));
    systemAndUser.x = (sys.QuadPart - lastSysCPU.QuadPart);
    systemAndUser.y = (user.QuadPart - lastUserCPU.QuadPart);
    systemAndUser /= (float)(now.QuadPart - lastCPU.QuadPart);
    systemAndUser /= (float)numProcessors;
    systemAndUser *= 100.0f;
    lastCPU = now;
    lastUserCPU = user;
    lastSysCPU = sys;

    PDH_FMT_COUNTERVALUE counterVal;
    PdhCollectQueryData(cpuQuery);
    PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, NULL, &counterVal);
    systemAndUser.z = (float)counterVal.doubleValue;
}

void setupCpuMonitorThread() {
    initCpuUsage();
    auto cpuMonitorThread = QThread::currentThread();

    QTimer* timer = new QTimer();
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, [] {
        updateCpuInformation();
        vec3 kernelUserAndSystem;
        getCpuUsage(kernelUserAndSystem);
        PROFILE_COUNTER(app, "cpuProcess", { { "system", kernelUserAndSystem.x }, { "user", kernelUserAndSystem.y } });
        PROFILE_COUNTER(app, "cpuSystem", { { "system", kernelUserAndSystem.z } });
    });
    QObject::connect(cpuMonitorThread, &QThread::finished, [=] {
        timer->deleteLater();
        cpuMonitorThread->deleteLater();
    });
    timer->start();
}

#endif

void Application::idle() {
    PerformanceTimer perfTimer("idle");

    auto offscreenUi = getOffscreenUI();

#ifdef Q_OS_WIN
    // If tracing is enabled then monitor the CPU in a separate thread
    static std::once_flag once;
    std::call_once(once, [&] {
        if (trace_app().isDebugEnabled()) {
            QThread* cpuMonitorThread = new QThread(qApp);
            cpuMonitorThread->setObjectName("cpuMonitorThread");
            QObject::connect(cpuMonitorThread, &QThread::started, [this] { setupCpuMonitorThread(); });
            QObject::connect(qApp, &QCoreApplication::aboutToQuit, cpuMonitorThread, &QThread::quit);
            cpuMonitorThread->start();
        }
    });
#endif

    auto displayPlugin = getActiveDisplayPlugin();
    if (displayPlugin) {
        auto uiSize = displayPlugin->getRecommendedUiSize();
        // Bit of a hack since there's no device pixel ratio change event I can find.
        if (offscreenUi->size() != fromGlm(uiSize)) {
            qCDebug(interfaceapp) << "Device pixel ratio changed, triggering resize to " << uiSize;
            offscreenUi->resize(fromGlm(uiSize));
        }
    }

    if (displayPlugin) {
        PROFILE_COUNTER_IF_CHANGED(app, "present", float, displayPlugin->presentRate());
    }
    PROFILE_COUNTER_IF_CHANGED(app, "renderLoopRate", float, getRenderLoopRate());
    PROFILE_COUNTER_IF_CHANGED(app, "currentDownloads", uint32_t, ResourceCache::getLoadingRequests().length());
    PROFILE_COUNTER_IF_CHANGED(app, "pendingDownloads", uint32_t, ResourceCache::getPendingRequestCount());
    PROFILE_COUNTER_IF_CHANGED(app, "currentProcessing", int, DependencyManager::get<StatTracker>()->getStat("Processing").toInt());
    PROFILE_COUNTER_IF_CHANGED(app, "pendingProcessing", int, DependencyManager::get<StatTracker>()->getStat("PendingProcessing").toInt());
    auto renderConfig = _graphicsEngine.getRenderEngine()->getConfiguration();
    PROFILE_COUNTER_IF_CHANGED(render, "gpuTime", float, (float)_graphicsEngine.getGPUContext()->getFrameTimerGPUAverage());

    PROFILE_RANGE(app, __FUNCTION__);

    float secondsSinceLastUpdate = (float)_lastTimeUpdated.nsecsElapsed() / NSECS_PER_MSEC / MSECS_PER_SECOND;
    _lastTimeUpdated.start();

    // If the offscreen Ui has something active that is NOT the root, then assume it has keyboard focus.
    if (offscreenUi && offscreenUi->getWindow()) {
        auto activeFocusItem = offscreenUi->getWindow()->activeFocusItem();
        if (_keyboardDeviceHasFocus && (activeFocusItem != NULL && activeFocusItem != offscreenUi->getRootItem())) {
            _keyboardMouseDevice->pluginFocusOutEvent();
            _keyboardDeviceHasFocus = false;
            synthesizeKeyReleasEvents();
        } else if (activeFocusItem == offscreenUi->getRootItem()) {
            _keyboardDeviceHasFocus = true;
        }
    }
    
    _windowCursor.apply(_window, _glWidget);

    auto stats = Stats::getInstance();
    if (stats) {
        stats->updateStats();
    }
    auto animStats = AnimStats::getInstance();
    if (animStats) {
        animStats->updateStats();
    }

    // Normally we check PipelineWarnings, but since idle will often take more than 10ms we only show these idle timing
    // details if we're in ExtraDebugging mode. However, the ::update() and its subcomponents will show their timing
    // details normally.
#ifdef Q_OS_ANDROID
    bool showWarnings = false;
#else
    bool showWarnings = getLogger()->extraDebugging();
#endif
    PerformanceWarning warn(showWarnings, "idle()");

    {
        _gameWorkload.updateViews(_viewState.viewFrustum(), getMyAvatar()->getHeadPosition());
        _gameWorkload._engine->run();
    }
    {
        PerformanceTimer perfTimer("update");
        PerformanceWarning warn(showWarnings, "Application::idle()... update()");
        static const float BIGGEST_DELTA_TIME_SECS = 0.25f;
        update(glm::clamp(secondsSinceLastUpdate, 0.0f, BIGGEST_DELTA_TIME_SECS));
    }

    _keyboardFocus.update();

    {
        PerformanceTimer perfTimer("pluginIdle");
        PerformanceWarning warn(showWarnings, "Application::idle()... pluginIdle()");
        getActiveDisplayPlugin()->idle();
        auto inputPlugins = PluginManager::getInstance()->getInputPlugins();
        foreach(auto inputPlugin, inputPlugins) {
            if (inputPlugin->isActive()) {
                inputPlugin->idle();
            }
        }
    }
    {
        PerformanceTimer perfTimer("rest");
        PerformanceWarning warn(showWarnings, "Application::idle()... rest of it");
        _idleLoopStdev.addValue(secondsSinceLastUpdate);

        //  Record standard deviation and reset counter if needed
        const int STDEV_SAMPLES = 500;
        if (_idleLoopStdev.getSamples() > STDEV_SAMPLES) {
            _idleLoopStdev.reset();
        }
    }

    _overlayConductor.update(secondsSinceLastUpdate);

    _gameLoopCounter.increment();
}

void Application::updateLOD(float deltaTime) const {
    PerformanceTimer perfTimer("LOD");
    // adjust it unless we were asked to disable this feature, or if we're currently in throttleRendering mode
    if (!_display.isThrottleRendering()) {
        float presentTime = getActiveDisplayPlugin()->getAveragePresentTime();
        float engineRunTime = (float)(_graphicsEngine.getRenderEngine()->getConfiguration().get()->getCPURunTime());
        float gpuTime = getGPUContext()->getFrameTimerGPUAverage();
        float batchTime = getGPUContext()->getFrameTimerBatchAverage();
        auto lodManager = DependencyManager::get<LODManager>();
        lodManager->setRenderTimes(presentTime, engineRunTime, batchTime, gpuTime);
        lodManager->autoAdjustLOD(deltaTime);
    } else {
        DependencyManager::get<LODManager>()->resetLODAdjust();
    }
}

void Application::pushPostUpdateLambda(void* key, const std::function<void()>& func) {
    std::unique_lock<std::mutex> guard(_postUpdateLambdasLock);
    _postUpdateLambdas[key] = func;
}

// Called during Application::update immediately before AvatarManager::updateMyAvatar, updating my data that is then sent
// to everyone.
// The principal result is to call updateLookAtTargetAvatar() and then setLookAtPosition().
// Note that it is called BEFORE we update position or joints based on sensors, etc.
void Application::updateMyAvatarLookAtPosition(float deltaTime) {
    PerformanceTimer perfTimer("lookAt");
    bool showWarnings = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showWarnings, "Application::updateMyAvatarLookAtPosition()");

    auto myAvatar = getMyAvatar();
    myAvatar->updateEyesLookAtPosition(deltaTime);
}

void Application::updateThreads(float deltaTime) {
    PerformanceTimer perfTimer("updateThreads");
    bool showWarnings = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showWarnings, "Application::updateThreads()");

    // parse voxel packets
    if (!_enableProcessOctreeThread) {
        _domainSession.octreeProcessor().threadRoutine();
        _entityEditSender.threadRoutine();
    }
}

void Application::updateDialogs(float deltaTime) const {
    PerformanceTimer perfTimer("updateDialogs");
    bool showWarnings = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showWarnings, "Application::updateDialogs()");
    auto dialogsManager = DependencyManager::get<DialogsManager>();

    QPointer<OctreeStatsDialog> octreeStatsDialog = dialogsManager->getOctreeStatsDialog();
    if (octreeStatsDialog) {
        octreeStatsDialog->update();
    }
}

void Application::update(float deltaTime) {
    PROFILE_RANGE_EX(app, __FUNCTION__, 0xffff0000, (uint64_t)_graphicsEngine._renderFrameCount + 1);

    if (_aboutToQuit) {
        return;
    }

    _domainSession.enablePhysicsWhenReady();   // the physics gate: physics stays off until the scene near the avatar is in

    // the sign-in and loading screens are drawn at a capped rate on the desktop (RefreshRateManager::setScreenCap)
    getRefreshRateManager().setScreenCap(_signInGate.poppedUp() ? RefreshRateManager::SIGN_IN_SCREEN_RATE
        : (_domainSession.isInterstitialMode() || _domainSession.isWorldLoading()) ? RefreshRateManager::LOADING_SCREEN_RATE
        : 0);

    auto myAvatar = getMyAvatar();
    {
        PerformanceTimer perfTimer("devices");
        auto userInputMapper = DependencyManager::get<UserInputMapper>();

        controller::HmdAvatarAlignmentType hmdAvatarAlignmentType;
        if (myAvatar->getHmdAvatarAlignmentType() == "eyes") {
            hmdAvatarAlignmentType = controller::HmdAvatarAlignmentType::Eyes;
        } else {
            hmdAvatarAlignmentType = controller::HmdAvatarAlignmentType::Head;
        }

        controller::InputCalibrationData calibrationData = {
            myAvatar->getSensorToWorldMatrix(),
            createMatFromQuatAndPos(myAvatar->getWorldOrientation(), myAvatar->getWorldPosition()),
            myAvatar->getHMDSensorMatrix(),
            myAvatar->getCenterEyeCalibrationMat(),
            myAvatar->getHeadCalibrationMat(),
            myAvatar->getSpine2CalibrationMat(),
            myAvatar->getHipsCalibrationMat(),
            myAvatar->getLeftFootCalibrationMat(),
            myAvatar->getRightFootCalibrationMat(),
            myAvatar->getRightArmCalibrationMat(),
            myAvatar->getLeftArmCalibrationMat(),
            myAvatar->getRightHandCalibrationMat(),
            myAvatar->getLeftHandCalibrationMat(),
            hmdAvatarAlignmentType
        };

        InputPluginPointer keyboardMousePlugin;
        for (auto inputPlugin : PluginManager::getInstance()->getInputPlugins()) {
            if (inputPlugin->getName() == KeyboardMouseDevice::NAME) {
                keyboardMousePlugin = inputPlugin;
            } else if (inputPlugin->isActive()) {
                inputPlugin->pluginUpdate(deltaTime, calibrationData);
            }
        }

        userInputMapper->setInputCalibrationData(calibrationData);
        userInputMapper->update(deltaTime);

        if (keyboardMousePlugin && keyboardMousePlugin->isActive()) {
            keyboardMousePlugin->pluginUpdate(deltaTime, calibrationData);
        }
        // Transfer the user inputs to the driveKeys
        // FIXME can we drop drive keys and just have the avatar read the action states directly?
        myAvatar->clearDriveKeys();
        if (_viewState.camera().getMode() != CAMERA_MODE_INDEPENDENT && !isInterstitialMode()) {
            if (!_controllerScriptingInterface->areActionsCaptured() && _viewState.camera().getMode() != CAMERA_MODE_MIRROR) {
                myAvatar->setDriveKey(MyAvatar::TRANSLATE_Z, -1.0f * userInputMapper->getActionState(controller::Action::TRANSLATE_Z));
                myAvatar->setDriveKey(MyAvatar::TRANSLATE_Y, userInputMapper->getActionState(controller::Action::TRANSLATE_Y));
                myAvatar->setDriveKey(MyAvatar::TRANSLATE_X, userInputMapper->getActionState(controller::Action::TRANSLATE_X));
                if (deltaTime > FLT_EPSILON && userInputMapper->getActionState(controller::Action::TRANSLATE_CAMERA_Z)  == 0.0f) {
                    myAvatar->setDriveKey(MyAvatar::PITCH, -1.0f * userInputMapper->getActionState(controller::Action::PITCH));
                    myAvatar->setDriveKey(MyAvatar::YAW, -1.0f * userInputMapper->getActionState(controller::Action::YAW));
                    myAvatar->setDriveKey(MyAvatar::DELTA_PITCH, -1.0f * userInputMapper->getActionState(controller::Action::DELTA_PITCH));
                    myAvatar->setDriveKey(MyAvatar::DELTA_YAW, -1.0f * userInputMapper->getActionState(controller::Action::DELTA_YAW));
                    myAvatar->setDriveKey(MyAvatar::STEP_YAW, -1.0f * userInputMapper->getActionState(controller::Action::STEP_YAW));
                }
            }
            myAvatar->setDriveKey(MyAvatar::ZOOM, userInputMapper->getActionState(controller::Action::TRANSLATE_CAMERA_Z));
        }

        myAvatar->setSprintMode((bool)userInputMapper->getActionState(controller::Action::SPRINT));
        static const std::vector<controller::Action> avatarControllerActions = {
            controller::Action::LEFT_HAND,
            controller::Action::RIGHT_HAND,
            controller::Action::LEFT_FOOT,
            controller::Action::RIGHT_FOOT,
            controller::Action::HIPS,
            controller::Action::SPINE2,
            controller::Action::HEAD,
            controller::Action::LEFT_HAND_THUMB1,
            controller::Action::LEFT_HAND_THUMB2,
            controller::Action::LEFT_HAND_THUMB3,
            controller::Action::LEFT_HAND_THUMB4,
            controller::Action::LEFT_HAND_INDEX1,
            controller::Action::LEFT_HAND_INDEX2,
            controller::Action::LEFT_HAND_INDEX3,
            controller::Action::LEFT_HAND_INDEX4,
            controller::Action::LEFT_HAND_MIDDLE1,
            controller::Action::LEFT_HAND_MIDDLE2,
            controller::Action::LEFT_HAND_MIDDLE3,
            controller::Action::LEFT_HAND_MIDDLE4,
            controller::Action::LEFT_HAND_RING1,
            controller::Action::LEFT_HAND_RING2,
            controller::Action::LEFT_HAND_RING3,
            controller::Action::LEFT_HAND_RING4,
            controller::Action::LEFT_HAND_PINKY1,
            controller::Action::LEFT_HAND_PINKY2,
            controller::Action::LEFT_HAND_PINKY3,
            controller::Action::LEFT_HAND_PINKY4,
            controller::Action::RIGHT_HAND_THUMB1,
            controller::Action::RIGHT_HAND_THUMB2,
            controller::Action::RIGHT_HAND_THUMB3,
            controller::Action::RIGHT_HAND_THUMB4,
            controller::Action::RIGHT_HAND_INDEX1,
            controller::Action::RIGHT_HAND_INDEX2,
            controller::Action::RIGHT_HAND_INDEX3,
            controller::Action::RIGHT_HAND_INDEX4,
            controller::Action::RIGHT_HAND_MIDDLE1,
            controller::Action::RIGHT_HAND_MIDDLE2,
            controller::Action::RIGHT_HAND_MIDDLE3,
            controller::Action::RIGHT_HAND_MIDDLE4,
            controller::Action::RIGHT_HAND_RING1,
            controller::Action::RIGHT_HAND_RING2,
            controller::Action::RIGHT_HAND_RING3,
            controller::Action::RIGHT_HAND_RING4,
            controller::Action::RIGHT_HAND_PINKY1,
            controller::Action::RIGHT_HAND_PINKY2,
            controller::Action::RIGHT_HAND_PINKY3,
            controller::Action::RIGHT_HAND_PINKY4,
            controller::Action::LEFT_ARM,
            controller::Action::RIGHT_ARM,
            controller::Action::LEFT_SHOULDER,
            controller::Action::RIGHT_SHOULDER,
            controller::Action::LEFT_FORE_ARM,
            controller::Action::RIGHT_FORE_ARM,
            controller::Action::LEFT_LEG,
            controller::Action::RIGHT_LEG,
            controller::Action::LEFT_UP_LEG,
            controller::Action::RIGHT_UP_LEG,
            controller::Action::LEFT_TOE_BASE,
            controller::Action::RIGHT_TOE_BASE,
            controller::Action::LEFT_EYE,
            controller::Action::RIGHT_EYE

        };

        // copy controller poses from userInputMapper to myAvatar.
        glm::mat4 myAvatarMatrix = createMatFromQuatAndPos(myAvatar->getWorldOrientation(), myAvatar->getWorldPosition());
        glm::mat4 worldToSensorMatrix = glm::inverse(myAvatar->getSensorToWorldMatrix());
        glm::mat4 avatarToSensorMatrix = worldToSensorMatrix * myAvatarMatrix;
        for (auto& action : avatarControllerActions) {
            controller::Pose pose = userInputMapper->getPoseState(action);
            myAvatar->setControllerPoseInSensorFrame(action, pose.transform(avatarToSensorMatrix));
        }

        static const std::vector<QString> trackedObjectStringLiterals = {
            QStringLiteral("_TrackedObject00"), QStringLiteral("_TrackedObject01"), QStringLiteral("_TrackedObject02"), QStringLiteral("_TrackedObject03"),
            QStringLiteral("_TrackedObject04"), QStringLiteral("_TrackedObject05"), QStringLiteral("_TrackedObject06"), QStringLiteral("_TrackedObject07"),
            QStringLiteral("_TrackedObject08"), QStringLiteral("_TrackedObject09"), QStringLiteral("_TrackedObject10"), QStringLiteral("_TrackedObject11"),
            QStringLiteral("_TrackedObject12"), QStringLiteral("_TrackedObject13"), QStringLiteral("_TrackedObject14"), QStringLiteral("_TrackedObject15")
        };

        // Controlled by the Developer > Avatar > Show Tracked Objects menu.
        if (_showTrackedObjects) {
            static const std::vector<controller::Action> trackedObjectActions = {
                controller::Action::TRACKED_OBJECT_00, controller::Action::TRACKED_OBJECT_01, controller::Action::TRACKED_OBJECT_02, controller::Action::TRACKED_OBJECT_03,
                controller::Action::TRACKED_OBJECT_04, controller::Action::TRACKED_OBJECT_05, controller::Action::TRACKED_OBJECT_06, controller::Action::TRACKED_OBJECT_07,
                controller::Action::TRACKED_OBJECT_08, controller::Action::TRACKED_OBJECT_09, controller::Action::TRACKED_OBJECT_10, controller::Action::TRACKED_OBJECT_11,
                controller::Action::TRACKED_OBJECT_12, controller::Action::TRACKED_OBJECT_13, controller::Action::TRACKED_OBJECT_14, controller::Action::TRACKED_OBJECT_15
            };

            int i = 0;
            glm::vec4 BLUE(0.0f, 0.0f, 1.0f, 1.0f);
            for (auto& action : trackedObjectActions) {
                controller::Pose pose = userInputMapper->getPoseState(action);
                if (pose.valid) {
                    glm::vec3 pos = transformPoint(myAvatarMatrix, pose.translation);
                    glm::quat rot = glmExtractRotation(myAvatarMatrix) * pose.rotation;
                    DebugDraw::getInstance().addMarker(trackedObjectStringLiterals[i], rot, pos, BLUE);
                } else {
                    DebugDraw::getInstance().removeMarker(trackedObjectStringLiterals[i]);
                }
                i++;
            }
        } else if (_prevShowTrackedObjects) {
            for (auto& key : trackedObjectStringLiterals) {
                DebugDraw::getInstance().removeMarker(key);
            }
        }
        _prevShowTrackedObjects = _showTrackedObjects;
    }

    updateThreads(deltaTime); // If running non-threaded, then give the threads some time to process...
    updateDialogs(deltaTime); // update various stats dialogs if present

    auto grabManager = DependencyManager::get<GrabManager>();
    grabManager->simulateGrabs();

    // TODO: break these out into distinct perfTimers when they prove interesting
    {
        PROFILE_RANGE(app, "PickManager");
        PerformanceTimer perfTimer("pickManager");
        DependencyManager::get<PickManager>()->update();
    }

    {
        PROFILE_RANGE(app, "PointerManager");
        PerformanceTimer perfTimer("pointerManager");
        DependencyManager::get<PointerManager>()->update();
    }

    QSharedPointer<AvatarManager> avatarManager = DependencyManager::get<AvatarManager>();

    {
        PROFILE_RANGE(simulation_physics, "Simulation");
        PerformanceTimer perfTimer("simulation");

        getEntities()->preUpdate();
        _entitySimulation->removeDeadEntities();

        auto t0 = std::chrono::high_resolution_clock::now();
        auto t1 = t0;
        {
            PROFILE_RANGE(simulation_physics, "PrePhysics");
            PerformanceTimer perfTimer("prePhysics)");
            {
                PROFILE_RANGE(simulation_physics, "Entities");
                PhysicsEngine::Transaction transaction;
                _entitySimulation->buildPhysicsTransaction(transaction);
                _physicsEngine->processTransaction(transaction);
                _entitySimulation->handleProcessedPhysicsTransaction(transaction);
            }

            t1 = std::chrono::high_resolution_clock::now();

            {
                PROFILE_RANGE(simulation_physics, "Avatars");
                PhysicsEngine::Transaction transaction;
                avatarManager->buildPhysicsTransaction(transaction);
                _physicsEngine->processTransaction(transaction);
                avatarManager->handleProcessedPhysicsTransaction(transaction);

                myAvatar->prepareForPhysicsSimulation();
                myAvatar->getCharacterController()->preSimulation();
            }
        }

        // Frame pacing (PHYSICS.md "The driver's stutter"): the presents are paced at the display's period, one a
        // refresh interval, and this update is posted at each; the physics' paced clock advances by the intervals
        // between the present that posted this update and the one that posted the last (one as a rule; two when a
        // present lost a refresh or this thread missed a present's update), never by a wall clock this thread's own
        // lateness disturbs (an update run late and the next on time are 2 ms apart, yet each shows for a refresh)
        {
            uint64_t slot = _presentSlot.load();
            const uint64_t MAX_PERIODS = 16;
            int periods = (_lastUpdateSlot != 0 && slot > _lastUpdateSlot) ? (int)std::min(slot - _lastUpdateSlot, MAX_PERIODS) : 1;
            _lastUpdateSlot = slot;
            _physicsEngine->setPresentPeriod(getPresentPeriod(), periods);
        }

        if (isPhysicsEnabled()) {
            {
                PROFILE_RANGE(simulation_physics, "PrepareActions");
                _entitySimulation->applyDynamicChanges();
                _physicsEngine->forEachDynamic([&](EntityDynamicPointer dynamic) {
                    dynamic->prepareForPhysicsSimulation();
                });
            }
            auto t2 = std::chrono::high_resolution_clock::now();
            {
                PROFILE_RANGE(simulation_physics, "StepPhysics");
                PerformanceTimer perfTimer("stepPhysics");
                getEntities()->getTree()->withWriteLock([&] {
                    _physicsEngine->stepSimulation();
                });
            }
            auto t3 = std::chrono::high_resolution_clock::now();
            {
                if (_physicsEngine->hasOutgoingChanges()) {
                    {
                        PROFILE_RANGE(simulation_physics, "PostPhysics");
                        PerformanceTimer perfTimer("postPhysics");
                        // grab the collision events BEFORE handleChangedMotionStates() because at this point
                        // we have a better idea of which objects we own or should own.
                        auto& collisionEvents = _physicsEngine->getCollisionEvents();

                        getEntities()->getTree()->withWriteLock([&] {
                            PROFILE_RANGE(simulation_physics, "HandleChanges");
                            PerformanceTimer perfTimer("handleChanges");

                            const VectorOfMotionStates& outgoingChanges = _physicsEngine->getChangedMotionStates();
                            _entitySimulation->handleChangedMotionStates(outgoingChanges);
                            avatarManager->handleChangedMotionStates(outgoingChanges);

                            const VectorOfMotionStates& deactivations = _physicsEngine->getDeactivatedMotionStates();
                            _entitySimulation->handleDeactivatedMotionStates(deactivations);
                        });

                        // handleCollisionEvents() AFTER handleChangedMotionStates()
                        {
                            PROFILE_RANGE(simulation_physics, "CollisionEvents");
                            avatarManager->handleCollisionEvents(collisionEvents);
                            // Collision events (and their scripts) must not be handled when we're locked, above. (That would risk
                            // deadlock.)
                            _entitySimulation->handleCollisionEvents(collisionEvents);
                        }

                        {
                            PROFILE_RANGE(simulation_physics, "MyAvatar");
                            myAvatar->getCharacterController()->postSimulation();
                            myAvatar->harvestResultsFromPhysicsSimulation(deltaTime);
                        }

                        if (PerformanceTimer::isActive() &&
                                Menu::getInstance()->isOptionChecked(MenuOption::DisplayDebugTimingDetails) &&
                                Menu::getInstance()->isOptionChecked(MenuOption::ExpandPhysicsTiming)) {
                            _physicsEngine->harvestPerformanceStats();
                        }
                        // NOTE: the PhysicsEngine stats are written to stdout NOT to Qt log framework
                        _physicsEngine->dumpStatsIfNecessary();
                    }
                    auto t4 = std::chrono::high_resolution_clock::now();

                    // NOTE: the getEntities()->update() call below will wait for lock
                    // and will provide non-physical entity motion
                    getEntities()->update(true); // update the models...

                    auto t5 = std::chrono::high_resolution_clock::now();

                    workload::Timings timings(6);
                    timings[0] = t1 - t0; // prePhysics entities
                    timings[1] = t2 - t1; // prePhysics avatars
                    timings[2] = t3 - t2; // stepPhysics
                    timings[3] = t4 - t3; // postPhysics
                    timings[4] = t5 - t4; // non-physical kinematics
                    timings[5] = workload::Timing_ns((int32_t)(NSECS_PER_SECOND * deltaTime)); // game loop duration
                    _gameWorkload.updateSimulationTimings(timings);
                }
            }
        } else {
            // update the rendering without any simulation
            getEntities()->update(false);
        }
        // remove recently dead avatarEntities
        SetOfEntities deadAvatarEntities;
        _entitySimulation->takeDeadAvatarEntities(deadAvatarEntities);
        avatarManager->removeDeadAvatarEntities(deadAvatarEntities);
    }

    // AvatarManager update
    {
        {
            PROFILE_RANGE(simulation, "OtherAvatars");
            PerformanceTimer perfTimer("otherAvatars");
            avatarManager->updateOtherAvatars(deltaTime);
        }

        {
            PROFILE_RANGE(simulation, "MyAvatar");
            PerformanceTimer perfTimer("MyAvatar");
            qApp->updateMyAvatarLookAtPosition(deltaTime);
            avatarManager->updateMyAvatar(deltaTime);
        }
    }

    bool showWarnings = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showWarnings, "Application::update()");

    updateLOD(deltaTime);

    _signInGate.update();   // the headset's sign-in dialog, while it is up

    {
        PROFILE_RANGE_EX(app, "Overlays", 0xffff0000, (uint64_t)getActiveDisplayPlugin()->presentCount());
        PerformanceTimer perfTimer("overlays");
        _overlays.update(deltaTime);
    }

    _viewState.update();   // the view frustum from the camera, the conical views for the queries below

    quint64 now = usecTimestampNow();

    _domainSession.queryServers();   // the entity and avatar queries, when the views changed enough or the period passed

    _domainSession.sendNacksWhenDue(now);

    // send packet containing downstream audio stats to the AudioMixer
    {
        quint64 sinceLastNack = now - _lastSendDownstreamAudioStats;
        if (sinceLastNack > TOO_LONG_SINCE_LAST_SEND_DOWNSTREAM_AUDIO_STATS && !isInterstitialMode()) {
            _lastSendDownstreamAudioStats = now;

            QMetaObject::invokeMethod(DependencyManager::get<AudioClient>().data(), "sendDownstreamAudioStatsPacket", Qt::QueuedConnection);
        }
    }

    {
        PerformanceTimer perfTimer("avatarManager/postUpdate");
        avatarManager->postUpdate(deltaTime, getMain3DScene());
    }

    {
        PROFILE_RANGE_EX(app, "PostUpdateLambdas", 0xffff0000, (uint64_t)0);
        PerformanceTimer perfTimer("postUpdateLambdas");
        std::unique_lock<std::mutex> guard(_postUpdateLambdasLock);
        for (auto& iter : _postUpdateLambdas) {
            iter.second();
        }
        _postUpdateLambdas.clear();
    }


    updateRenderArgs(deltaTime);

    {
        PerformanceTimer perfTimer("AnimDebugDraw");
        AnimDebugDraw::getInstance().update();
    }


    { // Game loop is done, mark the end of the frame for the scene transactions and the render loop to take over
        PerformanceTimer perfTimer("enqueueFrame");
        getMain3DScene()->enqueueFrame();
    }

    // If the display plugin is inactive then the frames won't be processed so process them here.
    if (!getActiveDisplayPlugin()->isActive()) {
        getMain3DScene()->processTransactionQueue();
    }

    // decide if the sensorToWorldMatrix is changing in a way that warrents squeezing the edges of the view down
    if (getActiveDisplayPlugin()->isHmd()) {
        PerformanceTimer perfTimer("squeezeVision");
        _visionSqueeze.updateVisionSqueeze(myAvatar->getSensorToWorldMatrix(), deltaTime);
    }
}

void Application::updateRenderArgs(float deltaTime) {
    _graphicsEngine.editRenderArgs([this, deltaTime](AppRenderArgs& appRenderArgs) {
        PerformanceTimer perfTimer("editRenderArgs");
        appRenderArgs._headPose = getHMDSensorPose();

        auto myAvatar = getMyAvatar();

        // update the avatar with a fresh HMD pose
        {
            PROFILE_RANGE(render, "/updateAvatar");
            myAvatar->updateFromHMDSensorMatrix(appRenderArgs._headPose);
        }

        auto lodManager = DependencyManager::get<LODManager>();

        float sensorToWorldScale = getMyAvatar()->getSensorToWorldScale();
        appRenderArgs._sensorToWorldScale = sensorToWorldScale;
        appRenderArgs._sensorToWorld = getMyAvatar()->getSensorToWorldMatrix();
        {
            PROFILE_RANGE(render, "/buildFrustrumAndArgs");
            {
                // adjust near clip plane to account for sensor scaling. This is rebuilt every frame, so the near
                // clip setting (Render.nearClip, desktop only: an HMD keeps the short one for the hands) is read here
                float aspectRatio = getActiveDisplayPlugin()->getRecommendedAspectRatio();
                float fovY = verticalFieldOfView(_fieldOfView.get(), aspectRatio);
                float nearClip = isHMDMode() ? DEFAULT_NEAR_CLIP : getNearClip();
                auto adjustedProjection = glm::perspective(
                    glm::radians(fovY), aspectRatio, nearClip * sensorToWorldScale, getFarClip()
                );
                _viewState.setProjection(adjustedProjection);
            }
            appRenderArgs._renderArgs = RenderArgs(_graphicsEngine.getGPUContext(), lodManager->getVisibilityDistance(),
                lodManager->getBoundaryLevelAdjust(), lodManager->getLODHalfAngleTan(), RenderArgs::DEFAULT_RENDER_MODE,
                RenderArgs::MONO, RenderArgs::DEFERRED, RenderArgs::RENDER_DEBUG_NONE);
            appRenderArgs._renderArgs._scene = getMain3DScene();
            appRenderArgs._renderArgs._impostorAngleHalfTanSq = lodManager->getImpostorAngleHalfTanSq();
            appRenderArgs._renderArgs._impostorDebug = lodManager->getImpostorDebug();
            appRenderArgs._renderArgs._impostorCrossfade = lodManager->getImpostorCrossfade();
            appRenderArgs._renderArgs._impostorMarch = lodManager->getImpostorMarch();
            appRenderArgs._renderArgs._impostorMarchFootprints = lodManager->getImpostorMarchFootprints();
            appRenderArgs._renderArgs._meshLodErrorPixels = lodManager->getMeshLodErrorPixelsInEffect();
            appRenderArgs._renderArgs._meshLodForceLevel = lodManager->getMeshLodForceLevel();
            // GPU instancing (INSTANCING.md 3.6): Developer > Render > GPU Instancing, for comparing on and off (the
            // first call comes from the constructor, before the menu exists: on then)
            auto menu = Menu::getInstance();
            appRenderArgs._renderArgs._enableInstancing = !menu || menu->isOptionChecked(MenuOption::GpuInstancing);

            {
                QMutexLocker viewLocker(&_viewState.mutex());
                appRenderArgs._renderArgs.setViewFrustum(_viewState.viewFrustum());
            }
        }
        {
            PROFILE_RANGE(render, "/resizeGL");
            bool showWarnings = false;
            bool suppressShortTimings = false;
            auto menu = Menu::getInstance();
            if (menu) {
                suppressShortTimings = menu->isOptionChecked(MenuOption::SuppressShortTimings);
                showWarnings = menu->isOptionChecked(MenuOption::PipelineWarnings);
            }
            PerformanceWarning::setSuppressShortTimings(suppressShortTimings);
            PerformanceWarning warn(showWarnings, "Application::paintGL()");
            _display.resizeGL();
        }

        _viewState.updateCamera(appRenderArgs._renderArgs, deltaTime);
        appRenderArgs._eyeToWorld = _viewState.camera().getTransform();
        appRenderArgs._isStereo = false;

        {
            auto hmdInterface = DependencyManager::get<HMDScriptingInterface>();
            float ipdScale = hmdInterface->getIPDScale();

            // scale IPD by sensorToWorldScale, to make the world seem larger or smaller accordingly.
            ipdScale *= sensorToWorldScale;

            auto baseProjection = appRenderArgs._renderArgs.getViewFrustum().getProjection();

            if (getActiveDisplayPlugin()->isStereo()) {
                // Stereo modes will typically have a larger projection matrix overall,
                // so we ask for the 'mono' projection matrix, which for stereo and HMD
                // plugins will imply the combined projection for both eyes.
                //
                // This is properly implemented for the Oculus plugins, but for OpenVR
                // and Stereo displays I'm not sure how to get / calculate it, so we're
                // just relying on the left FOV in each case and hoping that the
                // overall culling margin of error doesn't cause popping in the
                // right eye.  There are FIXMEs in the relevant plugins
                _viewState.camera().setProjection(getActiveDisplayPlugin()->getCullingProjection(baseProjection));
                appRenderArgs._isStereo = true;

                auto& eyeOffsets = appRenderArgs._eyeOffsets;
                auto& eyeProjections = appRenderArgs._eyeProjections;

                // FIXME we probably don't need to set the projection matrix every frame,
                // only when the display plugin changes (or in non-HMD modes when the user
                // changes the FOV manually, which right now I don't think they can.
                for_each_eye([&](Eye eye) {
                    // For providing the stereo eye views, the HMD head pose has already been
                    // applied to the avatar, so we need to get the difference between the head
                    // pose applied to the avatar and the per eye pose, and use THAT as
                    // the per-eye stereo matrix adjustment.
                    mat4 eyeToHead = getActiveDisplayPlugin()->getEyeToHeadTransform(eye);
                    // Grab the translation
                    vec3 eyeOffset = glm::vec3(eyeToHead[3]);
                    // Apply IPD scaling
                    mat4 eyeOffsetTransform = glm::translate(mat4(), eyeOffset * -1.0f * ipdScale);
                    eyeOffsets[eye] = eyeOffsetTransform;
                    eyeProjections[eye] = getActiveDisplayPlugin()->getEyeProjection(eye, baseProjection);
                });

                // Configure the type of display / stereo
                appRenderArgs._renderArgs._displayMode = (isHMDMode() ? RenderArgs::STEREO_HMD : RenderArgs::STEREO_MONITOR);
            }
        }

        appRenderArgs._renderArgs._stencilMaskMode = getActiveDisplayPlugin()->getStencilMaskMode();
        if (appRenderArgs._renderArgs._stencilMaskMode == StencilMaskMode::MESH) {
            appRenderArgs._renderArgs._stencilMaskOperator = getActiveDisplayPlugin()->getStencilMaskMeshOperator();
        }

        {
            QMutexLocker viewLocker(&_viewState.mutex());
            _viewState.loadDisplayViewFrustum();
            appRenderArgs._view = glm::inverse(_viewState.displayViewFrustum().getView());
        }

        {
            QMutexLocker viewLocker(&_viewState.mutex());
            appRenderArgs._renderArgs.setViewFrustum(_viewState.displayViewFrustum());
        }


        // HACK
        // load the view frustum
        // FIXME: This preDisplayRender call is temporary until we create a separate render::scene for the mirror rendering.
        // Then we can move this logic into the Avatar::simulate call.
        myAvatar->preDisplaySide(&appRenderArgs._renderArgs);
    });
}

// The display plug-in's present callback, on its thread: the next idle event posted when none is pending, the next
// render event when the graphics engine has one waiting.
void Application::onPresent(quint32 frameCount, quint64 presentSlot) {
    _presentSlot = presentSlot;
    bool expected = false;
    if (_pendingIdleEvent.compare_exchange_strong(expected, true)) {
        postEvent(this, new QEvent((QEvent::Type)ApplicationEvent::Idle), Qt::HighEventPriority);
    }
    expected = false;
    if (_graphicsEngine.checkPendingRenderEvent() && !isAboutToQuit()) {
        postEvent(_graphicsEngine._renderEventHandler, new QEvent((QEvent::Type)ApplicationEvent::Render));
    }
}
