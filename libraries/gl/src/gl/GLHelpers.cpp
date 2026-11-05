#include "GLHelpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

#include "Config.h"

#include <QtCore/QObject>
#include <QtCore/QThread>
#include <QtCore/QRegularExpression>
#include <QtCore/QProcessEnvironment>

#include <QtGui/QSurfaceFormat>
#include <QtGui/QOpenGLContext>
#include <QOpenGLDebugLogger>   // QtGui on Qt 5, the QtOpenGL module on Qt 6

#ifdef Q_OS_MACOS
#include <dispatch/dispatch.h>
#endif

#include "Context.h"

size_t evalGLFormatSwapchainPixelSize(const QSurfaceFormat& format) {
    size_t pixelSize = format.redBufferSize() + format.greenBufferSize() + format.blueBufferSize() + format.alphaBufferSize();
    // We don't apply the length of the swap chain into this pixelSize since it is not vsible for the Process (on windows).
    // Let s keep this here remember that:
    // if (format.swapBehavior() > 0) {
    //     pixelSize *= format.swapBehavior(); // multiply the color buffer pixel size by the actual swapchain depth
    // }
    pixelSize += format.stencilBufferSize() + format.depthBufferSize();
    return pixelSize;
}

static bool FORCE_DISABLE_OPENGL_45 = false;

void gl::setDisableGl45(bool disable) {
    FORCE_DISABLE_OPENGL_45 = disable;
}

bool gl::disableGl45() {
#if defined(USE_GLES)
    return false;
#else
    static const QString DEBUG_FLAG("HIFI_DISABLE_OPENGL_45");
    static bool disableOpenGL45 = QProcessEnvironment::systemEnvironment().contains(DEBUG_FLAG);
    return FORCE_DISABLE_OPENGL_45 || disableOpenGL45;
#endif
}

#ifdef Q_OS_MACOS
#define SERIALIZE_GL_RENDERING
#endif

#ifdef SERIALIZE_GL_RENDERING

// This terrible terrible hack brought to you by the complete lack of reasonable
// OpenGL debugging tools on OSX.  Without this serialization code, the UI textures
// frequently become 'glitchy' and get composited onto the main scene in what looks
// like a partially rendered state.
// This looks very much like either state bleeding across the contexts, or bad
// synchronization for the shared OpenGL textures.  However, previous attempts to resolve
// it, even with gratuitous use of glFinish hasn't improved the situation

static std::mutex _globalOpenGLLock;
static std::atomic<int> _globalOpenGLWaiters { 0 };

void gl::globalLock() {
    ++_globalOpenGLWaiters;
    _globalOpenGLLock.lock();
    --_globalOpenGLWaiters;
}

void gl::globalRelease(bool finish) {
    if (finish) {
        glFinish();
    }
    _globalOpenGLLock.unlock();
}

bool gl::globalLockWanted() {
    return _globalOpenGLWaiters.load() > 0;
}

#else

void gl::globalLock() {}
void gl::globalRelease(bool finish) {}
bool gl::globalLockWanted() { return false; }

#endif

// how deep the main thread is in ViewChangeGuards (only the main thread makes them)
static int _viewChangeDepth { 0 };
// until when, on the steady clock in milliseconds, the presenting thread leaves the view alone
static std::atomic<int64_t> _presentationPausedUntil { 0 };
// how the app puts pending window changes in place at once (main thread only)
static std::function<void()> _viewChangeFlush;
// a guard has ended since the presenting thread last looked (takeViewChanged)
static std::atomic<bool> _viewChanged { false };
// for the present-stall report: how many changes began, and the last one's Qt event type
static std::atomic<uint32_t> _viewChangeGuards { 0 };
static std::atomic<int> _viewChangeEventType { 0 };
// the pause after a change: a frame's margin once it is in place; with no way to put it in place at once, long enough
// for it to settle by itself at the end of the main thread's loop pass
static const int64_t PAUSE_AFTER_FLUSH_MS = 20;
static const int64_t PAUSE_WITHOUT_FLUSH_MS = 250;

