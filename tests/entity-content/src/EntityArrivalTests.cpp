//
//  EntityArrivalTests.cpp
//  tests/entity-content/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "EntityArrivalTests.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <AddressManager.h>
#include <AvatarData.h>
#include <EntityContentAddresses.h>
#include <EntityItem.h>
#include <EntityItemProperties.h>
#include <EntityTree.h>
#include <EntityTreeElement.h>
#include <EntityTypes.h>
#include <ModelEntityItem.h>
#include <NodeList.h>
#include <Octree.h>
#include <OctreePacketData.h>
#include <SharedUtil.h>
#include <SpatialParentFinder.h>
#include <udt/SequenceNumber.h>
#include <TextEntityItem.h>
#include <WebEntityItem.h>
#include <ZoneEntityItem.h>

QTEST_GUILESS_MAIN(EntityArrivalTests)

static const QUuid ENTITY_ID = QUuid("{11111111-2222-3333-4444-555555555555}");

// a tree finds parents through the app's finder; the tests give it one that looks in the tree (as the rights tests do)
class TreeParentFinder : public SpatialParentFinder {
public:
    SpatiallyNestableWeakPointer find(QUuid parentID, bool& success, SpatialParentTree* entityTree) const override {
        SpatiallyNestablePointer found = entityTree ? entityTree->findByID(parentID) : SpatiallyNestablePointer();
        success = found != nullptr;
        return found;
    }
};

// an entity's data as the entity server sends it: the type's own item, its properties set, appended to a packet
static OctreePacketData* appendEntity(EntityTypes::EntityType type, const EntityItemProperties& properties) {
    EntityItemProperties withType = properties;
    withType.setType(type);
    EntityItemPointer entity = EntityTypes::constructEntityItem(type, EntityItemID(ENTITY_ID), withType);
    entity->setProperties(withType);
    auto packetData = new OctreePacketData();
    EncodeBitstreamParams params;
    auto extra = std::make_shared<EntityTreeElementExtraEncodeData>();
    entity->appendEntityData(packetData, params, extra);
    return packetData;
}

// the same data read into a fresh item of the type, with or without the refusal
static EntityItemPointer readEntity(EntityTypes::EntityType type, OctreePacketData* packetData, bool refuses,
                                    QStringList& refused) {
    EntityItemPointer entity = EntityTypes::constructEntityItem(type, EntityItemID(ENTITY_ID), EntityItemProperties());
    ReadBitstreamToTreeParams args;
    args.refusesContentFromThisComputer = refuses;
    entity->readEntityDataFromBuffer(packetData->getUncompressedData(), packetData->getUncompressedSize(), args);
    refused = args.refusedAddresses;
    return entity;
}

void EntityArrivalTests::initTestCase() {
    // a tree adds an entity only with a node list about (as the rights tests set up)
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent, INVALID_PORT);
    DependencyManager::set<SpatialParentFinder, TreeParentFinder>();
}

