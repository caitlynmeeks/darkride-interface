//
//  EditorCreate.cpp
//  interface/src/editor
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "EditorCreate.h"

#include <cmath>

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMimeDatabase>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QHttpMultiPart>
#include <QtNetwork/QHttpPart>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

#include <AccountManager.h>
#include <GLMHelpers.h>
#include <NetworkAccessManager.h>
#include <ShapeInfo.h>

#include "InterfaceLogging.h"
#include "ui/RightsLine.h"

namespace {
    const glm::u8vec3 SHAPE_COLOR { 190, 190, 190 };       // Unity's default material, more or less
    const glm::u8vec3 WHITE { 255, 255, 255 };
    const glm::u8vec3 BLACK { 0, 0, 0 };
    // a mid grey reads on white sand and on dark grass alike, where white vanished on sand
    const glm::u8vec3 GRID_COLOR { 110, 110, 110 };
    // a new Track: an oval loop, its ride line lifted so a car's floor clears the ground, its box reaching from the
    // pylons' feet (8 m under the ride line by default) to a little over it
    const int TRACK_LOOP_POINTS = 8;
    const float TRACK_LOOP_HALF_LENGTH = 15.0f;
    const float TRACK_LOOP_HALF_WIDTH = 7.5f;
    const float TRACK_LIFT = 3.0f;
    const float TRACK_LOOP_BOX_HEIGHT = 12.0f;
    const float TRACK_LOOP_REGISTRATION_Y = 9.0f / 12.0f;   // the ride line 9 m up the 12 m box: 9 m below, 3 m above
    const glm::u8vec3 TRACK_COLOR { 215, 215, 222 };
    const glm::vec3 TRACK_CAR_DIMENSIONS { 2.6f, 0.3f, 6.0f };   // a floor
    const glm::u8vec3 TRACK_CAR_COLOR { 60, 70, 90 };
    const glm::vec3 UNIT_DIMENSIONS { 1.0f };
    const glm::vec3 PANEL_DIMENSIONS { 1.0f, 1.0f, 0.01f };
    const float EMPTY_SIZE = 0.2f;           // an Empty's box: enough to frame and to take the gizmo
    // a quad or circle lies in X and Z, facing up: the engine keeps it flat in Y
    const glm::vec3 FLAT_DIMENSIONS { 1.0f, 0.0001f, 1.0f };
    const float FLAT_LIFT = 0.01f;           // just above the surface: lying on it, it would flicker with it
    const float LIGHT_RANGE = 3.0f;          // how far a new light reaches (half its dimensions); its glow,
                                             // with the falloff below, fills about that much
    const float POINT_LIGHT_LIFT = 1.5f;     // a lamp's height
    const float SPOT_LIGHT_LIFT = 2.5f;      // a downlight's
    const QString DEFAULT_IMAGE = "qrc:///images/empty-image-entity.png";
    const QString DEFAULT_PARTICLE = "qrc:///images/wispy-smoke.png";
    const QString DEFAULT_WEB_PAGE = "https://vr.darkride.ai";
    const QString FILES_API_PATH = "/api/user/files";

    glm::quat facing(float cameraYaw) {
        // a panel's visible face is +Z: with the camera's yaw it looks back at the camera
        return glm::angleAxis(cameraYaw, Vectors::UP);
    }

    EntityItemProperties shape(const QString& shapeName, const glm::vec3& position) {
        EntityItemProperties p;
        p.setType(EntityTypes::Shape);
        p.setName(shapeName);
        p.setShape(shapeName);
        p.setColor(SHAPE_COLOR);
        p.setDimensions(UNIT_DIMENSIONS);
        p.setPosition(position);
        return p;
    }

}

QString EditorUpload::uploadingStatus(const QString& fileName, int percent, const QString& more, bool fromComputer) {
    QString status = percent >= 100 ? QString("Processing %1…%2").arg(fileName, more)
                                    : QString("Uploading %1 %2%%3").arg(fileName).arg(percent).arg(more);
    return fromComputer ? status + " · " + rights::sentence() : status;
}

