//
//  AvatarThumbnails.cpp
//  interface/src/avatar
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "AvatarThumbnails.h"

#include <memory>

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFutureWatcher>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

#include <AccountManager.h>
#include <GLMHelpers.h>
#include <NetworkingConstants.h>
#include <PathUtils.h>
#include <ViewFrustum.h>

#include "Application.h"
#include "AvatarManager.h"
#include "InterfaceLogging.h"
#include "MainWindow.h"
#include "MyAvatar.h"
#include "SecondaryCamera.h"
#include "editor/EditorCreate.h"

namespace {
    const float FIELD_OF_VIEW = 30.0f;       // degrees, vertical: a portrait lens
    const float FRAME = 0.6f;                // of the avatar's height in the picture: the top of the head to the waist
    const float NEAR_CLIP = 0.05f;
    const int LOAD_POLL_MS = 250;
    const int LOAD_LIMIT_MS = 20000;
    const int SETTLE_MS = 1500;              // after an avatar just put on has loaded: its textures
    const int CAMERA_SETTLE_MS = 300;        // the camera's new place and size reach the render
    const int JPEG_QUALITY = 88;
    // automatic pictures (autoPictureFor)
    const int AUTO_FIRST_MS = 4000;          // after the avatar worn has loaded: its textures, and you, settle
    const int AUTO_POLL_MS = 2000;           // then tried again until you've settled
    const int AUTO_TRIES = 60;               // two minutes of that, then left until it's worn again
    const float AUTO_STILL_SPEED = 0.2f;     // m/s: standing, not walking or carried by something moving
    const int AUTO_KEPT = 300;               // pictures kept; the oldest go (about 40 KB each)
    const int AUTO_FRAMES = 6;               // frames rendered with the secondary camera on before the picture
    const int FRAME_POLL_MS = 4;             // how often those frames are counted
    const qint64 AUTO_AFTER_PHYSICS_MS = 20000;  // after physics turns on: an arrival's loading tail (performance)
    const char* AUTO_INDEX = "index.json";

    void answer(QJSValue& callback, const QString& error, const QString& url) {
        if (callback.isCallable()) {
            callback.call({ error.isEmpty() ? QJSValue(QJSValue::NullValue) : QJSValue(error), QJSValue(url) });
        }
    }

    // the avatar worn has loaded the model it asks for
    bool avatarLoaded() {
        auto myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
        auto skeleton = myAvatar ? myAvatar->getSkeletonModel() : nullptr;
        return skeleton && skeleton->getURL() == myAvatar->getSkeletonModelURL() && skeleton->isLoaded();
    }
}

QString AvatarThumbnails::pictureName(const QString& name) {
    return EditorUpload::safeFileName(name + ".preview.jpg");
}

AvatarThumbnails::AvatarThumbnails() {
    _autoTimer = new QTimer(this);
    _autoTimer->setSingleShot(true);
    connect(_autoTimer, &QTimer::timeout, this, &AvatarThumbnails::tryAutoPicture);
    // the avatar worn (made with the avatar manager, before this) says when a model and its animations have loaded
    auto avatarManager = DependencyManager::get<AvatarManager>();
    auto myAvatar = avatarManager ? avatarManager->getMyAvatar() : nullptr;
    if (myAvatar) {
        connect(myAvatar.get(), &MyAvatar::onLoadComplete, this, [this]() {
            auto myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
            if (myAvatar) {
                autoPictureFor(myAvatar->getFullAvatarURLFromPreferences().toString());
            }
        }, Qt::QueuedConnection);
    }
}

