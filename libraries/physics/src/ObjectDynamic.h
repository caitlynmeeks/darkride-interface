//
//  ObjectDynamic.h
//  libraries/physcis/src
//
//  Created by Seth Alves 2015-6-2
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// http://bulletphysics.org/Bullet/BulletFull/classbtActionInterface.html

#ifndef hifi_ObjectDynamic_h
#define hifi_ObjectDynamic_h

#include <vector>

#include <QUuid>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <EntityItemID.h>
#include <shared/ReadWriteLockable.h>
#include <SpatiallyNestable.h>

#include "ObjectMotionState.h"
#include "JoltUtil.h"
#include "EntityDynamicInterface.h"

// A "dynamic" is something added to an entity that changes how it moves: an action pushes on its body every
// substep, a constraint ties it to another body.
class ObjectDynamic : public EntityDynamicInterface, public ReadWriteLockable {
public:
    ObjectDynamic(EntityDynamicType type, const QUuid& id, EntityItemPointer ownerEntity);
    virtual ~ObjectDynamic();

    virtual void remapIDs(QHash<EntityItemID, EntityItemID>& map) override;

    virtual void removeFromSimulation(EntitySimulationPointer simulation) const override;
    virtual EntityItemWeakPointer getOwnerEntity() const override { return _ownerEntity; }
    virtual void setOwnerEntity(const EntityItemPointer ownerEntity) override;

    virtual void invalidate() {};

    virtual bool updateArguments(QVariantMap arguments) override;
    virtual QVariantMap getArguments() override;

    virtual QByteArray serialize() const override = 0;
    virtual void deserialize(QByteArray serializedArguments) override = 0;

    virtual bool lifetimeIsOver() override;
    virtual quint64 getExpires() override { return _expires; }

    // the bodies this dynamic touches, for ownership bookkeeping in the engine
    virtual std::vector<JPH::BodyID> getBodyIDs();

    // whether the other entity's body is in this world, or there's no other entity: a constraint's two bodies must
    // be in one world, as Jolt can't join bodies across physics systems
    bool otherIsInWorld(const PhysicsWorld* world);

protected:
    quint64 localTimeToServerTime(quint64 timeValue) const;
    quint64 serverTimeToLocalTime(quint64 timeValue) const;

    // the owner's motion state and body, when it is in the simulation
    ObjectMotionState* getMotionState() const;
    virtual JPH::Body* getBody();
    ObjectMotionState* getOtherMotionState(EntityItemID otherEntityID) const;
    JPH::Body* getOtherBody(EntityItemID otherEntityID);
    EntityItemPointer getEntityByID(EntityItemID entityID) const;
    virtual void activateBody(bool forceActivation = false);
    virtual void forceBodyNonStatic();

    EntityItemWeakPointer _ownerEntity;
    QString _tag;
    quint64 _expires { 0 }; // in seconds since epoch

    EntityItemID _otherID;
    SpatiallyNestableWeakPointer _other;
    SpatiallyNestablePointer getOther();
    int _otherJointIndex { -1 };

private:
    qint64 getEntityServerClockSkew() const;
};

typedef std::shared_ptr<ObjectDynamic> ObjectDynamicPointer;

#endif // hifi_ObjectDynamic_h
