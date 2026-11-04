//
//  AnimationCache.h
//  libraries/animation/src/
//
//  Created by Andrzej Kapolka on 4/14/14.
//  Copyright (c) 2014 High Fidelity, Inc. All rights reserved.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_AnimationCache_h
#define hifi_AnimationCache_h

#include <QtCore/QRunnable>

#include <DependencyManager.h>
#include <hfm/HFM.h>
#include <ResourceCache.h>

class Animation;

using AnimationPointer = QSharedPointer<Animation>;

class AnimationCache : public ResourceCache, public Dependency  {
    Q_OBJECT
    SINGLETON_DEPENDENCY

public:

    Q_INVOKABLE AnimationPointer getAnimation(const QString& url) { return getAnimation(QUrl(url)); }
    Q_INVOKABLE AnimationPointer getAnimation(const QUrl& url);

protected:
    virtual QSharedPointer<Resource> createResource(const QUrl& url) override;
    QSharedPointer<Resource> createResourceCopy(const QSharedPointer<Resource>& resource) override;

private:
    explicit AnimationCache(QObject* parent = NULL);
    virtual ~AnimationCache() { }

};

Q_DECLARE_METATYPE(AnimationPointer)

/// An animation loaded from the network.
class Animation : public Resource {
    Q_OBJECT

public:

    Animation(const Animation& other) : Resource(other), _hfmModel(other._hfmModel) {}
    Animation(const QUrl& url) : Resource(url) {}

    QString getType() const override { return "Animation"; }

    const HFMModel& getHFMModel() const { return *_hfmModel; }

    virtual bool isLoaded() const override;

    Q_INVOKABLE QStringList getJointNames() const;
    
    Q_INVOKABLE QVector<HFMAnimationFrame> getFrames() const;

    const QVector<HFMAnimationFrame>& getFramesReference() const;
    
protected:
    virtual void downloadFinished(const QByteArray& data) override;

protected slots:
    void animationParseSuccess(HFMModel::Pointer hfmModel);
    void animationParseError(int error, QString str);

private:
    
    HFMModel::Pointer _hfmModel;
};

/// Reads geometry in a worker thread.
class AnimationReader : public QObject, public QRunnable {
    Q_OBJECT

public:
    AnimationReader(const QUrl& url, const QByteArray& data);
    virtual void run() override;

signals:
    void onSuccess(HFMModel::Pointer hfmModel);
    void onError(int error, QString str);

private:
    QUrl _url;
    QByteArray _data;
};


// What a script sees of an animation (AnimationCache.getAnimation): the record the script engine makes of it
// (WasmTypeConversions), as documented here.
/**jsdoc
 * Information about an animation resource, created by {@link AnimationCache.getAnimation}.
 *
 * @class AnimationObject
 * @hideconstructor
 *
 * @hifi-interface
 * @hifi-client-entity
 * @hifi-avatar
 * @hifi-server-entity
 * @hifi-assignment-client
 *
 * @property {string[]} jointNames - The names of the joints that are animated. <em>Read-only.</em>
 * @property {AnimationFrameObject[]} frames - The frames in the animation. <em>Read-only.</em>
 */

/**jsdoc
 * Gets the names of the joints that are animated.
 * @function AnimationObject.getJointNames
 * @returns {string[]} The names of the joints that are animated.
 */

/**jsdoc
 * Gets the frames in the animation.
 * @function AnimationObject.getFrames
 * @returns {AnimationFrameObject[]} The frames in the animation.
 */

/**jsdoc
 * Joint rotations in one frame of an {@link AnimationObject}.
 *
 * @class AnimationFrameObject
 * @hideconstructor
 *
 * @hifi-interface
 * @hifi-client-entity
 * @hifi-avatar
 * @hifi-server-entity
 * @hifi-assignment-client
 *
 * @property {Quat[]} rotations - Joint rotations. <em>Read-only.</em>
 */

#endif // hifi_AnimationCache_h