QByteArray AvatarThumbnails::fit(const QImage& image) {
    if (image.isNull()) {
        return QByteArray();
    }
    const QImage scaled = image.scaled(WIDTH, HEIGHT, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QImage cropped = scaled.copy((scaled.width() - WIDTH) / 2, (scaled.height() - HEIGHT) / 2, WIDTH, HEIGHT);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    cropped.convertToFormat(QImage::Format_RGB888).save(&buffer, "JPG", JPEG_QUALITY);
    return jpeg;
}

void AvatarThumbnails::capture(Done done) {
    if (avatarLoaded()) {
        shoot(done);
        return;
    }
    // an avatar just put on (the importer's Wear & Keep): its model, then a moment for its textures
    auto waited = std::make_shared<int>(0);
    auto timer = new QTimer(this);
    timer->setInterval(LOAD_POLL_MS);
    connect(timer, &QTimer::timeout, this, [this, timer, waited, done]() {
        *waited += LOAD_POLL_MS;
        const bool loaded = avatarLoaded();
        if (!loaded && *waited < LOAD_LIMIT_MS) {
            return;
        }
        timer->stop();
        timer->deleteLater();
        if (!loaded) {
            done(QByteArray(), "the avatar didn't finish loading");
            return;
        }
        QTimer::singleShot(SETTLE_MS, this, [this, done]() { shoot(done); });
    });
    timer->start();
}

void AvatarThumbnails::shoot(Done done, int frames) {
    auto renderConfig = qApp->getRenderEngine()->getConfiguration();
    auto camera = renderConfig ? dynamic_cast<SecondaryCameraJobConfig*>(renderConfig->getConfig("SecondaryCamera")) : nullptr;
    auto myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    if (_taking || !camera || !myAvatar) {
        done(QByteArray(), _taking ? "a picture is being taken" : "there's no camera");
        return;
    }
    _taking = true;

    // what the camera was doing (the spectator camera may be on), put back after the picture
    struct Was {
        bool enabled;
        QUuid attached;
        QUuid portalEntrance;
        glm::vec3 position;
        glm::quat orientation;
        float vFoV;
        float nearClip;
        float farClip;
        int width;
        int height;
        bool mirror;
        bool portal;
    };
    auto was = std::make_shared<Was>(Was { camera->isEnabled(), camera->attachedEntityId, camera->portalEntranceEntityId,
                                           camera->position, camera->orientation, camera->vFoV,
                                           camera->nearClipPlaneDistance, camera->farClipPlaneDistance,
                                           camera->textureWidth, camera->textureHeight, camera->mirrorProjection,
                                           camera->portalProjection });

    // in front of you, facing you, a little above the chest, the head to the waist in the frame: the head joint sits
    // at the neck, the top of the head about an eighth of the height above it, so the frame's middle is a tenth below
    const float height = myAvatar->getHeight();
    glm::vec3 forward = myAvatar->getWorldOrientation() * Vectors::FRONT;
    forward.y = 0.0f;
    forward = glm::length(forward) > 0.01f ? glm::normalize(forward) : Vectors::FRONT;
    const glm::vec3 target = myAvatar->getHeadPosition() - Vectors::UNIT_Y * (0.1f * height);
    const float distance = (FRAME * height * 0.5f) / tanf(glm::radians(FIELD_OF_VIEW * 0.5f));
    const glm::vec3 position = target + forward * distance + Vectors::UNIT_Y * (0.05f * height);
    const glm::vec3 look = glm::normalize(target - position);
    // a camera looks down -z: turn it to the look's heading, then tilt it to the look's pitch
    const glm::quat orientation = glm::angleAxis(atan2f(-look.x, -look.z), Vectors::UNIT_Y) *
                                  glm::angleAxis(asinf(glm::clamp(look.y, -1.0f, 1.0f)), Vectors::UNIT_X);

    camera->attachedEntityId = QUuid();
    camera->portalEntranceEntityId = QUuid();
    camera->mirrorProjection = false;
    camera->portalProjection = false;
    camera->vFoV = FIELD_OF_VIEW;
    camera->nearClipPlaneDistance = NEAR_CLIP;
    camera->farClipPlaneDistance = DEFAULT_FAR_CLIP;
    camera->setPosition(position);
    camera->setOrientation(orientation);
    camera->resetSizeSpectatorCamera(WIDTH, HEIGHT);
    camera->enableSecondaryCameraRenderConfigs(true);

    // the picture once the camera's new place and size have reached the render: after CAMERA_SETTLE_MS, or after a
    // number of rendered frames (an automatic picture: as few frames of the scene drawn twice as will do, performance)
    auto request = [this, camera, was, done]() {
        qApp->getSnapshotRequests().add(std::make_tuple([this, camera, was, done](const QImage& image) {
            // on the display's thread: the rest on the main one
            const QImage picture = image.copy();
            qApp->postLambdaEvent([this, camera, was, done, picture]() {
                camera->attachedEntityId = was->attached;
                camera->portalEntranceEntityId = was->portalEntrance;
                camera->mirrorProjection = was->mirror;
                camera->portalProjection = was->portal;
                camera->vFoV = was->vFoV;
                camera->nearClipPlaneDistance = was->nearClip;
                camera->farClipPlaneDistance = was->farClip;
                camera->setPosition(was->position);
                camera->setOrientation(was->orientation);
                camera->resetSizeSpectatorCamera(was->width, was->height);
                camera->enableSecondaryCameraRenderConfigs(was->enabled);
                _taking = false;
                // scaled and made a JPEG on a worker thread: tens of milliseconds for a whole window's picture
                auto watcher = new QFutureWatcher<QByteArray>(this);
                connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [watcher, done]() {
                    const QByteArray jpeg = watcher->result();
                    watcher->deleteLater();
                    done(jpeg, jpeg.isEmpty() ? "the camera gave no picture" : QString());
                });
                watcher->setFuture(QtConcurrent::run([picture]() { return AvatarThumbnails::fit(picture); }));
            });
        }, 0.0f, false));
    };
    if (frames <= 0) {
        QTimer::singleShot(CAMERA_SETTLE_MS, this, request);
        return;
    }
    const size_t until = qApp->getRenderFrameCount() + (size_t)frames;
    const qint64 giveUpAt = QDateTime::currentMSecsSinceEpoch() + CAMERA_SETTLE_MS;
    auto poll = new QTimer(this);
    poll->setInterval(FRAME_POLL_MS);
    connect(poll, &QTimer::timeout, this, [poll, until, giveUpAt, request]() {
        // the frames drawn, or CAMERA_SETTLE_MS when they don't come (a render held back)
        if (qApp->getRenderFrameCount() >= until || QDateTime::currentMSecsSinceEpoch() >= giveUpAt) {
            poll->stop();
            poll->deleteLater();
            request();
        }
    });
    poll->start();
}