static int64_t steadyMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool gl::isPresentationPaused() {
#ifdef Q_OS_MACOS
    return steadyMilliseconds() < _presentationPausedUntil.load();
#else
    // the pause is for macOS, which finishes resizing a view's surface after the change (298e8dba84); elsewhere it only
    // held presents off, and a steady run of window events could hold them off indefinitely (a Windows run, 10-21)
    return false;
#endif
}

void gl::setViewChangeFlush(const std::function<void()>& flush) {
    _viewChangeFlush = flush;
}
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)

// how the view's GL context takes the view's new frame, on the main thread (main thread only)
static std::function<void()> _viewChangeContextUpdate;

void gl::setViewChangeContextUpdate(const std::function<void()>& update) {
    _viewChangeContextUpdate = update;
}
#endif

gl::ViewChangeGuard::ViewChangeGuard(int eventType) {
    if (_viewChangeDepth++ == 0) {
        _viewChangeGuards.fetch_add(1, std::memory_order_relaxed);
        _viewChangeEventType.store(eventType, std::memory_order_relaxed);
        // the pause first: a frame waiting for the lock finds it when it gets the lock
        _presentationPausedUntil = steadyMilliseconds() + PAUSE_WITHOUT_FLUSH_MS;
        globalLock();
    }
}

gl::ViewChangeGuard::~ViewChangeGuard() {
    // the change is put in place while the presenting thread is still held off; the depth stays above zero meanwhile,
    // so a guard made by what the flush sets off nests rather than locking again
    if (_viewChangeDepth == 1 && _viewChangeFlush) {
        _viewChangeFlush();
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
    // the change is in place: the context takes the view's new frame here, on the main thread, while the present
    // thread is still held off (Qt 5: queued by Qt from the present thread's next makeCurrent)
    if (_viewChangeDepth == 1 && _viewChangeContextUpdate) {
        _viewChangeContextUpdate();
    }
#endif
    if (--_viewChangeDepth == 0) {
        _presentationPausedUntil = steadyMilliseconds() + (_viewChangeFlush ? PAUSE_AFTER_FLUSH_MS : PAUSE_WITHOUT_FLUSH_MS);
        _viewChanged = true;
        globalRelease(false);   // the main thread has no context of its own to finish
    }
}

gl::ViewChangeState gl::viewChangeState() {
    ViewChangeState state;
    state.guards = _viewChangeGuards.load(std::memory_order_relaxed);
    state.lastEventType = _viewChangeEventType.load(std::memory_order_relaxed);
    state.pauseLeftMs = std::max<int64_t>(0, _presentationPausedUntil.load() - steadyMilliseconds());
    return state;
}

bool gl::takeViewChanged() {
    return _viewChanged.exchange(false);
}

void gl::waitForMainQueue(int milliseconds) {
#ifdef Q_OS_MACOS
    // a marker behind what is queued: the queue runs in order, so its signal means all before it has run. The block
    // holds its own reference to the semaphore, so a wait that times out leaves it nothing freed to signal.
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_retain(done);
    dispatch_async_f(dispatch_get_main_queue(), done, [](void* context) {
        auto semaphore = (dispatch_semaphore_t)context;
        dispatch_semaphore_signal(semaphore);
        dispatch_release(semaphore);
    });
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)milliseconds * (int64_t)NSEC_PER_MSEC));
    dispatch_release(done);
#endif
}


uint16_t gl::getTargetVersion() {
    uint8_t major = 0, minor = 0;

#if defined(USE_GLES)
    major = 3;
    minor = 2;
#elif defined(Q_OS_MACOS)
    major = 4;
    minor = 1;
#else
    major = 4;
    minor = disableGl45() ? 1 : 5;
#endif
    return GL_MAKE_VERSION(major, minor);
}

