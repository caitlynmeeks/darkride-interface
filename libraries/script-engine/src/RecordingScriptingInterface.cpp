//
//  Created by Bradley Austin Davis on 2015/11/13
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RecordingScriptingInterface.h"

#include <QStandardPaths>
#include <QtCore/QThread>
#include <QtCore/QUrl>
#include <QtWidgets/QFileDialog>

#include <shared/QtHelpers.h>
#include <AssetClient.h>
#include <AssetUpload.h>
#include <BuildInfo.h>
#include <NumericalConstants.h>
#include <PathUtils.h>
#include <Transform.h>
#include <recording/Deck.h>
#include <recording/Recorder.h>
#include <recording/Clip.h>
#include <recording/Frame.h>
#include <recording/ClipCache.h>

#include "ScriptEngineLogging.h"

using namespace recording;

static const QString HFR_EXTENSION = "hfr";

RecordingScriptingInterface::RecordingScriptingInterface() {
    _player = DependencyManager::get<Deck>();
    _recorder = DependencyManager::get<Recorder>();
}

bool RecordingScriptingInterface::isPlaying() const {
    return _player->isPlaying();
}

bool RecordingScriptingInterface::isPaused() const {
    return _player->isPaused();
}

float RecordingScriptingInterface::playerElapsed() const {
    return _player->position();
}

float RecordingScriptingInterface::playerLength() const {
    return _player->length();
}

void RecordingScriptingInterface::loadRecording(const QString& url, const ScriptCallback& callback) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "loadRecording", Q_ARG(const QString&, url), Q_ARG(const ScriptCallback&, callback));
        return;
    }

    auto clipLoader = DependencyManager::get<recording::ClipCache>()->getClipLoader(url);

    if (clipLoader->isLoaded()) {
        qCDebug(scriptengine) << "Recording already loaded from" << url;
        _player->queueClip(clipLoader->getClip());
        callback.post({ true, url });
        return;
    }

    // hold a strong pointer to the loading clip so that it has a chance to load
    _clipLoaders.insert(clipLoader);

    auto weakClipLoader = clipLoader.toWeakRef();

    // the callback is posted to the module's thread: (true, url) when the clip has loaded, (false, url) when it failed
    connect(clipLoader.data(), &recording::NetworkClipLoader::clipLoaded, this, [this, weakClipLoader, url, callback]() {
        if (auto clipLoader = weakClipLoader.toStrongRef()) {
            qCDebug(scriptengine) << "Loaded recording from" << url;
            _player->queueClip(clipLoader->getClip());
            callback.post({ true, url });
            _clipLoaders.remove(clipLoader);
        }
    });

    connect(clipLoader.data(), &recording::NetworkClipLoader::failed, this, [this, weakClipLoader, url, callback](QNetworkReply::NetworkError error) {
        Q_UNUSED(error);
        qCDebug(scriptengine) << "Failed to load recording from\"" << url << '"';
        callback.post({ false, url });
        if (auto clipLoader = weakClipLoader.toStrongRef()) {
            _clipLoaders.remove(clipLoader);
        }
    });
}

void RecordingScriptingInterface::startPlaying() {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "startPlaying");
        return;
    }

    _player->play();
}

void RecordingScriptingInterface::setPlayerVolume(float volume) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "setPlayerVolume", Q_ARG(float, volume));
        return;
    }

    _player->setVolume(std::min(std::max(volume, 0.0f), 1.0f));
}

void RecordingScriptingInterface::setPlayerAudioOffset(float audioOffset) {
    // FIXME 
}

void RecordingScriptingInterface::setPlayerTime(float time) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "setPlayerTime", Q_ARG(float, time));
        return;
    }
    _player->seek(time);
}

void RecordingScriptingInterface::setPlayFromCurrentLocation(bool playFromCurrentLocation) {
    _playFromCurrentLocation = playFromCurrentLocation;
}

