//
//  main.cpp
//  interface/src
//
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <atomic>
#include <memory>
#include <thread>

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QSemaphore>
#include <QThreadPool>
#include <QTranslator>
#include <QtNetwork/QSslConfiguration>

#include <BuildInfo.h>
#include <SharedUtil.h>
#include <StartupProfiler.h>
#include <NetworkAccessManager.h>
#include <gl/GLHelpers.h>

#include "AddressManager.h"
#include "Application.h"
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtWebEngineQuick/QtWebEngineQuick>
#endif
#include "ClientOptions.h"
#include "InterfaceLogging.h"
#include "MainWindow.h"
#include "SingleInstance.h"

#include "Profile.h"

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

#ifdef Q_OS_MACOS
#include "MacAutoFill.h"
#endif

namespace {
// Qt's TLS set-up, warmed on a pool thread from the top of main() and joined before the Application is built
struct TlsWarmup {
    QSemaphore done;                   // released when the set-up is over
    std::atomic<bool> started { false };  // the pool has begun it (for the log line, if the join times out)
};
// Generous: the slow case is a cold Windows start, the system's root certificates read for the first time after a boot
const int TLS_WARMUP_JOIN_TIMEOUT_MS = 15000;
}

int main(int argc, const char* argv[]) {
    StartupProfiler::start();
    // Windows and Linux: the interface follows the display's scale (at 150 % on a 4K screen everything was a third of
    // its size), at the exact factor rather than rounded to a whole one; before the QApplication. macOS scales on its
    // own and is unchanged
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt 6: the panels' QML is drawn for Qt 5's default Controls style, which Qt 6 calls "Basic"; left alone, Qt 6 picks
    // the platform's native style (macOS), which ignores our custom backgrounds, ticks and slider handles. A style the
    // user's environment already names is kept. Before the QML engine starts
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_STYLE")) {
        qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    }
    // The docked panels' windows (ui/QmlDock.h) draw on the main thread, as a QQuickWidget does: Qt's threaded loop would
    // make their GL contexts current on a render thread, where macOS traps the context's update on a resize
    // (team/rendering/QT6-R1-ENTRY.md). Here, not at the first dock: setenv racing another thread's getenv isn't safe
    if (!qEnvironmentVariableIsSet("QSG_RENDER_LOOP")) {
        qputenv("QSG_RENDER_LOOP", "basic");
    }
#endif
    StartupProfiler::phase("main: surface format, application name, command line");
#ifdef Q_OS_MACOS
    // before anything of AppKit's runs: AutoFill is set up when the app first becomes active
    disableMacAutoFill();
    // the panels stay ordinary widgets beside the native 3D view: by default Qt makes a native widget's neighbours
    // native too, and Qt won't drag a docked panel that has a native window of its own on macOS (it pops it out where
    // it was docked and ends the drag with a made-up mouse release: the drag Caity lost, 10-27); before any widget
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

    auto format = getDefaultOpenGLSurfaceFormat();
    // Deal with some weirdness in the chromium context sharing on Mac.
    // The primary share context needs to be 3.2, so that the Chromium will
    // succeed in it's creation of it's command stub contexts.  
    format.setVersion(3, 2);
    // This appears to resolve the issues with corrupted fonts on OSX.  No
    // idea why.
    qputenv("QT_ENABLE_GLYPH_CACHE_WORKAROUND", "true");
    // QML's XMLHttpRequest may read and write local files in this Qt (it only warns that a future Qt will refuse):
    // the app's own QML never needs that, and no QML from anywhere else should have it
    qputenv("QML_XHR_ALLOW_FILE_READ", "0");
    qputenv("QML_XHR_ALLOW_FILE_WRITE", "0");
	// https://i.kym-cdn.com/entries/icons/original/000/008/342/ihave.jpg
    QSurfaceFormat::setDefaultFormat(format);
#endif