uint16_t gl::getRequiredVersion() {
    uint8_t major = 0, minor = 0;
#if defined(USE_GLES)
    major = 3;
    minor = 2;
#else 
    major = 4;
    minor = 1;
#endif
    return GL_MAKE_VERSION(major, minor);
}

#if defined(Q_OS_WIN)

typedef BOOL(APIENTRYP PFNWGLCHOOSEPIXELFORMATARBPROC)(HDC hdc, const int *piAttribIList, const FLOAT *pfAttribFList, UINT nMaxFormats, int *piFormats, UINT *nNumFormats);
typedef HGLRC(APIENTRYP PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC hDC, HGLRC hShareContext, const int *attribList);
GLAPI PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB;
GLAPI PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB;

static bool setupPixelFormatSimple(HDC hdc) {
    // FIXME build the PFD based on the 
    static const PIXELFORMATDESCRIPTOR pfd =    // pfd Tells Windows How We Want Things To Be
    {
        sizeof(PIXELFORMATDESCRIPTOR),         // Size Of This Pixel Format Descriptor
        1,                                      // Version Number
        PFD_DRAW_TO_WINDOW |                    // Format Must Support Window
        PFD_SUPPORT_OPENGL |                    // Format Must Support OpenGL
        PFD_DOUBLEBUFFER,                       // Must Support Double Buffering
        PFD_TYPE_RGBA,                          // Request An RGBA Format
        24,                                     // Select Our Color Depth
        0, 0, 0, 0, 0, 0,                       // Color Bits Ignored
        1,                                      // Alpha Buffer
        0,                                      // Shift Bit Ignored
        0,                                      // No Accumulation Buffer
        0, 0, 0, 0,                             // Accumulation Bits Ignored
        24,                                     // 24 Bit Z-Buffer (Depth Buffer)  
        8,                                      // 8 Bit Stencil Buffer
        0,                                      // No Auxiliary Buffer
        PFD_MAIN_PLANE,                         // Main Drawing Layer
        0,                                      // Reserved
        0, 0, 0                                 // Layer Masks Ignored
    };
    auto pixelFormat = ChoosePixelFormat(hdc, &pfd);
    if (pixelFormat == 0) {
        return false;
    }

    if (SetPixelFormat(hdc, pixelFormat, &pfd) == FALSE) {
        return false;
    }
    return true;
}

#endif


const gl::ContextInfo& gl::ContextInfo::get(bool init) {
    static gl::ContextInfo INSTANCE;
    if (init) {
        static std::once_flag once;
        std::call_once(once, [&] {
            INSTANCE.init();
        });
    }
    return INSTANCE;
}

gl::ContextInfo& gl::ContextInfo::init() {
    if (glGetString) {
        version = (const char*)glGetString(GL_VERSION);
        shadingLanguageVersion = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
        vendor = (const char*)glGetString(GL_VENDOR);
        renderer = (const char*)glGetString(GL_RENDERER);
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; ++i) {
            extensions.emplace_back((const char*)glGetStringi(GL_EXTENSIONS, i));
        }
    }
    return *this;
}

