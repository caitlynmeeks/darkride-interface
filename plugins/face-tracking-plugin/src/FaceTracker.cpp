//
//  FaceTracker.cpp
//  plugins/face-tracking-plugin/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "FaceTracker.h"

#include <climits>
#include <cstring>

#include <QtCore/QFile>
#include <QtCore/QLoggingCategory>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtMultimedia/QAbstractVideoSurface>
#include <QtMultimedia/QCameraInfo>
#include <QtMultimedia/QCameraViewfinderSettings>
#include <QtMultimedia/QVideoSurfaceFormat>
#else
#include <QtCore/QCoreApplication>
#include <QtCore/QPermissions>
#include <QtMultimedia/QCameraDevice>
#include <QtMultimedia/QCameraFormat>
#include <QtMultimedia/QMediaCaptureSession>
#include <QtMultimedia/QMediaDevices>
#include <QtMultimedia/QVideoFrameFormat>
#include <QtMultimedia/QVideoSink>
#endif

#include "CameraPermission.h"

Q_LOGGING_CATEGORY(faceTracking, "darkride.facetracking")

namespace {

const char* MODEL_RESOURCE = ":/face-tracking/face_landmarker.task";
const int MODEL_THREADS = 2;
const int MIN_FRAME_INTERVAL_MS = 25;  // about 30 frames a second, at most, whatever the camera gives
const int PERMISSION_CHECK_MS = 500;
const char* STILL_VARIABLE = "DARKRIDE_FACE_TRACKING_STILL";
const int STILL_FRAME_MS = 33;
// the face needs no more than this, and a smaller frame costs less to copy
const QSize WANTED_RESOLUTION(640, 480);
const qreal MIN_FRAME_RATE = 24.0;

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
const QList<QVideoFrame::PixelFormat> FRAME_FORMATS = {
    QVideoFrame::Format_RGB32,   QVideoFrame::Format_ARGB32, QVideoFrame::Format_BGRA32, QVideoFrame::Format_BGR32,
    QVideoFrame::Format_NV12,    QVideoFrame::Format_NV21,   QVideoFrame::Format_YUV420P, QVideoFrame::Format_YV12,
    QVideoFrame::Format_UYVY,    QVideoFrame::Format_YUYV,   QVideoFrame::Format_RGB24,   QVideoFrame::Format_BGR24,
};
#else
// Windows lists a virtual camera (a headset app's, a streaming app's) as a software device, "\\?\swd#..." (Windows 11's
// virtual cameras) or "\\?\root#...", and a webcam by its bus, "\\?\usb#..."
bool isVirtualCamera(const QCameraDevice& camera) {
    const QString id = QString::fromUtf8(camera.id());
    return id.startsWith("\\\\?\\swd#", Qt::CaseInsensitive) || id.startsWith("\\\\?\\root#", Qt::CaseInsensitive);
}
#endif

}  // namespace

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
// The camera's frames, as they come; nothing is drawn.
class FrameSurface : public QAbstractVideoSurface {
public:
    using Callback = std::function<void(const QVideoFrame& frame, bool bottomToTop)>;

    FrameSurface(Callback callback, QObject* parent) : QAbstractVideoSurface(parent), _callback(callback) {}

    QList<QVideoFrame::PixelFormat> supportedPixelFormats(QAbstractVideoBuffer::HandleType type) const override {
        return type == QAbstractVideoBuffer::NoHandle ? FRAME_FORMATS : QList<QVideoFrame::PixelFormat>();
    }

    bool start(const QVideoSurfaceFormat& format) override {
        // DirectShow's frames come bottom row first
        _bottomToTop = format.scanLineDirection() == QVideoSurfaceFormat::BottomToTop;
        return QAbstractVideoSurface::start(format);
    }

    bool present(const QVideoFrame& frame) override {
        _callback(frame, _bottomToTop);
        return true;
    }

private:
    Callback _callback;
    bool _bottomToTop { false };
};
#endif

FaceTracker::FaceTracker(Listener listener) : _listener(listener) {
}

FaceTracker::~FaceTracker() {
    // no report: whoever listens may be going too
    closeCamera();
}