QString EditorUpload::safeFileName(const QString& name) {
    QString s = name;
    s.replace(QRegularExpression("[^A-Za-z0-9._-]+"), "_");
    // at most 64 characters, its ending kept (".preview.jpg", an extension): a long entity or file name made paths
    // past Windows' 260 (an impostor's cache file; qa's 1.1 review #24)
    const int MAX_LENGTH = 64;
    const int MAX_ENDING = 20;
    if (s.length() > MAX_LENGTH) {
        int dot = s.indexOf('.', s.length() - MAX_ENDING);
        QString ending = dot >= 0 ? s.mid(dot) : QString();
        s = s.left(MAX_LENGTH - ending.length()) + ending;
    }
    return s;
}

const QVector<EditorCreate::Item>& EditorCreate::items() {
    static const QVector<Item> ITEMS {
        { "empty", "Empty", "", EntityTypes::Shape },
        { "model", "Model…", "", EntityTypes::Model, true },
        { "cube", "Cube", "Shape", EntityTypes::Shape, true },
        { "sphere", "Sphere", "Shape", EntityTypes::Shape },
        { "cylinder", "Cylinder", "Shape", EntityTypes::Shape },
        { "cone", "Cone", "Shape", EntityTypes::Shape },
        { "quad", "Quad", "Shape", EntityTypes::Shape },
        { "circle", "Circle", "Shape", EntityTypes::Shape },
        { "hexagon", "Hexagon", "Shape", EntityTypes::Shape },
        { "octahedron", "Octahedron", "Shape", EntityTypes::Shape },
        { "icosahedron", "Icosahedron", "Shape", EntityTypes::Shape },
        { "point", "Point Light", "Light", EntityTypes::Light },
        { "spot", "Spot Light", "Light", EntityTypes::Light },
        { "probe", "Reflection Probe", "Light", EntityTypes::Shape },
        { "text", "Text", "", EntityTypes::Text, true },
        { "image", "Image", "", EntityTypes::Image },
        { "web", "Web Page", "", EntityTypes::Web },
        { "zone", "Zone", "", EntityTypes::Zone },
        { "particles", "Particles", "", EntityTypes::ParticleEffect },
        { "material", "Material", "", EntityTypes::Material },
        { "grid", "Grid", "", EntityTypes::Grid },
        { "terrain", "Terrain", "", EntityTypes::Terrain },
        { "track", "Track", "", EntityTypes::Track },
        { "trackcar", "Track Car", "", EntityTypes::Shape },
        // ready-made things (Caity, 10-24): a mirror, and vehicles to drive at once, the Test Track's own as physics
        // tuned them (tests-manual/physics-baseline: landrover.js, kart-rig.js, aircraft-rig.js, airship-rig.js; the monorail's
        // loop and car). Later here: a scripted-avatar slot, a sound emitter, a wind source
        { "mirror", "Mirror", "Special items", EntityTypes::Shape, true },
        { "vehicle-car", "Car", "Special items/Vehicles", EntityTypes::Box, false, "editor/vehicles/car.json", 0.0f, false,
          "A four-wheeled car: get in its driver's seat to drive" },
        { "vehicle-kart", "Go-kart", "Special items/Vehicles", EntityTypes::Box, false, "editor/vehicles/kart.json", 0.0f, false,
          "A one-seat go-kart, small enough for a doorway" },
        { "vehicle-rail", "Rail", "Special items/Vehicles", EntityTypes::Track, false, "editor/vehicles/rail.json", 20.0f,
          false, "A loop of monorail track with a car that stops at its station: walk up the ramp to the platform to board" },
        { "vehicle-airship", "Airship", "Special items/Vehicles", EntityTypes::Box, false, "editor/vehicles/airship.json",
          25.0f, false, "A small airship with a deck to walk on, 36 m long: it goes 25 m ahead of you" },
        { "vehicle-aircraft", "Aircraft", "Special items/Vehicles", EntityTypes::Box, false, "editor/vehicles/aircraft.json",
          6.0f, false, "A small aircraft with a flight assistant. It flies best in a world with the physics server on" },
        { "vehicle-watercraft", "Watercraft (coming later)", "Special items/Vehicles", EntityTypes::Box, false, "", 0.0f,
          true, "Boats need water physics, which isn't built yet" },
    };
    return ITEMS;
}