uint16_t gl::getAvailableVersion() {
    static uint8_t major = 0, minor = 0;
    static std::once_flag once;
    std::call_once(once, [&] {
#if defined(USE_GLES)
        // FIXME do runtime detection of the available GL version
        major = 3;
        minor = 2;
#elif defined(Q_OS_MACOS)
        // Damn it Apple.
        major = 4;
        minor = 1;
#elif defined(Q_OS_WIN)
        // 
        HINSTANCE hInstance = GetModuleHandle(nullptr);
        const auto windowClassName = "OpenGLVersionCheck";
        WNDCLASS wc = { };
        wc.lpfnWndProc   = DefWindowProc;
        wc.hInstance     = hInstance;
        wc.lpszClassName = windowClassName;
        RegisterClass(&wc);

        using Handle = std::shared_ptr<void>;
        HWND rawHwnd = CreateWindowEx(
            WS_EX_APPWINDOW, // extended style 
            windowClassName, // class name
            windowClassName, // title
            WS_CLIPSIBLINGS | WS_CLIPCHILDREN | CS_OWNDC | WS_POPUP, // style
            0, 0, 10, 10,   // position and size
            NULL, NULL, hInstance, NULL);
        auto WindowDestroyer = [](void* handle) {
            DestroyWindow((HWND)handle);
        };
        Handle hWnd = Handle(rawHwnd, WindowDestroyer);
        if (!hWnd) {
            return;
        }
        HDC rawDC = GetDC(rawHwnd);
        auto DCDestroyer = [=](void* handle) {
            ReleaseDC(rawHwnd, (HDC)handle);
        };
        if (!rawDC) {
            return;
        }
        Handle hDC = Handle(rawDC, DCDestroyer);
        if (!setupPixelFormatSimple(rawDC)) {
            return;
        }
        auto GLRCDestroyer = [](void* handle) {
            wglDeleteContext((HGLRC)handle);
        };
        auto rawglrc = wglCreateContext(rawDC);
        if (!rawglrc) {
            return;
        }
        Handle hGLRC = Handle(rawglrc, GLRCDestroyer);
        if (!wglMakeCurrent(rawDC, rawglrc)) {
            return;
        }
        gl::initModuleGl();

        std::string glvendor{ (const char*)glGetString(GL_VENDOR) };
        std::transform(glvendor.begin(), glvendor.end(), glvendor.begin(), ::tolower); 

        // Intel has *notoriously* buggy DSA implementations, especially around cubemaps
        if (std::string::npos != glvendor.find("intel")) {
            gl::setDisableGl45(true);
        }

        wglMakeCurrent(0, 0);
        hGLRC.reset();
        if (!wglChoosePixelFormatARB || !wglCreateContextAttribsARB) {
            return;
        }

        // The only two versions we care about on Windows 
        // are 4.5 and 4.1
        if (GLAD_GL_VERSION_4_5) {
            major = 4;
            minor = disableGl45() ? 1 : 5;
        } else if (GLAD_GL_VERSION_4_1) {
            major = 4;
            minor = 1;
        }
#else
        // FIXME do runtime detection of GL version on non-Mac/Windows/Mobile platforms
        major = 4;
        minor = disableGl45() ? 1 : 5;
#endif
    });
    return GL_MAKE_VERSION(major, minor);
}

const QSurfaceFormat& getDefaultOpenGLSurfaceFormat() {
    static QSurfaceFormat format;
    static std::once_flag once;
    std::call_once(once, [] {
#if defined(USE_GLES)
        format.setRenderableType(QSurfaceFormat::OpenGLES);
        format.setRedBufferSize(8);
        format.setGreenBufferSize(8);
        format.setBlueBufferSize(8);
        format.setAlphaBufferSize(8);
#else
        format.setProfile(QSurfaceFormat::OpenGLContextProfile::CoreProfile);
#endif
        if (gl::Context::enableDebugLogger()) {
            format.setOption(QSurfaceFormat::DebugContext);
        }
        // Qt Quick may need a depth and stencil buffer. Always make sure these are available.
        format.setDepthBufferSize(DEFAULT_GL_DEPTH_BUFFER_BITS);
        format.setStencilBufferSize(DEFAULT_GL_STENCIL_BUFFER_BITS);
        auto glversion = ::gl::getTargetVersion();
        format.setMajorVersion(GL_GET_MAJOR_VERSION(glversion));
        format.setMinorVersion(GL_GET_MINOR_VERSION(glversion));
    });
    return format;
}

int glVersionToInteger(QString glVersion) {
    QStringList versionParts = glVersion.split(QRegularExpression("[\\.\\s]"));
    int majorNumber = 0, minorNumber = 0;
    if (versionParts.size() >= 2) {
        majorNumber = versionParts[0].toInt();
        minorNumber = versionParts[1].toInt();
    }
    return (majorNumber << 16) | minorNumber;
}