QString AvatarThumbnails::keep(const QByteArray& jpeg, QString& error) const {
    // in the app's data folder, like the editor's thumbnails, so a client given its own (TIVOLI_APP_DATA_DIR, a
    // walled-off test client) keeps its pictures there and not among the main installation's
    QDir folder(PathUtils::getAppLocalDataPath() + "avatar-thumbnails");
    if (!folder.mkpath(".")) {
        error = "couldn't make a folder for the picture";
        return QString();
    }
    const QString path = folder.filePath(QString::number(QDateTime::currentMSecsSinceEpoch()) + ".jpg");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(jpeg) != jpeg.size()) {
        error = "couldn't keep the picture";
        return QString();
    }
    return path;
}

void AvatarThumbnails::take(QJSValue callback) {
    capture([this, callback](const QByteArray& jpeg, const QString& error) mutable {
        if (!error.isEmpty()) {
            answer(callback, error, QString());
            return;
        }
        QString problem;
        const QString path = keep(jpeg, problem);
        answer(callback, problem, path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString());
    });
}

void AvatarThumbnails::fromFile(const QString& path, QJSValue callback) {
    const QUrl url(path);
    const QString localPath = url.isLocalFile() ? url.toLocalFile() : path;
    const QImage image(localPath);
    if (image.isNull()) {
        answer(callback, "that file isn't a picture this can read", QString());
        return;
    }
    QString problem;
    const QString kept = keep(fit(image), problem);
    answer(callback, problem, kept.isEmpty() ? QString() : QUrl::fromLocalFile(kept).toString());
}

QVariantMap AvatarThumbnails::myFilesPlace(const QString& fstUrl) const {
    auto accountManager = DependencyManager::get<AccountManager>();
    const QString me = accountManager->getAccountInfo().getUsername().toLower();
    if (me.isEmpty() || !accountManager->isLoggedIn()) {
        return QVariantMap();
    }
    // tea://<user>/<path>, or the files host's https://files.<metaverse host>/<user>/<path>
    const QUrl url(fstUrl);
    QString path;
    if (url.scheme() == "tea") {
        if (url.host().toLower() != me) {
            return QVariantMap();
        }
        path = url.path();
    } else if (url.scheme() == "https" || url.scheme() == "http") {
        const QString filesHost = "files." + NetworkingConstants::METAVERSE_SERVER_URL().host();
        if (url.host().toLower() != filesHost.toLower() || url.path().section('/', 1, 1).toLower() != me) {
            return QVariantMap();
        }
        path = "/" + url.path().section('/', 2);
    } else {
        return QVariantMap();
    }
    if (!path.endsWith(".fst", Qt::CaseInsensitive)) {
        return QVariantMap();
    }
    QVariantMap place;
    place["folder"] = path.section('/', 0, -2);
    place["name"] = path.section('/', -1).chopped(4);
    return place;
}