const EditorCreate::Item* EditorCreate::item(const QString& key) {
    for (const auto& item : items()) {
        if (item.key == key) {
            return &item;
        }
    }
    return nullptr;
}

EntityItemProperties EditorCreate::properties(const QString& key, const glm::vec3& position, float cameraYaw) {
    if (key == "empty") {
        // a parent with nothing of its own to show, as Unity's empty GameObject: the engine has no such type, so a
        // small cube that is not drawn, not collided with and so not picked, found in the Hierarchy; children follow
        // it, and it becomes a cube by being made visible
        auto p = shape("Cube", position);
        p.setName("Empty");
        p.setDimensions(glm::vec3(EMPTY_SIZE));
        p.setVisible(false);
        p.setCollisionless(true);
        return p;
    }
    if (key == "probe") {
        // a reflection probe (LIGHTMAPS.md §7): an invisible, collisionless box, the region whose surfaces reflect
        // the probe's cube map (userData "reflectionProbe": the shape renderer and ReflectionProbeStage); a room's
        // worth to start with, the probe at the entity's position
        auto p = shape("Cube", position);
        p.setName("Reflection Probe");
        p.setDimensions(glm::vec3(5.0f, 3.0f, 5.0f));
        p.setVisible(false);
        p.setCollisionless(true);
        p.setUserData("{\"reflectionProbe\": {\"intensity\": 1, \"blendDistance\": 1, \"boxProjection\": true}}");
        return p;
    }
    if (key == "cube") { return shape("Cube", position); }
    if (key == "sphere") { return shape("Sphere", position); }
    if (key == "cylinder") { return shape("Cylinder", position); }
    if (key == "cone") { return shape("Cone", position); }
    if (key == "hexagon") { return shape("Hexagon", position); }
    if (key == "octahedron") { return shape("Octahedron", position); }
    if (key == "icosahedron") { return shape("Icosahedron", position); }
    if (key == "mirror") {
        // a thin dark slab facing the camera, its front the glass (userData "mirror": the shape renderer and Mirrors.h)
        auto p = shape("Cube", position);
        p.setName("Mirror");
        p.setDimensions(glm::vec3(1.0f, 2.0f, 0.02f));
        p.setColor(glm::u8vec3(40, 44, 48));
        p.setUserData("{\"mirror\": \"screen\"}");
        p.setRotation(facing(cameraYaw));
        return p;
    }
    if (key == "quad" || key == "circle") {
        auto p = shape(key == "quad" ? "Quad" : "Circle", position);
        p.setDimensions(FLAT_DIMENSIONS);   // 1 x 1 m, lying flat (a 1 x 1 m upright panel came out as a line)
        p.setRotation(facing(cameraYaw));
        return p;
    }

    EntityItemProperties p;
    p.setPosition(position);
    if (key == "point" || key == "spot") {
        p.setType(EntityTypes::Light);
        p.setName(key == "spot" ? "Spot Light" : "Point Light");
        p.setDimensions(glm::vec3(LIGHT_RANGE * 2.0f));   // the light reaches half its dimensions
        p.setColor(WHITE);
        p.setIntensity(5.0f);
        p.setFalloffRadius(1.0f);
        p.setIsSpotlight(key == "spot");
        p.setExponent(1.0f);
        p.setCutoff(45.0f);
        if (key == "spot") {
            p.setRotation(facing(cameraYaw) * glm::angleAxis(-glm::half_pi<float>(), Vectors::RIGHT));   // down
        }
    } else if (key == "text") {
        p.setType(EntityTypes::Text);
        p.setName("Text");
        p.setText("Text");
        p.setDimensions(glm::vec3(0.65f, 0.3f, 0.01f));
        p.setLineHeight(0.06f);
        p.setTextColor(WHITE);
        p.setBackgroundColor(BLACK);
        p.setRotation(facing(cameraYaw));
    } else if (key == "image") {
        p = imageProperties(QUrl(DEFAULT_IMAGE), position, cameraYaw);
        p.setName("Image");
    } else if (key == "web") {
        p.setType(EntityTypes::Web);
        p.setName("Web Page");
        p.setSourceUrl(DEFAULT_WEB_PAGE);
        p.setDimensions(glm::vec3(1.6f, 0.9f, 0.01f));
        p.setDPI(30);
        p.setMaxFPS(30);
        p.setRotation(facing(cameraYaw));
    } else if (key == "zone") {
        p.setType(EntityTypes::Zone);
        p.setName("Zone");
        p.setDimensions(glm::vec3(10.0f));
        p.setShapeType(SHAPE_TYPE_BOX);
        // its own sun, casting shadows: a zone without them looked flat (Caity, 10-25). The engine's defaults (the
        // sun inherited, no shadows) stay, so saved worlds and scripts' zones are as they were
        p.setKeyLightMode((uint32_t)COMPONENT_MODE_ENABLED);
        p.getKeyLight().setCastShadows(true);
    } else if (key == "particles") {
        p.setType(EntityTypes::ParticleEffect);
        p.setName("Particles");
        p.setDimensions(UNIT_DIMENSIONS);
        p.setTextures(DEFAULT_PARTICLE);
        p.setLifespan(1.5f);
        p.setMaxParticles(10);
        p.setEmitRate(5.5f);
        p.setEmitSpeed(0.0f);
        p.setEmitDimensions(glm::vec3(0.0f));
        p.setEmitterShouldTrail(true);
        p.setParticleRadius(0.25f);
        p.setColor(WHITE);
        p.setAlpha(0.0f);
        p.setAlphaStart(1.0f);
        p.setEmitAcceleration(glm::vec3(0.0f, 2.5f, 0.0f));
        p.setPolarFinish(glm::pi<float>());
        p.setAzimuthStart(-glm::pi<float>());
        p.setAzimuthFinish(glm::pi<float>());
    } else if (key == "material") {
        p.setType(EntityTypes::Material);
        p.setName("Material");
        p.setDimensions(glm::vec3(0.1f));
        p.setMaterialURL("materialData");
        // as a new URP/Lit material starts in Unity: maps packed its way, Smoothness 0.5, Metallic 0
        p.setMaterialData("{\"materials\":{\"packing\":\"unity\",\"albedo\":[0.8,0.8,0.8],\"smoothness\":0.5,"
                          "\"metallic\":0.0}}");
        p.setPriority(1);
    } else if (key == "grid") {
        p.setType(EntityTypes::Grid);
        p.setName("Grid");
        p.setDimensions(glm::vec3(10.0f, 10.0f, 0.01f));
        p.setColor(GRID_COLOR);
        p.setFollowCamera(false);
        p.setMajorGridEvery(5);
        p.setMinorGridEvery(1.0f);
        p.setRotation(glm::angleAxis(-glm::half_pi<float>(), Vectors::RIGHT));   // flat on the ground
    } else if (key == "terrain") {
        // 64 x 32 x 64 voxels at half a metre; the ground starts flat halfway up the box (TERRAIN.md)
        p.setType(EntityTypes::Terrain);
        p.setName("Terrain");
        p.setDimensions(glm::vec3(32.0f, 16.0f, 32.0f));
        p.setVoxelSize(0.5f);
    } else if (key == "track") {
        // a loop of eight points round an oval 30 by 15 m, the monorail's beam under its ride line (PHYSICS.md 4.8),
        // placed with the ride line a car's height above the surface; the Inspector's Points list and the viewport's
        // handles take it from here
        p.setType(EntityTypes::Track);
        p.setName("Track");
        QVector<glm::vec3> points;
        for (int i = 0; i < TRACK_LOOP_POINTS; ++i) {
            float angle = glm::two_pi<float>() * (float)i / (float)TRACK_LOOP_POINTS;
            points << glm::vec3(TRACK_LOOP_HALF_LENGTH * std::cos(angle), 0.0f, TRACK_LOOP_HALF_WIDTH * std::sin(angle));
        }
        p.setTrackPoints(points);
        p.setTrackClosed(true);
        p.setTrackSpeed(5.0f);
        p.setTrackAcceleration(1.0f);
        p.setColor(TRACK_COLOR);
        p.setRestitution(0.0f);   // nothing bounces off a beam (the entity default of 0.5 threw a dropped ball off it)
        p.setFriction(0.8f);
        p.setDimensions(glm::vec3(2.0f * TRACK_LOOP_HALF_LENGTH + 2.0f, TRACK_LOOP_BOX_HEIGHT, 2.0f * TRACK_LOOP_HALF_WIDTH + 2.0f));
        p.setRegistrationPoint(glm::vec3(0.5f, TRACK_LOOP_REGISTRATION_Y, 0.5f));
    } else if (key == "trackcar") {
        // a car that is a moving frame: a floor to stand on that rides the Track selected when it is made (the
        // editor sets trackID then); walls, seats and a roof go on as its children
        p = shape("Cube", position);
        p.setName("Track Car");
        p.setDimensions(TRACK_CAR_DIMENSIONS);
        p.setColor(TRACK_CAR_COLOR);
        p.setFrame(true);
    }
    return p;
}

