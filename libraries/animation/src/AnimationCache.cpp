//
//  AnimationCache.cpp
//  libraries/animation/src/
//
//  Created by Andrzej Kapolka on 4/14/14.
//  Copyright (c) 2014 High Fidelity, Inc. All rights reserved.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AnimationCache.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QRunnable>
#include <QThreadPool>

#include <shared/QtHelpers.h>
#include <Trace.h>
#include <StatTracker.h>
#include <Profile.h>

#include "AnimationLogging.h"
#include <hfm/ModelFormatRegistry.h>
#include <GLMHelpers.h>
#include <HumanoidJointMap.h>

int animationPointerMetaTypeId = qRegisterMetaType<AnimationPointer>();

AnimationCache::AnimationCache(QObject* parent) :
    ResourceCache(parent)
{
    const qint64 ANIMATION_DEFAULT_UNUSED_MAX_SIZE = 50 * BYTES_PER_MEGABYTES;
    setUnusedResourceCacheSize(ANIMATION_DEFAULT_UNUSED_MAX_SIZE);
    setObjectName("AnimationCache");
}

AnimationPointer AnimationCache::getAnimation(const QUrl& url) {
    return getResource(url).staticCast<Animation>();
}

QSharedPointer<Resource> AnimationCache::createResource(const QUrl& url) {
    return QSharedPointer<Resource>(new Animation(url), &Resource::deleter);
}

QSharedPointer<Resource> AnimationCache::createResourceCopy(const QSharedPointer<Resource>& resource) {
    return QSharedPointer<Resource>(new Animation(*resource.staticCast<Animation>()), &Resource::deleter);
}

AnimationReader::AnimationReader(const QUrl& url, const QByteArray& data) :
    _url(url),
    _data(data) {
    DependencyManager::get<StatTracker>()->incrementStat("PendingProcessing");
}

// An animation of a humanoid whose joints don't have Darkride's names takes them, as a model of the same skeleton does
// when it's loaded (PrepareJointsTask), so the two still match
static void nameHumanoidJoints(HFMModel& hfmModel) {
    if (hasMainHumanoidJoints(hfmModel.getJointNames())) {
        return;
    }
    std::vector<HumanoidJointInfo> joints;
    for (const HFMJoint& joint : hfmModel.joints) {
        joints.push_back({ joint.name, joint.parentIndex, joint.isSkeletonJoint, extractTranslation(joint.globalTransform) });
    }
    QHash<int, QString> names = mapHumanoidJoints(joints);
    for (auto itr = names.begin(); itr != names.end(); itr++) {
        hfmModel.joints[itr.key()].name = itr.value();
    }
}

void AnimationReader::run() {
    DependencyManager::get<StatTracker>()->decrementStat("PendingProcessing");
    CounterStat counter("Processing");

    PROFILE_RANGE_EX(resource_parse, __FUNCTION__, 0xFF00FF00, 0, { { "url", _url.toString() } });
    auto originalPriority = QThread::currentThread()->priority();
    if (originalPriority == QThread::InheritPriority) {
        originalPriority = QThread::NormalPriority;
    }
    QThread::currentThread()->setPriority(QThread::LowPriority);
    try {
        if (_data.isEmpty()) {
            throw QString("Reply is NULL ?!");
        }
        QString urlname = _url.path().toLower();
        bool urlValid = true;
        urlValid &= !urlname.isEmpty();
        urlValid &= !_url.path().isEmpty();

        if (urlValid && urlname.endsWith(".muscles.json")) {
            // A humanoid muscle clip (hfm::MuscleClip), written by the avatar importer from a Unity clip.
            const QJsonObject json = QJsonDocument::fromJson(_data).object();
            HFMModel::Pointer hfmModel = std::make_shared<HFMModel>();
            hfm::MuscleClip& clip = hfmModel->muscleClip;
            clip.frameRate = (float)json.value("frameRate").toDouble(30.0);
            clip.loop = json.value("loop").toBool(false);
            for (const QJsonValue& muscle : json.value("muscles").toArray()) {
                clip.muscles.push_back(muscle.toInt(-1));
            }
            for (const QJsonValue& frame : json.value("frames").toArray()) {
                std::vector<float> values;
                for (const QJsonValue& value : frame.toArray()) {
                    values.push_back((float)value.toDouble());
                }
                clip.frames.push_back(values);
            }
            hfmModel->animationFrames.resize((int)clip.frames.size());
            if (clip.isValid()) {
                emit onSuccess(hfmModel);
            } else {
                emit onError(299, "an empty muscle clip " + _url.toString());
            }
        } else if (urlValid) {
            // Parse directly from the QNetworkReply
            HFMModel::Pointer hfmModel;

            auto serializer = DependencyManager::get<ModelFormatRegistry>()->getSerializerForMediaType(_data, _url, "");
            if (serializer) {
                hfmModel = serializer->read(_data, QVariantHash(), _url.path());
                if (hfmModel) {
                    nameHumanoidJoints(*hfmModel);
                }
            } else {
                QString errorStr("unsupported format " + _url.toString());
                emit onError(299, errorStr);
            }

            emit onSuccess(hfmModel);
        } else {
            throw QString("url is invalid");
        }

    } catch (const QString& error) {
        emit onError(299, error);
    }
    QThread::currentThread()->setPriority(originalPriority);
}

bool Animation::isLoaded() const {
    return _loaded && _hfmModel;
}

QStringList Animation::getJointNames() const {
    if (QThread::currentThread() != thread()) {
        QStringList result;
        BLOCKING_INVOKE_METHOD(const_cast<Animation*>(this), "getJointNames",
            Q_RETURN_ARG(QStringList, result));
        return result;
    }
    QStringList names;
    if (_hfmModel) {
        foreach (const HFMJoint& joint, _hfmModel->joints) {
            names.append(joint.name);
        }
    }
    return names;
}

QVector<HFMAnimationFrame> Animation::getFrames() const {
    if (QThread::currentThread() != thread()) {
        QVector<HFMAnimationFrame> result;
        BLOCKING_INVOKE_METHOD(const_cast<Animation*>(this), "getFrames",
            Q_RETURN_ARG(QVector<HFMAnimationFrame>, result));
        return result;
    }
    if (_hfmModel) {
        return _hfmModel->animationFrames;
    } else {
        return QVector<HFMAnimationFrame>();
    }
}

const QVector<HFMAnimationFrame>& Animation::getFramesReference() const {
    return _hfmModel->animationFrames;
}

void Animation::downloadFinished(const QByteArray& data) {
    // parse the animation/fbx file on a background thread.
    AnimationReader* animationReader = new AnimationReader(_url, data);
    connect(animationReader, SIGNAL(onSuccess(HFMModel::Pointer)), SLOT(animationParseSuccess(HFMModel::Pointer)));
    connect(animationReader, SIGNAL(onError(int, QString)), SLOT(animationParseError(int, QString)));
    QThreadPool::globalInstance()->start(animationReader);
}

void Animation::animationParseSuccess(HFMModel::Pointer hfmModel) {
    _hfmModel = hfmModel;
    finishedLoading(true);
}

void Animation::animationParseError(int error, QString str) {
    qCCritical(animation) << "Animation parse error, code =" << error << str;
    emit failed(QNetworkReply::UnknownContentError);
    finishedLoading(false);
}