QList<QPair<QString, QString>> FaceTracker::cameras() {
    QList<QPair<QString, QString>> list;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    for (const QCameraInfo& info : QCameraInfo::availableCameras()) {
        list << qMakePair(info.deviceName(), info.description());
    }
#else
    for (const QCameraDevice& camera : QMediaDevices::videoInputs()) {
        list << qMakePair(QString::fromUtf8(camera.id()), camera.description());
    }
#endif
    return list;
}

void FaceTracker::report(Status status, const QString& detail) {
    Output output;
    output.status = status;
    output.detail = detail;
    output.frame = _frames;
    _listener(output);
}

void FaceTracker::start(const QString& cameraId) {
    closeCamera();
    _cameraId = cameraId;
    _frames = 0;
    report(Status::Starting);

    // the models load only while tracking is on, and are freed when it stops
    QFile file(MODEL_RESOURCE);
    if (!file.open(QIODevice::ReadOnly)) {
        report(Status::ModelFailed, "the face model isn't in this build");
        return;
    }
    auto landmarker = std::make_unique<FaceLandmarker>();
    QString error;
    if (!landmarker->load(file.readAll(), MODEL_THREADS, error)) {
        qCWarning(faceTracking) << "the face model didn't load:" << error;
        report(Status::ModelFailed, error);
        return;
    }
    _landmarker = std::move(landmarker);

    const QString still = qEnvironmentVariable(STILL_VARIABLE);
    if (!still.isEmpty()) {
        _still = QImage(still);
        if (_still.isNull()) {
            report(Status::CameraFailed, "DARKRIDE_FACE_TRACKING_STILL isn't a picture: " + still);
            return;
        }
        qCInfo(faceTracking) << "a still picture in place of the camera:" << still;
        _stillFrames = new QTimer(this);
        connect(_stillFrames, &QTimer::timeout, this, &FaceTracker::processStill);
        _stillFrames->start(STILL_FRAME_MS);
        report(Status::LookingForFace);
        return;
    }

    switch (cameraPermission()) {
        case CameraPermission::Granted:
            openCamera();
            break;
        case CameraPermission::Denied:
            report(Status::Blocked);
            break;
        case CameraPermission::NotAsked:
            report(Status::AskingPermission);
            askCameraPermission();
            _permissionCheck = new QTimer(this);
            connect(_permissionCheck, &QTimer::timeout, this, &FaceTracker::checkPermission);
            _permissionCheck->start(PERMISSION_CHECK_MS);
            break;
    }
}

void FaceTracker::checkPermission() {
    const CameraPermission permission = cameraPermission();
    if (permission == CameraPermission::NotAsked) {
        return;
    }
    _permissionCheck->deleteLater();
    _permissionCheck = nullptr;
    if (permission == CameraPermission::Granted) {
        openCamera();
    } else {
        report(Status::Blocked);
    }
}