EntityItemProperties EditorCreate::modelProperties(const QUrl& url, const glm::vec3& position) {
    EntityItemProperties p;
    p.setType(EntityTypes::Model);
    QString name = QFileInfo(url.path()).completeBaseName();
    p.setName(name.isEmpty() ? "Model" : name);
    p.setModelURL(url.toString());
    p.setDimensions(MODEL_PLACEHOLDER_DIMENSIONS);   // until the model loads and reports its own
    p.setShapeType(SHAPE_TYPE_NONE);
    p.setPosition(position);
    return p;
}

EntityItemProperties EditorCreate::imageProperties(const QUrl& url, const glm::vec3& position, float cameraYaw) {
    EntityItemProperties p;
    p.setType(EntityTypes::Image);
    QString name = QFileInfo(url.path()).completeBaseName();
    p.setName(name.isEmpty() ? "Image" : name);
    p.setImageURL(url.toString());
    p.setDimensions(PANEL_DIMENSIONS);
    p.setKeepAspectRatio(true);
    p.setEmissive(true);
    p.setCollisionless(true);
    p.setPosition(position);
    p.setRotation(facing(cameraYaw));
    return p;
}

float EditorCreate::lift(const EntityItemProperties& properties) {
    switch (properties.getType()) {
        case EntityTypes::Light: return properties.getIsSpotlight() ? SPOT_LIGHT_LIFT : POINT_LIGHT_LIFT;
        case EntityTypes::Shape:
            if (properties.getShape() == "Quad" || properties.getShape() == "Circle") {
                return FLAT_LIFT;
            }
            return properties.getDimensions().y * 0.5f;
        case EntityTypes::Material: return 0.0f;
        case EntityTypes::Grid: return 0.02f;
        case EntityTypes::Terrain: return 0.0f;   // the ground starts halfway up the box, so the box's centre sits at the surface
        case EntityTypes::Track: return TRACK_LIFT;   // the ride line a car's height up, the beam under it, the pylons to the ground
        default: return properties.getDimensions().y * 0.5f;   // resting on the surface
    }
}