void EntityArrivalTests::testWireReadRefuses() {
    QStringList refused;

    // a Model: its model, hull, textures map, animation, collision sound, indicator, href and userData
    EntityItemProperties model;
    model.setModelURL("file:///Users/me/car.glb");
    model.setCompoundShapeURL("/Users/me/hull.obj");
    model.setTextures("{\"tex.body\":\"file:///Users/me/body.png\",\"tex.wheel\":\"https://cdn/w.png\"}");
    model.getAnimation().setURL("C:\\anims\\run.fbx");
    model.setCollisionSoundURL("\\\\host\\share\\hit.wav");
    model.getGrab().setEquippableIndicatorURL("/Users/me/hand.fbx");
    model.setHref("file:///Users/me/world.json");
    model.setUserData("{\"impostor\":{\"albedo\":\"file:///Users/me/a.png\",\"normal\":\"file:///Users/me/n.png\","
                      "\"radius\":1.5},\"keep\":true}");
    OctreePacketData* packet = appendEntity(EntityTypes::Model, model);
    auto read = std::dynamic_pointer_cast<ModelEntityItem>(readEntity(EntityTypes::Model, packet, true, refused));
    QVERIFY(read);
    QCOMPARE(read->getModelURL(), QString());
    QCOMPARE(read->getCompoundShapeURL(), QString());
    QJsonObject textures = QJsonDocument::fromJson(read->getTextures().toUtf8()).object();
    QCOMPARE(textures.value("tex.body").toString(), QString());
    QCOMPARE(textures.value("tex.wheel").toString(), QString("https://cdn/w.png"));
    QCOMPARE(read->getAnimationURL(), QString());
    QCOMPARE(read->getCollisionSoundURL(), QString());
    QCOMPARE(read->getHref(), QString());
    QJsonObject userData = QJsonDocument::fromJson(read->getUserData().toUtf8()).object();
    QVERIFY(!userData.contains("impostor"));
    QVERIFY(userData.value("keep").toBool());
    for (const QString& name : { "modelURL", "compoundShapeURL", "textures", "animation.url", "collisionSoundURL",
                                 "grab.equippableIndicatorURL", "href", "userData" }) {
        QVERIFY2(refused.contains(name), qPrintable("refused should name " + QString(name) + ": " + refused.join(",")));
    }
    delete packet;

    // a Web entity: its page and injected script, by the path rule
    EntityItemProperties web;
    web.setSourceUrl("file:///Users/me/page.html");
    web.setScriptURL("/Users/me/inject.js");
    packet = appendEntity(EntityTypes::Web, web);
    auto readWeb = std::dynamic_pointer_cast<WebEntityItem>(readEntity(EntityTypes::Web, packet, true, refused));
    QVERIFY(readWeb);
    QCOMPARE(readWeb->getSourceUrl(), QString());
    QCOMPARE(readWeb->getScriptURL(), QString());
    QCOMPARE(refused, QStringList({ "sourceUrl", "scriptURL" }));
    delete packet;

    // a Zone: skybox, ambient and the server filter
    EntityItemProperties zone;
    zone.getSkybox().setURL("file:///Users/me/sky.png");
    zone.getAmbientLight().setAmbientURL("/~/../ambient.ktx");
    zone.setFilterURL("file:///Users/me/filter.js");
    packet = appendEntity(EntityTypes::Zone, zone);
    auto readZone = std::dynamic_pointer_cast<ZoneEntityItem>(readEntity(EntityTypes::Zone, packet, true, refused));
    QVERIFY(readZone);
    QCOMPARE(readZone->getSkyboxProperties().getURL(), QString());
    QCOMPARE(readZone->getAmbientLightProperties().getAmbientURL(), QString());
    QCOMPARE(readZone->getFilterURL(), QString());
    for (const QString& name : { "skybox.url", "ambientLight.ambientURL", "filterURL" }) {
        QVERIFY2(refused.contains(name), qPrintable(refused.join(",")));
    }
    delete packet;

    // a Text entity: a font address by the path rule, a family name kept
    EntityItemProperties text;
    text.setFont("C:/fonts/x.sdff");
    packet = appendEntity(EntityTypes::Text, text);
    auto readText = std::dynamic_pointer_cast<TextEntityItem>(readEntity(EntityTypes::Text, packet, true, refused));
    QVERIFY(readText);
    QCOMPARE(readText->getFont(), QString());
    QCOMPARE(refused, QStringList({ "font" }));
    delete packet;
    text.setFont("Courier New");
    packet = appendEntity(EntityTypes::Text, text);
    readText = std::dynamic_pointer_cast<TextEntityItem>(readEntity(EntityTypes::Text, packet, true, refused));
    QCOMPARE(readText->getFont(), QString("Courier New"));
    QVERIFY(refused.isEmpty());
    delete packet;
}

void EntityArrivalTests::testWireReadKeepsWhenAllowed() {
    // the same packet with the refusal off (a local world) keeps every address; fetched addresses pass either way
    QStringList refused;
    EntityItemProperties model;
    model.setModelURL("file:///Users/me/car.glb");
    model.setTextures("{\"tex.body\":\"file:///Users/me/body.png\"}");
    model.setHref("file:///Users/me/world.json");
    OctreePacketData* packet = appendEntity(EntityTypes::Model, model);
    auto read = std::dynamic_pointer_cast<ModelEntityItem>(readEntity(EntityTypes::Model, packet, false, refused));
    QCOMPARE(read->getModelURL(), QString("file:///Users/me/car.glb"));
    QCOMPARE(read->getTextures(), QString("{\"tex.body\":\"file:///Users/me/body.png\"}"));
    QCOMPARE(read->getHref(), QString("file:///Users/me/world.json"));
    QVERIFY(refused.isEmpty());
    delete packet;

    model.setModelURL("https://cdn.darkride.app/car.glb");
    model.setTextures("{\"tex.body\":\"atp:/body.png\"}");
    model.setHref("/1,2,3/0,0,0,1");
    packet = appendEntity(EntityTypes::Model, model);
    read = std::dynamic_pointer_cast<ModelEntityItem>(readEntity(EntityTypes::Model, packet, true, refused));
    QCOMPARE(read->getModelURL(), QString("https://cdn.darkride.app/car.glb"));
    QCOMPARE(read->getTextures(), QString("{\"tex.body\":\"atp:/body.png\"}"));
    QCOMPARE(read->getHref(), QString("/1,2,3/0,0,0,1"));
    QVERIFY(refused.isEmpty());
    delete packet;
}