void FaceTracker::stop() {
    closeCamera();
    report(Status::Off);
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
void FaceTracker::openCamera() {
    QCameraInfo info;
    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    for (const QCameraInfo& camera : cameras) {
        if (camera.deviceName() == _cameraId) {
            info = camera;
        }
    }
    if (info.isNull()) {
        // none chosen, or the chosen one is gone: the system's default, but a real camera before a virtual one. On
        // Windows the default is whichever DirectShow lists first, and that can be a virtual camera ("@device:sw:...",
        // a headset app's or a streaming app's) ahead of the real webcam ("@device:pnp:...")
        info = QCameraInfo::defaultCamera();
        if (info.deviceName().startsWith("@device:sw:")) {
            for (const QCameraInfo& camera : cameras) {
                if (camera.deviceName().startsWith("@device:pnp:")) {
                    info = camera;
                    break;
                }
            }
        }
    }
    if (info.isNull()) {
        report(Status::NoCamera);
        return;
    }
    _camera = new QCamera(info, this);
    _cameraStarted = false;
    _surface = new FrameSurface([this](const QVideoFrame& frame, bool bottomToTop) { onFrame(frame, bottomToTop); }, this);
    connect(_camera, &QCamera::statusChanged, this, &FaceTracker::onCameraStatus);
    connect(_camera, &QCamera::errorOccurred, this, [this](QCamera::Error) {
        qCWarning(faceTracking) << "the camera failed:" << _camera->errorString();
        report(Status::CameraFailed, _camera->errorString());
    });
    _camera->setCaptureMode(QCamera::CaptureViewfinder);
    _camera->setViewfinder(_surface);
    // loaded first, so it can say which sizes it has; started once it has
    _camera->load();
}

void FaceTracker::onCameraStatus(QCamera::Status status) {
    if (status == QCamera::LoadedStatus && !_cameraStarted) {
        _cameraStarted = true;
        // the size nearest 640 x 480 that keeps up at least 24 frames a second, in a format the surface takes
        QCameraViewfinderSettings best;
        int bestDistance = INT_MAX;
        for (const QCameraViewfinderSettings& settings : _camera->supportedViewfinderSettings()) {
            if (settings.maximumFrameRate() < MIN_FRAME_RATE ||
                (settings.pixelFormat() != QVideoFrame::Format_Invalid && !FRAME_FORMATS.contains(settings.pixelFormat()))) {
                continue;
            }
            const QSize size = settings.resolution();
            const int distance = std::abs(size.width() - WANTED_RESOLUTION.width()) +
                                 std::abs(size.height() - WANTED_RESOLUTION.height());
            if (distance < bestDistance) {
                best = settings;
                bestDistance = distance;
            }
        }
        if (!best.isNull()) {
            qCDebug(faceTracking) << "camera" << best.resolution() << best.maximumFrameRate() << "fps" << best.pixelFormat();
            _camera->setViewfinderSettings(best);
        }
        // started once load() has returned, not from in here: Windows' camera (DirectShow) says Loaded from inside
        // load(), and a start from there is undone when load() finishes (it stops the preview before the first frame)
        QCamera* camera = _camera;
        QTimer::singleShot(0, camera, [camera] { camera->start(); });
    } else if (status == QCamera::ActiveStatus) {
        report(Status::LookingForFace);
    }
}
#else
void FaceTracker::openCamera() {
#if defined(Q_OS_MAC) && QT_CONFIG(permissions)
    // Qt 6's Mac camera starts only when Qt's own permission check says yes, and that check needs Qt's camera permission
    // plugin (QDarwinCameraPermissionPlugin) linked into the app; without it the camera stays off and says nothing
    if (qApp->checkPermission(QCameraPermission()) != Qt::PermissionStatus::Granted) {
        qCWarning(faceTracking) << "the camera is allowed, but Qt's permission check says no: Qt's camera permission"
                                << "plugin isn't linked into this app";
        report(Status::CameraFailed, "Qt's camera permission plugin isn't in this build");
        return;
    }
#endif
    QCameraDevice device;
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    for (const QCameraDevice& camera : cameras) {
        const QString id = QString::fromUtf8(camera.id());
        // a camera chosen on Qt 5 keeps its id on the Mac; on Windows Qt 5's was DirectShow's "@device:pnp:<the path>",
        // and the path is Qt 6's id
        if (id == _cameraId ||
            (_cameraId.startsWith("@device:pnp:") && _cameraId.mid(12).compare(id, Qt::CaseInsensitive) == 0)) {
            device = camera;
        }
    }
    if (device.isNull()) {
        // none chosen, or the chosen one is gone: the system's default, but a real camera before a virtual one
        device = QMediaDevices::defaultVideoInput();
        if (isVirtualCamera(device)) {
            for (const QCameraDevice& camera : cameras) {
                if (!isVirtualCamera(camera)) {
                    device = camera;
                    break;
                }
            }
        }
    }
    if (device.isNull()) {
        report(Status::NoCamera);
        return;
    }
    _camera = new QCamera(device, this);

    // the size nearest 640 x 480 that keeps up at least 24 frames a second; of two such, the one that isn't JPEG (each
    // JPEG frame is decoded). Qt 6 turns any of the camera's formats into a picture
    QCameraFormat best;
    int bestScore = INT_MAX;
    for (const QCameraFormat& format : device.videoFormats()) {
        if (format.maxFrameRate() < MIN_FRAME_RATE) {
            continue;
        }
        const QSize size = format.resolution();
        const int distance = std::abs(size.width() - WANTED_RESOLUTION.width()) +
                             std::abs(size.height() - WANTED_RESOLUTION.height());
        const int score = 2 * distance + (format.pixelFormat() == QVideoFrameFormat::Format_Jpeg ? 1 : 0);
        if (score < bestScore) {
            best = format;
            bestScore = score;
        }
    }
    if (!best.isNull()) {
        qCDebug(faceTracking) << "camera" << best.resolution() << best.maxFrameRate() << "fps" << best.pixelFormat();
        _camera->setCameraFormat(best);
    }

    _sink = new QVideoSink(this);
    _session = new QMediaCaptureSession(this);
    _session->setCamera(_camera);
    _session->setVideoSink(_sink);
    // the frames come on the camera's own thread, and are queued to this one. QVideoFrame::toImage applies the frame's
    // scan-line direction (Windows' bottom-row-first frames), so none is passed on
    connect(_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
        if (_camera) {  // not one queued before the camera closed
            onFrame(frame, false);
        }
    });
    connect(_camera, &QCamera::activeChanged, this, [this](bool active) {
        if (active) {
            report(Status::LookingForFace);
        }
    });
    connect(_camera, &QCamera::errorOccurred, this, [this](QCamera::Error, const QString& message) {
        qCWarning(faceTracking) << "the camera failed:" << message;
        report(Status::CameraFailed, message);
    });
    _camera->start();
}
#endif