bool EditorCreate::isModelFile(const QString& name) {
    static const QStringList EXTENSIONS { "glb", "gltf", "vrm", "fbx", "obj" };
    return EXTENSIONS.contains(QFileInfo(name).suffix().toLower());
}

bool EditorCreate::isImageFile(const QString& name) {
    static const QStringList EXTENSIONS { "png", "jpg", "jpeg", "gif", "webp", "bmp" };
    return EXTENSIONS.contains(QFileInfo(name).suffix().toLower());
}

// --- the upload -----------------------------------------------------------------------------------

EditorUpload::EditorUpload(QObject* parent) : QObject(parent) {
}

bool EditorUpload::start(const QString& localPath, const QString& remoteFolder, QString& error) {
    auto file = new QFile(localPath);
    if (!file->open(QIODevice::ReadOnly)) {
        error = "cannot read " + localPath;
        delete file;
        return false;
    }
    QFileInfo info(localPath);
    return send(file, info.fileName(), remoteFolder, QMimeDatabase().mimeTypeForFile(localPath).name(), info.size(), error);
}

bool EditorUpload::startData(const QByteArray& data, const QString& fileName, const QString& remoteFolder, const QString& mimeType,
                             QString& error) {
    auto buffer = new QBuffer();
    buffer->setData(data);
    buffer->open(QIODevice::ReadOnly);
    return send(buffer, fileName, remoteFolder, mimeType, data.size(), error);
}