void EntityArrivalTests::testPartialUpdateUntouched() {
    // an update carrying only the name leaves an address the entity already holds alone, and refuses nothing
    EntityItemPointer entity = EntityTypes::constructEntityItem(EntityTypes::Model, EntityItemID(ENTITY_ID),
                                                                EntityItemProperties());
    auto model = std::dynamic_pointer_cast<ModelEntityItem>(entity);
    model->setModelURL("https://cdn.darkride.app/car.glb");

    EntityItemProperties update;
    update.setType(EntityTypes::Model);
    update.setName("renamed");
    EntityItemPointer sender = EntityTypes::constructEntityItem(EntityTypes::Model, EntityItemID(ENTITY_ID), update);
    sender->setName("renamed");
    OctreePacketData packetData;
    EncodeBitstreamParams params;
    // the encoder sends the properties the element's extra data names for the entity: only the name here
    auto extra = std::make_shared<EntityTreeElementExtraEncodeData>();
    EntityPropertyFlags onlyName;
    onlyName += PROP_NAME;
    extra->entities[EntityItemID(ENTITY_ID)] = onlyName;
    sender->appendEntityData(&packetData, params, extra);

    ReadBitstreamToTreeParams args;
    args.refusesContentFromThisComputer = true;
    model->readEntityDataFromBuffer(packetData.getUncompressedData(), packetData.getUncompressedSize(), args);
    QCOMPARE(model->getName(), QString("renamed"));
    QCOMPARE(model->getModelURL(), QString("https://cdn.darkride.app/car.glb"));
    QVERIFY(args.refusedAddresses.isEmpty());
}

void EntityArrivalTests::testFreshClientTree() {
    // a tree told to refuse before any world is known empties the first packet's addresses: the flag reaches the
    // reader through the read parameters the tree fills, and the tree counts and warns what was refused (a new
    // entity from the wire waits in the tree's pending list until readBitstreamToTree adds it, so the count is the
    // proof here; the values are proven above)
    EntityTreePointer tree(new EntityTree());
    tree->createRootElement();
    tree->setRefusesContentFromThisComputer(true);
    QVERIFY(tree->refusesContentFromThisComputer());
    QCOMPARE(tree->takeRefusedAddressesCount(), 0);

    EntityItemProperties model;
    model.setModelURL("file:///Users/me/car.glb");
    model.setTextures("{\"tex.body\":\"file:///Users/me/body.png\"}");
    OctreePacketData* entityPacket = appendEntity(EntityTypes::Model, model);
    // the element's form: a count, then each entity's data
    OctreePacketData packetData;
    packetData.appendValue((uint16_t)1);
    packetData.appendRawData(entityPacket->getUncompressedData(), entityPacket->getUncompressedSize());
    delete entityPacket;

    ReadBitstreamToTreeParams args;
    tree->readEntityDataFromBuffer(packetData.getUncompressedData(), packetData.getUncompressedSize(), args);
    QCOMPARE(tree->takeRefusedAddressesCount(), 2);   // modelURL and textures
    QCOMPARE(tree->takeRefusedAddressesCount(), 0);   // taken once

    // the same packet again: still refused (counted), warned only the first time (the set keeps the names)
    tree->readEntityDataFromBuffer(packetData.getUncompressedData(), packetData.getUncompressedSize(), args);
    QCOMPARE(tree->takeRefusedAddressesCount(), 2);

    // the flag off (a local world, or Test Locally): nothing refused
    tree->setRefusesContentFromThisComputer(false);
    tree->readEntityDataFromBuffer(packetData.getUncompressedData(), packetData.getUncompressedSize(), args);
    QCOMPARE(tree->takeRefusedAddressesCount(), 0);
    tree->eraseAllOctreeElements(false);
}