QString AvatarThumbnails::pictureKey(const QString& fstUrl) const {
    const QVariantMap place = myFilesPlace(fstUrl);
    return place.isEmpty() ? QString() : place["folder"].toString() + "/" + pictureName(place["name"].toString());
}

void AvatarThumbnails::upload(const QString& pictureUrl, const QString& fstUrl, QJSValue callback) {
    const QVariantMap place = myFilesPlace(fstUrl);
    if (place.isEmpty()) {
        answer(callback, "this avatar isn't in your My Files", QString());
        return;
    }
    const QUrl url(pictureUrl);
    QFile file(url.isLocalFile() ? url.toLocalFile() : pictureUrl);
    if (!file.open(QIODevice::ReadOnly)) {
        answer(callback, "couldn't read the picture", QString());
        return;
    }
    auto upload = new EditorUpload(this);
    QString error;
    if (!upload->startData(file.readAll(), pictureName(place["name"].toString()), place["folder"].toString(), "image/jpeg",
                           error)) {
        delete upload;
        answer(callback, error, QString());
        return;
    }
    connect(upload, &EditorUpload::failed, this, [upload, callback](const QString& error) mutable {
        upload->deleteLater();
        answer(callback, error, QString());
    });
    connect(upload, &EditorUpload::finished, this, [upload, callback](const QString& url) mutable {
        upload->deleteLater();
        answer(callback, QString(), url);
    });
}

// ---- automatic pictures -------------------------------------------------------------------------------------------

QString AvatarThumbnails::autoFolder() const {
    // the app's data folder, as the pictures taken above (a walled-off test client keeps its own), one folder per
    // account: on a shared computer each person sees only the avatars they wore (security, 11-08, as chat's data);
    // the signed-in account's name hashed, or "signed-out"
    auto accountManager = DependencyManager::get<AccountManager>();
    const QString username = accountManager && accountManager->isLoggedIn()
        ? accountManager->getAccountInfo().getUsername().toLower() : QString();
    const QString who = username.isEmpty() ? QString("signed-out")
        : QString::fromLatin1(QCryptographicHash::hash(username.toUtf8(), QCryptographicHash::Sha1).toHex());
    return PathUtils::getAppLocalDataPath() + "avatar-pictures/" + who;
}

const QVariantMap& AvatarThumbnails::autoIndex() const {
    // read again when the account (its folder) changed
    const QString folder = autoFolder();
    if (folder != _autoIndexFolder) {
        _autoIndexFolder = folder;
        _autoIndex.clear();
        QFile file(QDir(folder).filePath(AUTO_INDEX));
        if (file.open(QIODevice::ReadOnly)) {
            _autoIndex = QJsonDocument::fromJson(file.readAll()).object().toVariantMap();
        }
    }
    return _autoIndex;
}

QVariantMap AvatarThumbnails::autoPictures() const {
    QVariantMap pictures;
    const QDir folder(autoFolder());
    const QVariantMap& index = autoIndex();
    for (auto it = index.cbegin(); it != index.cend(); ++it) {
        const QVariantMap entry = it.value().toMap();
        const QString name = entry.value("file").toString();
        // a name this wrote: <hash>.jpg, nothing with a folder in it
        if (name.isEmpty() || name.contains('/') || name.contains('\\') || !name.endsWith(".jpg")) {
            continue;
        }
        // the time it was taken in the address, so a new picture of the same avatar isn't the old one cached
        pictures.insert(it.key(), QUrl::fromLocalFile(folder.filePath(name)).toString() + "?" +
                                      QString::number(entry.value("taken").toLongLong()));
    }
    return pictures;
}

void AvatarThumbnails::autoPictureFor(const QString& url) {
    // DARKRIDE_AUTO_AVATAR_PICTURES=0: none (a timing run's clients, whose secondary camera mustn't switch on)
    static const bool off = qEnvironmentVariable("DARKRIDE_AUTO_AVATAR_PICTURES") == "0";
    if (off || url.isEmpty() || url == _autoUrl || autoIndex().contains(url)) {
        return;   // being waited for already, or it has its picture
    }
    _autoUrl = url;
    _autoTries = 0;
    _autoTimer->start(AUTO_FIRST_MS);
}