void FaceTracker::onFrame(const QVideoFrame& frame, bool bottomToTop) {
    // the latest frame only: one that comes while another is processed replaces the one waiting
    _latest = frame;
    _latestBottomToTop = bottomToTop;
    if (!_processQueued) {
        _processQueued = true;
        QMetaObject::invokeMethod(this, [this] { processLatest(); }, Qt::QueuedConnection);
    }
}

void FaceTracker::processLatest() {
    _processQueued = false;
    if (!_landmarker || !_latest.isValid()) {
        return;
    }
    if (_sinceProcessed.isValid() && _sinceProcessed.elapsed() < MIN_FRAME_INTERVAL_MS) {
        return;
    }
    _sinceProcessed.start();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QImage image = _latest.image();
#else
    QImage image = _latest.toImage();
#endif
    const bool bottomToTop = _latestBottomToTop;
    _latest = QVideoFrame();  // the camera's buffer goes back to it
    if (image.isNull()) {
        return;
    }
    if (bottomToTop) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        image = image.mirrored(false, true);
#else
        image = image.flipped(Qt::Vertical);
#endif
    }
    Output output;
    output.result = _landmarker->process(image);
    output.frame = ++_frames;
    output.status = output.result.face ? Status::Tracking : Status::LookingForFace;
    logFrame(output);
    _listener(output);
}

void FaceTracker::processStill() {
    Output output;
    output.result = _landmarker->process(_still);
    output.frame = ++_frames;
    output.status = output.result.face ? Status::Tracking : Status::LookingForFace;
    logFrame(output);
    _listener(output);
}

void FaceTracker::logFrame(const Output& output) {
    // the first frame and every 900th (about 30 s): whether there's a face, and a few of its weights, for the log
    const quint64 LOG_EVERY = 900;
    if (output.frame % LOG_EVERY != 1) {
        return;
    }
    const auto& weights = output.result.blendshapes;
    auto weight = [&](const char* name) {
        for (int i = 0; i < FaceLandmarker::NUM_BLENDSHAPES; i++) {
            if (strcmp(FaceLandmarker::BLENDSHAPE_NAMES[i], name) == 0) {
                return weights[i];
            }
        }
        return -1.0f;
    };
    qCDebug(faceTracking) << "frame" << output.frame << (output.result.face ? "a face:" : "no face")
                          << "smile" << weight("mouthSmileLeft") << weight("mouthSmileRight") << "jaw" << weight("jawOpen")
                          << "blink" << weight("eyeBlinkLeft") << weight("eyeBlinkRight");
}

void FaceTracker::closeCamera() {
    if (_stillFrames) {
        delete _stillFrames;
        _stillFrames = nullptr;
    }
    _still = QImage();
    if (_permissionCheck) {
        delete _permissionCheck;
        _permissionCheck = nullptr;
    }
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (_camera) {
        _camera->disconnect(this);
        _camera->stop();
        _camera->unload();
        delete _camera;
        _camera = nullptr;
    }
    delete _surface;
    _surface = nullptr;
#else
    if (_camera) {
        _camera->disconnect(this);
        _camera->stop();
    }
    if (_sink) {
        _sink->disconnect(this);
    }
    delete _session;
    _session = nullptr;
    delete _camera;
    _camera = nullptr;
    delete _sink;
    _sink = nullptr;
#endif
    _latest = QVideoFrame();
    _processQueued = false;
    _sinceProcessed.invalidate();
    _landmarker.reset();
}