#if defined(Q_OS_WIN) 
    // Check the minimum version of 
    if (gl::getAvailableVersion() < gl::getRequiredVersion()) {
        MessageBoxA(nullptr, "Darkride requires OpenGL 4.1 or higher", "Unsupported", MB_OK);
        return -1;
    }
#endif

    setupHifiApplication(BuildInfo::INTERFACE_NAME);

    ClientOptions::parse(argc, argv);
    const ClientOptions& options = ClientOptions::get();

    StartupProfiler::phase("main: tracer");
    auto tracer = DependencyManager::set<tracing::Tracer>();
    if (!options.traceFile.isEmpty()) {
        tracer->startTracing();
    }
   
    PROFILE_SYNC_BEGIN(startup, "main startup", "");

    QElapsedTimer startupTime;
    startupTime.start();

    StartupProfiler::phase("main: settings (Setting::init)");
    Setting::init();

    // The TLS backend's first use loads it and the system's root certificates, about 130 ms on this Mac, and it used to
    // fall on the main thread at start-up: inside the keypair request in a fresh home, inside the profile request in a
    // signed-in one. Warmed on a pool thread here, beside the steps below, and joined before the Application is built
    // (below), so no other thread uses TLS while it runs: two threads in that first set-up at once deadlocked a Windows
    // start in Qt 6.11's Schannel backend (11-10, the keypair check and this warm-up each waiting on a lock inside the
    // backend, until the deadlock watchdog ended the process). Nothing between here and the join uses TLS.
    auto tlsWarmup = std::make_shared<TlsWarmup>();
    QThreadPool::globalInstance()->start([tlsWarmup] {
        tlsWarmup->started = true;
        QSslConfiguration::defaultConfiguration();
        tlsWarmup->done.release();
    });



    StartupProfiler::phase("main: single-instance check, scripts path");
    SingleInstance singleInstance(options.allowMultipleInstances, options.hasUrl, options.url);
    if (singleInstance.anotherIsRunning()) {
        return EXIT_SUCCESS;
    }
    // this needs to be done here in main, as the mechanism for setting the
    // scripts directory appears not to work.  See the bug report
    // https://highfidelity.fogbugz.com/f/cases/5759/Issues-changing-scripts-directory-in-ScriptsEngine
    if (!options.scriptsPath.isEmpty()) {
        QDir scriptsPath(options.scriptsPath);
        if (scriptsPath.exists()) {
            PathUtils::defaultScriptsLocation(scriptsPath.path());
        }
    }


    StartupProfiler::phase("main: Application::initPlugins");
    // the preferred and disabled plug-ins, before the Application constructor creates the GL context and activates them
    Application::initPlugins(options);

    int exitCode;
    {
        StartupProfiler::phase("main: running marker, arguments");
        RunningMarker runningMarker(RUNNING_MARKER_FILENAME);
        bool runningMarkerExisted = runningMarker.fileExists();
        runningMarker.writeRunningMarkerFile();

        // Extend argv to enable WebGL rendering
        std::vector<const char*> argvExtended(&argv[0], &argv[argc]);
        argvExtended.push_back("--ignore-gpu-blacklist");
        int argcExtended = (int)argvExtended.size();

        PROFILE_SYNC_END(startup, "main startup", "");
        PROFILE_SYNC_BEGIN(startup, "app full ctor", "");
        StartupProfiler::phase("Application: QApplication, main window, file logger");
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        // Qt 6 wants the web engine initialized before the application object (it sets AA_ShareOpenGLContexts and
        // Chromium's early state; our Qt 5 code skipped it and set Chromium's share context by hand after the GL
        // view existed, ApplicationStartup.cpp:1746). Without it, on macOS 26 every web helper failed its Mach port
        // rendezvous with the browser process ("Permission denied", the helper taking launchd for its parent), while a
        // 20-line QtWebEngineQuick app with this call worked (11-05 runs 15-23). The share context Qt makes at start
        // is the 3.2 core one main sets as the default format, and initializeGL takes an existing global share
        // context as it did when QtWebEngineWidgets made one.
#ifndef Q_OS_MACOS
        // Qt's share context takes the default format of this moment. macOS set it above; elsewhere it was only set in
        // openWindow(), after the application object, which would leave Qt's context on Qt's own default, not core
        QSurfaceFormat::setDefaultFormat(getDefaultOpenGLSurfaceFormat());
#endif
        QtWebEngineQuick::initialize();
#endif
        // The TLS warm-up's join (its start is above): from here on any thread may use TLS. It is normally over by now,
        // and the wait is 0.
        QElapsedTimer tlsJoinTimer;
        tlsJoinTimer.start();
        int tlsJoinStep = StartupProfiler::begin("main: join the TLS warm-up (a wait only while it still runs)");
        const bool tlsWarmedUp = tlsWarmup->done.tryAcquire(1, TLS_WARMUP_JOIN_TIMEOUT_MS);
        StartupProfiler::end(tlsJoinStep);
        const qint64 tlsJoinWaitMs = tlsJoinTimer.elapsed();
        // Nothing but the warm-up has used TLS yet, so a warm-up still running is slow or stuck on its own, not
        // deadlocked with this thread. The main thread goes on; its first TLS use waits on the backend's lock.
        QString tlsJoinTimedOut;  // empty when the join found the warm-up finished
        if (!tlsWarmedUp) {
            tlsJoinTimedOut = QString("TLS warm-up NOT FINISHED after %1 ms (%2): going on without it; the first TLS "
                                      "use on any thread waits for it")
                .arg(tlsJoinWaitMs)
                .arg(tlsWarmup->started ? QString("running, slow or stuck in Qt's TLS set-up on its own")
                                        : QString("never started: the thread pool had not run it"));
            qCWarning(interfaceapp).noquote() << tlsJoinTimedOut;  // the console: the log file comes with the Application
        }
        Application app(argcExtended, const_cast<char**>(argvExtended.data()), startupTime, runningMarkerExisted);
        PROFILE_SYNC_END(startup, "app full ctor", "");
        if (tlsJoinTimedOut.isEmpty()) {
            qCInfo(interfaceapp) << "TLS warm-up joined before the Application was built; the main thread waited"
                                 << tlsJoinWaitMs << "ms";
        } else {
            qCWarning(interfaceapp).noquote() << tlsJoinTimedOut;
        }

        StartupProfiler::phase("main: local server, system information, translator");

        QTimer exitTimer;
        if (options.traceDuration > 0.0f) {
            exitTimer.setSingleShot(true);
            QObject::connect(&exitTimer, &QTimer::timeout, &app, &Application::quit);
            exitTimer.start(int(1000 * options.traceDuration));
        }


        // later launches hand their address to this client (SingleInstance)
        QObject::connect(&singleInstance, &SingleInstance::connected, &app.getDisplay(), &Display::raiseWindow);
        QObject::connect(&singleInstance, &SingleInstance::urlHandedOff, &app.getDomainSession(), &DomainSession::goToHandedOffUrl);
        singleInstance.listen();

        printSystemInformation();

        if (options.hasUrl) {
            app.overrideEntry();
        }
        if (!options.displayName.isEmpty()) {
            app.forceDisplayName(options.displayName);
        }
        if (options.hasTokens) {
            app.forceLoginWithTokens(options.tokens);
        }

        QTranslator translator;
        translator.load("i18n/interface_en");
        app.installTranslator(&translator);
        qCDebug(interfaceapp, "Created QT Application.");
        StartupProfiler::logReport();
        exitCode = app.exec();
        singleInstance.close();

        if (!options.traceFile.isEmpty()) {
            tracer->stopTracing();
            // a relative name lands in Documents (FileUtils::computeDocumentPath), an absolute path where it says
            tracer->serialize(options.traceFile);
        }
    }

    qCDebug(interfaceapp, "Normal exit.");
#if !defined(DEBUG) && !defined(Q_OS_LINUX)
    // HACK: exit immediately (don't handle shutdown callbacks) for Release build
    _exit(exitCode);
#endif
    return exitCode;
}