bool EditorUpload::send(QIODevice* body, const QString& fileName, const QString& remoteFolder, const QString& mimeType, qint64 size,
                        QString& error) {
    auto accountManager = DependencyManager::get<AccountManager>();
    if (!accountManager->isLoggedIn()) {
        error = "you're signed out";   // mary 11-08: one wording for it across the app
        delete body;
        return false;
    }
    _fileName = safeFileName(fileName);
    QString remotePath = remoteFolder + "/" + _fileName;

    // the server reads the path from the form before it streams the file, so the path part goes first
    auto multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart pathPart;
    pathPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"path\""));
    pathPart.setBody(remotePath.toUtf8());
    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
        QVariant("form-data; name=\"file\"; filename=\"" + _fileName + "\""));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(mimeType));
    filePart.setBodyDevice(body);
    body->setParent(multiPart);
    multiPart->append(pathPart);
    multiPart->append(filePart);

    QNetworkRequest request = accountManager->createRequest(FILES_API_PATH, AccountManagerAuth::Required);
    if (request.url().isEmpty()) {
        error = "no access token";
        delete multiPart;
        return false;
    }
    _timer.start();
    qCDebug(interfaceapp) << "editor upload started" << remotePath << size << "bytes";
    auto reply = NetworkAccessManager::getInstance().put(request, multiPart);
    multiPart->setParent(reply);
    connect(reply, &QNetworkReply::uploadProgress, this, &EditorUpload::progress);
    connect(reply, &QNetworkReply::finished, this, [this, reply, remotePath] {
        reply->deleteLater();
        QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            QString detail = QJsonDocument::fromJson(body).object().value("message").toString();
            qCWarning(interfaceapp) << "editor upload failed" << remotePath << "after" << _timer.elapsed() << "ms:"
                << reply->errorString() << detail;
            emit failed(detail.isEmpty() ? reply->errorString() : detail);
            return;
        }
        QString url = QJsonDocument::fromJson(body).object().value("url").toString();
        if (url.isEmpty()) {
            qCWarning(interfaceapp) << "editor upload: no url in the reply for" << remotePath << body.left(200);
            emit failed("the server did not return a URL");
            return;
        }
        qCDebug(interfaceapp) << "editor upload done" << remotePath << "->" << url << "in" << _timer.elapsed() << "ms";
        emit finished(url);
    });
    return true;
}