void AvatarThumbnails::tryAutoPicture() {
    auto myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    if (_autoUrl.isEmpty() || !myAvatar || myAvatar->getFullAvatarURLFromPreferences().toString() != _autoUrl ||
        autoIndex().contains(_autoUrl)) {
        _autoUrl.clear();   // another avatar's load starts its own wait
        return;
    }
    auto renderConfig = qApp->getRenderEngine()->getConfiguration();
    auto camera = renderConfig ? dynamic_cast<SecondaryCameraJobConfig*>(renderConfig->getConfig("SecondaryCamera")) : nullptr;
    MainWindow* window = qApp->getWindow();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!qApp->isPhysicsEnabled()) {
        _physicsOnAt = 0;
    } else if (_physicsOnAt == 0) {
        _physicsOnAt = now;
    }
    const RefreshRateManager& refresh = qApp->getRefreshRateManager();
    // settled: the model loaded, not seated or riding (a seat parents you), standing still (not walking, not carried by
    // something moving), the spectator camera not in use (its view isn't borrowed unasked), the window shown (a
    // minimized one draws no frame for the picture), and no picture being taken. And frame time to spare
    // (performance, 11-08): physics on for 20 s (past an arrival's loading tail), the refresh governor at full rate
    // and past its arrival grace (a doubled render could drop it to half rate for the rest of the stay), and not in a
    // headset (a third render of the scene at the headset's rate)
    const bool settled = avatarLoaded() && myAvatar->getParentID().isNull() &&
                         glm::length(myAvatar->getWorldVelocity()) < AUTO_STILL_SPEED && camera && !camera->isEnabled() &&
                         window && window->isVisible() && !window->isMinimized() && !_taking &&
                         _physicsOnAt > 0 && now - _physicsOnAt >= AUTO_AFTER_PHYSICS_MS && !refresh.isHalfRate() &&
                         !refresh.inArrivalGrace() && !qApp->isHMDMode();
    if (!settled) {
        if (++_autoTries < AUTO_TRIES) {
            _autoTimer->start(AUTO_POLL_MS);
        } else {
            qCDebug(interfaceapp) << "Avatar picture: not taken, never settled for" << _autoUrl;
            _autoUrl.clear();
        }
        return;
    }
    const QString url = _autoUrl;
    const QString folder = autoFolder();
    shoot([this, url, folder](const QByteArray& jpeg, const QString& error) {
        if (_autoUrl == url) {
            _autoUrl.clear();
        }
        if (!error.isEmpty()) {
            qCDebug(interfaceapp) << "Avatar picture: not taken for" << url << error;
            return;
        }
        if (folder != autoFolder()) {
            return;   // the account changed while it was taken: it isn't the new one's
        }
        keepAutoPicture(url, jpeg);
    }, AUTO_FRAMES);
}

void AvatarThumbnails::keepAutoPicture(const QString& url, const QByteArray& jpeg) {
    QDir folder(autoFolder());
    if (jpeg.isEmpty() || !folder.mkpath(".")) {
        return;
    }
    // named by the link's hash: no part of a link in a file name
    const QString name = QString::fromLatin1(QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex()) + ".jpg";
    QSaveFile picture(folder.filePath(name));
    if (!picture.open(QIODevice::WriteOnly) || picture.write(jpeg) != jpeg.size() || !picture.commit()) {
        qCDebug(interfaceapp) << "Avatar picture: couldn't keep it for" << url;
        return;
    }
    autoIndex();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    _autoIndex.insert(url, QVariantMap { { "file", name }, { "taken", now } });
    // the oldest go past AUTO_KEPT
    while (_autoIndex.size() > AUTO_KEPT) {
        // the first as a start, so one always goes (whatever times a changed clock left in the index)
        QString oldest = _autoIndex.firstKey();
        qint64 oldestTaken = _autoIndex.first().toMap().value("taken").toLongLong();
        for (auto it = _autoIndex.cbegin(); it != _autoIndex.cend(); ++it) {
            const qint64 taken = it.value().toMap().value("taken").toLongLong();
            if (taken < oldestTaken) {
                oldestTaken = taken;
                oldest = it.key();
            }
        }
        const QString oldName = _autoIndex.value(oldest).toMap().value("file").toString();
        if (!oldName.isEmpty() && !oldName.contains('/') && !oldName.contains('\\')) {
            folder.remove(oldName);
        }
        _autoIndex.remove(oldest);
    }
    QSaveFile index(folder.filePath(AUTO_INDEX));
    if (index.open(QIODevice::WriteOnly)) {
        index.write(QJsonDocument(QJsonObject::fromVariantMap(_autoIndex)).toJson(QJsonDocument::Compact));
        index.commit();
    }
    qCDebug(interfaceapp) << "Avatar picture: kept for" << url;
    emit autoPicturesChanged();
}