void RecordingScriptingInterface::setPlayerLoop(bool loop) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "setPlayerLoop", Q_ARG(bool, loop));
        return;
    }

    _player->loop(loop);
}

void RecordingScriptingInterface::setPlayerUseDisplayName(bool useDisplayName) {
    _useDisplayName = useDisplayName;
}

void RecordingScriptingInterface::setPlayerUseAttachments(bool useAttachments) {
    _useAttachments = useAttachments;
}

void RecordingScriptingInterface::setPlayerUseHeadModel(bool useHeadModel) {
    _useHeadModel = useHeadModel;
}

void RecordingScriptingInterface::setPlayerUseSkeletonModel(bool useSkeletonModel) {
    _useSkeletonModel = useSkeletonModel;
}

void RecordingScriptingInterface::pausePlayer() {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "pausePlayer");
        return;
    }
    _player->pause();
}

void RecordingScriptingInterface::stopPlaying() {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "stopPlaying");
        return;
    }
    _player->stop();
}

bool RecordingScriptingInterface::isRecording() const {
    return _recorder->isRecording();
}

float RecordingScriptingInterface::recorderElapsed() const {
    return _recorder->position();
}

void RecordingScriptingInterface::startRecording() {
    if (_recorder->isRecording()) {
        qCWarning(scriptengine) << "Recorder is already running";
        return;
    }

    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "startRecording");
        return;
    }

    _recorder->start();
}

void RecordingScriptingInterface::stopRecording() {
    if (!_recorder->isRecording()) {
        qCWarning(scriptengine) << "Recorder is not running";
        return;
    }

    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "stopRecording");
        return;
    }

    _recorder->stop();
    _lastClip = _recorder->getClip();
    _lastClip->seek(0);
}

QString RecordingScriptingInterface::getDefaultRecordingSaveDirectory() {
    QString directory = PathUtils::getAppLocalDataPath() + "Avatar Recordings/";
    if (!QDir(directory).exists()) {
        QDir().mkdir(directory);
    }
    return directory;
}

void RecordingScriptingInterface::saveRecording(const QString& filename) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "saveRecording",
            Q_ARG(QString, filename));
        return;
    }

    if (!_lastClip) {
        qWarning() << "There is no recording to save";
        return;
    }

    recording::Clip::toFile(filename, _lastClip);
}

bool RecordingScriptingInterface::saveRecordingToAsset(const ScriptCallback& getClipAtpUrl) {
    if (!getClipAtpUrl.isValid()) {
        qCWarning(scriptengine) << "The argument is not a function.";
        return false;
    }

    if (QThread::currentThread() != thread()) {
        bool result;
        BLOCKING_INVOKE_METHOD(this, "saveRecordingToAsset",
            Q_RETURN_ARG(bool, result),
            Q_ARG(const ScriptCallback&, getClipAtpUrl));
        return result;
    }

    if (!_lastClip) {
        qWarning() << "There is no recording to save";
        return false;
    }

    if (auto upload = DependencyManager::get<AssetClient>()->createUpload(recording::Clip::toBuffer(_lastClip))) {
        QObject::connect(upload, &AssetUpload::finished, this, [=](AssetUpload* upload, const QString& hash) {
            QString clip_atp_url = "";
            if (upload->getError() == AssetUpload::NoError) {
                clip_atp_url = QString("%1:%2").arg(URL_SCHEME_ATP, hash);
            } else {
                qCWarning(scriptengine) << "Error during the Asset upload.";
            }
            // the upload is done with either way (a failed one was never freed before)
            upload->deleteLater();
            getClipAtpUrl.post({ clip_atp_url });
        });
        upload->start();
        return true;
    }

    qCWarning(scriptengine) << "Saving on asset failed.";
    return false;
}

void RecordingScriptingInterface::loadLastRecording() {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "loadLastRecording");
        return;
    }

    if (!_lastClip) {
        qCDebug(scriptengine) << "There is no recording to load";
        return;
    }

    _player->queueClip(_lastClip);
    _player->play();
}