void EntityArrivalTests::testWorldFile() {
    // a world file read through readFromMap: refused when the tree says so (a world from a link), kept otherwise
    const QString worldJson = "{\"Version\":142,\"Entities\":[{\"id\":\"" + ENTITY_ID.toString() + "\","
        "\"type\":\"Model\",\"modelURL\":\"file:///Users/me/car.glb\",\"name\":\"car\"}]}";
    for (bool refuses : { true, false }) {
        EntityTreePointer tree(new EntityTree());
        tree->createRootElement();
        tree->setIsServerlessMode(true);
        tree->setRefusesContentFromThisComputer(refuses);
        QVariantMap map = QJsonDocument::fromJson(worldJson.toUtf8()).toVariant().toMap();
        QVERIFY(tree->readFromMap(map));
        EntityItemPointer added = tree->findEntityByEntityItemID(EntityItemID(ENTITY_ID));
        QVERIFY(added);
        QCOMPARE(std::dynamic_pointer_cast<ModelEntityItem>(added)->getModelURL(),
                 refuses ? QString() : QString("file:///Users/me/car.glb"));
        QCOMPARE(added->getName(), QString("car"));
        tree->eraseAllOctreeElements(false);
    }
}

void EntityArrivalTests::testAvatarBlob() {
    // another visitor's wearable, as OtherAvatar reads it from the mixer's blob: the three address forms are emptied
    const QByteArray blob = "{\"type\":\"Model\",\"modelURL\":\"file:///Users/me/hat.glb\","
                            "\"compoundShapeURL\":\"/Users/me/hat-hull.obj\","
                            "\"textures\":\"{\\\"tex.hat\\\":\\\"C:\\\\\\\\pictures\\\\\\\\hat.png\\\"}\","
                            "\"name\":\"hat\"}";
    EntityItemProperties properties;
    QVERIFY(EntityItemProperties::blobToProperties(blob, properties));
    properties.markAllChanged();
    const QStringList cleared = EntityContentAddresses::clearThisComputer(properties);
    QCOMPARE(cleared, QStringList({ "modelURL", "compoundShapeURL", "textures" }));
    QCOMPARE(properties.getModelURL(), QString());
    QCOMPARE(properties.getCompoundShapeURL(), QString());
    const QJsonObject textures = QJsonDocument::fromJson(properties.getTextures().toUtf8()).object();
    QCOMPARE(textures.value("tex.hat").toString(), QString());
    QCOMPARE(properties.getName(), QString("hat"));
}

void EntityArrivalTests::testReceivedAvatarIdentity() {
    // another avatar's identity, as the mixer relays it: a model address or an attachment's that names a file on this
    // computer is taken as blank (the default avatar, an attachment without a model); fetched addresses stay
    AttachmentData hat;
    hat.modelURL = QUrl("file:///Users/me/hat.glb");
    hat.jointName = "Head";
    AttachmentData badge;
    badge.modelURL = QUrl("https://cdn.darkride.app/badge.glb");
    badge.jointName = "Chest";
    AttachmentData wand;
    wand.modelURL = QUrl("C:/Users/me/wand.glb");
    wand.jointName = "RightHand";

    QByteArray identityData;
    {
        QDataStream stream(&identityData, QIODevice::WriteOnly);
        stream << QUuid::createUuid()
               << (udt::SequenceNumber::Type)1
               << QUrl("/Users/me/body.fst")
               << QVector<AttachmentData>({ hat, badge, wand })
               << QString("Someone")
               << QString("Someone")
               << AvatarDataPacket::IdentityFlags(AvatarDataPacket::IdentityFlag::none);
    }
    AvatarData other;
    QVERIFY(!other.isMyAvatar());
    QDataStream stream(identityData);
    bool identityChanged = false, displayNameChanged = false, skeletonModelURLChanged = false;
    other.processAvatarIdentity(stream, identityChanged, displayNameChanged, skeletonModelURLChanged);
    QCOMPARE(other.getSkeletonModelURL(), AvatarData::defaultFullAvatarModelUrl());
    const QVector<AttachmentData> attachments = other.getAttachmentData();
    QCOMPARE(attachments.size(), 3);
    QCOMPARE(attachments[0].modelURL, QUrl());
    QCOMPARE(attachments[1].modelURL, QUrl("https://cdn.darkride.app/badge.glb"));
    QCOMPARE(attachments[2].modelURL, QUrl());
    QCOMPARE(attachments[0].jointName, QString("Head"));
    QCOMPARE(other.getDisplayName(), QString("Someone"));
}