QThread* RENDER_THREAD = nullptr;

bool isRenderThread() {
    return QThread::currentThread() == RENDER_THREAD;
}

#if defined(Q_OS_ANDROID)
#define USE_GLES 1
#endif

namespace gl {
    void withSavedContext(const std::function<void()>& f) {
        // Save the original GL context, because creating a QML surface will create a new context
        QOpenGLContext * savedContext = QOpenGLContext::currentContext();
        QSurface * savedSurface = savedContext ? savedContext->surface() : nullptr;
        f();
        if (savedContext) {
            savedContext->makeCurrent(savedSurface);
        }
    }

    bool checkGLError(const char* name) {
        GLenum error = glGetError();
        if (!error) {
            return false;
        } 
        switch (error) {
            case GL_INVALID_ENUM:
                qCWarning(glLogging) << "GLBackend" << name << ": An unacceptable value is specified for an enumerated argument.The offending command is ignored and has no other side effect than to set the error flag.";
                break;
            case GL_INVALID_VALUE:
                qCWarning(glLogging) << "GLBackend" << name << ": A numeric argument is out of range.The offending command is ignored and has no other side effect than to set the error flag";
                break;
            case GL_INVALID_OPERATION:
                qCWarning(glLogging) << "GLBackend" << name << ": The specified operation is not allowed in the current state.The offending command is ignored and has no other side effect than to set the error flag..";
                break;
            case GL_INVALID_FRAMEBUFFER_OPERATION:
                qCWarning(glLogging) << "GLBackend" << name << ": The framebuffer object is not complete.The offending command is ignored and has no other side effect than to set the error flag.";
                break;
            case GL_OUT_OF_MEMORY:
                qCWarning(glLogging) << "GLBackend" << name << ": There is not enough memory left to execute the command.The state of the GL is undefined, except for the state of the error flags, after this error is recorded.";
                break;
#if !defined(USE_GLES)
            case GL_STACK_UNDERFLOW:
                qCWarning(glLogging) << "GLBackend" << name << ": An attempt has been made to perform an operation that would cause an internal stack to underflow.";
                break;
            case GL_STACK_OVERFLOW:
                qCWarning(glLogging) << "GLBackend" << name << ": An attempt has been made to perform an operation that would cause an internal stack to overflow.";
                break;
#endif
        }
        return true;
    }


    bool checkGLErrorDebug(const char* name) {
        // Disabling error checking macro on Android debug builds for now, 
        // as it throws off performance testing, which must be done on 
        // Debug builds
#if defined(DEBUG) && !defined(Q_OS_ANDROID)
        return checkGLError(name);
#else
        Q_UNUSED(name);
        return false;
#endif
    }

    // Enables annotation of captures made by tools like renderdoc
    bool khrDebugEnabled() {
        static std::once_flag once;
        static bool khrDebug = false;
        std::call_once(once, [&] {
            khrDebug = nullptr != glPushDebugGroupKHR;
        });
        return khrDebug;
    }

    // Enables annotation of captures made by tools like renderdoc
    bool extDebugMarkerEnabled() {
        static std::once_flag once;
        static bool extMarker = false;
        std::call_once(once, [&] {
            extMarker = nullptr != glPushGroupMarkerEXT;
        });
        return extMarker;
    }

    bool debugContextEnabled() {
#if defined(Q_OS_MACOS)
        // OSX does not support GL_KHR_debug or GL_ARB_debug_output
        static bool enableDebugLogger = false;
#elif defined(DEBUG) || defined(USE_GLES)
        //static bool enableDebugLogger = true;
        static bool enableDebugLogger = false;
#else
        static const QString DEBUG_FLAG("HIFI_DEBUG_OPENGL");
        static bool enableDebugLogger = QProcessEnvironment::systemEnvironment().contains(DEBUG_FLAG);
#endif
        return enableDebugLogger;
    }
}
