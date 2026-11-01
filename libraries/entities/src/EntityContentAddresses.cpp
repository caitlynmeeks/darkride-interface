//
//  EntityContentAddresses.cpp
//  libraries/entities/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "EntityContentAddresses.h"

#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>

#include "EntityItemProperties.h"

namespace EntityContentAddresses {

    static const QString APP_RESOURCES_PREFIX = QStringLiteral("/~/");
    static const QString FILE_SCHEME = QStringLiteral("file");
    static const QString INLINE_MATERIAL = QStringLiteral("materialData");

    // "/~/rest" is the app's own resources folder (PathUtils::expandToLocalDataAbsolutePath puts rest after the
    // folder's path), which only ".." can leave: the rest is decoded once, as the loader decodes it, its separators
    // made the same, its leading separators dropped (they only double the folder's own) and the path cleaned; what
    // still starts with ".." climbs out.
    static bool climbsOutOfResources(const QString& address) {
        QString rest = QUrl::fromPercentEncoding(address.mid(APP_RESOURCES_PREFIX.size()).toUtf8());
        rest.replace('\\', '/');
        while (rest.startsWith('/')) {
            rest.remove(0, 1);
        }
        const QString cleaned = QDir::cleanPath(rest);
        return cleaned == QStringLiteral("..") || cleaned.startsWith(QStringLiteral("../"));
    }

    // "C:\...", "C:/..." or "C:" alone
    static bool isDriveLetter(const QString& address) {
        return address.size() >= 2 && address.at(0).isLetter() && address.at(1) == ':' &&
               (address.size() == 2 || address.at(2) == '/' || address.at(2) == '\\');
    }

    bool isPathOnThisComputer(const QString& address) {
        const QString trimmed = address.trimmed();
        if (trimmed.isEmpty()) {
            return false;
        }
        if (trimmed.startsWith(APP_RESOURCES_PREFIX)) {
            return climbsOutOfResources(trimmed);
        }
        if (trimmed.startsWith('/') || trimmed.startsWith('\\') || isDriveLetter(trimmed)) {
            return true;
        }
        return QUrl(trimmed).scheme() == FILE_SCHEME;   // QUrl lowercases the scheme, so "FILE:" is caught too
    }

    bool isOnThisComputer(const QString& address) {
        const QString trimmed = address.trimmed();
        if (trimmed.isEmpty()) {
            return false;
        }
        if (trimmed.startsWith(APP_RESOURCES_PREFIX)) {
            return climbsOutOfResources(trimmed);
        }
        // the schemes ResourceManager::createResourceRequest fetches over the network or from the app's own resources
        // (getKnownUrls there, less file:); every other address is read as "file:///" + the address. Nearly every
        // real address starts with one of them: that is settled by a prefix compare, before any parsing.
        static const QStringList FETCHED_PREFIXES = { QStringLiteral("http://"), QStringLiteral("https://"),
                                                      QStringLiteral("atp:"),    QStringLiteral("tea://"),
                                                      QStringLiteral("qrc:"),    QStringLiteral("ftp://") };
        for (const QString& prefix : FETCHED_PREFIXES) {
            if (trimmed.startsWith(prefix, Qt::CaseInsensitive)) {
                return false;
            }
        }
        if (isPathOnThisComputer(trimmed)) {
            return true;
        }
        static const QStringList FETCHED_SCHEMES = { QStringLiteral("http"), QStringLiteral("https"),
                                                     QStringLiteral("ftp"),  QStringLiteral("atp"),
                                                     QStringLiteral("tea"),  QStringLiteral("qrc") };
        const QString scheme = QUrl(trimmed).scheme();
        return scheme.isEmpty() || !FETCHED_SCHEMES.contains(scheme);
    }

    // href is followed by AddressManager::handleUrl: a file: address loads a world file from this computer; a bare path
    // is a viewpoint ("/x,y,z"), a bare name a world's name, and another scheme is ignored
    static bool isWorldFileOnThisComputer(const QString& href) {
        return QUrl(href.trimmed()).scheme() == FILE_SCHEME;
    }

    static QString compactJson(const QJsonObject& object) {
        return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    }

    bool readTexturesMap(const QString& textures, QJsonObject& map) {
        // the Model renderer's reading, step for step (parseTexturesToMap, libraries/shared RegisteredMetaTypes.cpp:
        // no trim; a value whose RAW first character isn't '{' is the legacy ",\n"-separated list of name:"address",
        // rewritten into JSON; what then fails to parse is ignored). testTexturesReadAsRenderer keeps the two together.
        if (textures.isEmpty()) {
            return false;
        }
        QString json = textures;
        if (*json.cbegin() != '{') {
            json.replace(QStringLiteral(":\""), QStringLiteral("\":\""));
            json.replace(QStringLiteral(",\n"), QStringLiteral(",\""));
            json = "{\"" + json + "}";
        }
        QJsonParseError error;
        QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            return false;
        }
        map = document.object();
        return true;
    }

    // the textures property: a Model's map of texture name -> address (readTexturesMap), each entry on this computer
    // emptied; what the Model renderer can't read is a ParticleEffect's or PolyLine's one address, or nothing at all,
    // and is emptied unless it is a fetched address (a value no renderer reads is lost to no one)
    static bool clearTextures(QString& textures) {
        if (textures.isEmpty()) {
            return false;
        }
        QJsonObject map;
        if (!readTexturesMap(textures, map)) {
            if (isOnThisComputer(textures)) {
                textures = QString();
                return true;
            }
            return false;
        }
        bool changed = false;
        for (const QString& name : map.keys()) {
            const QJsonValue value = map.value(name);
            if (value.isString() && isOnThisComputer(value.toString())) {
                map.insert(name, QString());
                changed = true;
            }
        }
        if (changed) {
            textures = compactJson(map);
        }
        return changed;
    }

    // the addresses a procedural shader's data holds (ProceduralData::parse, libraries/procedural Procedural.cpp): the
    // fragment shader as "fragmentShaderURL" or "shaderUrl", the vertex shader, and the "channels" pictures; each one
    // on this computer is emptied
    static bool clearProceduralAddresses(QJsonObject& procedural) {
        bool changed = false;
        static const QStringList SHADER_KEYS = { QStringLiteral("shaderUrl"), QStringLiteral("fragmentShaderURL"),
                                                 QStringLiteral("vertexShaderURL") };
        for (const QString& key : SHADER_KEYS) {
            if (procedural.value(key).isString() && isOnThisComputer(procedural.value(key).toString())) {
                procedural.insert(key, QString());
                changed = true;
            }
        }
        QJsonArray channels = procedural.value(QStringLiteral("channels")).toArray();
        bool channelsChanged = false;
        for (int i = 0; i < channels.size(); i++) {
            if (channels.at(i).isString() && isOnThisComputer(channels.at(i).toString())) {
                channels.replace(i, QString());
                channelsChanged = true;
            }
        }
        if (channelsChanged) {
            procedural.insert(QStringLiteral("channels"), channels);
            changed = true;
        }
        return changed;
    }

    // one material of a material JSON (Entities.Material, parseJSONMaterial in ProceduralMaterialCache.cpp): every
    // "...Map" picture and "lightMapDirection" (the keys it resolves against the material's address), and the
    // addresses of its "procedural" shader
    static bool clearMaterial(QJsonObject& material) {
        bool changed = false;
        for (const QString& key : material.keys()) {
            const QJsonValue value = material.value(key);
            const bool picture = key.endsWith(QStringLiteral("Map")) || key == QStringLiteral("lightMapDirection");
            if (picture && value.isString() && isOnThisComputer(value.toString())) {
                material.insert(key, QString());
                changed = true;
            } else if (key == QStringLiteral("procedural") && value.isObject()) {
                QJsonObject procedural = value.toObject();
                if (clearProceduralAddresses(procedural)) {
                    material.insert(key, procedural);
                    changed = true;
                }
            }
        }
        return changed;
    }

    // the materialData property: an Entities.MaterialResource, "materials" one material or a list of them
    static bool clearMaterialData(QString& materialData) {
        if (materialData.trimmed().isEmpty()) {
            return false;
        }
        QJsonParseError error;
        QJsonDocument document = QJsonDocument::fromJson(materialData.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            return false;
        }
        QJsonObject resource = document.object();
        const QJsonValue materials = resource.value(QStringLiteral("materials"));
        bool changed = false;
        if (materials.isArray()) {
            QJsonArray list = materials.toArray();
            for (int i = 0; i < list.size(); i++) {
                if (list.at(i).isObject()) {
                    QJsonObject material = list.at(i).toObject();
                    if (clearMaterial(material)) {
                        list.replace(i, material);
                        changed = true;
                    }
                }
            }
            if (changed) {
                resource.insert(QStringLiteral("materials"), list);
            }
        } else if (materials.isObject()) {
            QJsonObject material = materials.toObject();
            if (clearMaterial(material)) {
                resource.insert(QStringLiteral("materials"), material);
                changed = true;
            }
        }
        if (changed) {
            materialData = compactJson(resource);
        }
        return changed;
    }

    static const QString IMPOSTOR_KEY = QStringLiteral("impostor");
    static const QString PROCEDURAL_ENTITY_KEY = QStringLiteral("ProceduralEntity");

    // userData holds two kinds of address. The "impostor" entry (ImpostorEntry::parse, ImpostorCard.cpp) names the
    // card's atlases "albedo", "normal" and "material": one on this computer and the entry goes whole (a card can't
    // draw without its atlases). A procedural shader's data (ProceduralData::getProceduralData, Procedural.cpp; a
    // Shape's material and a Zone's skybox read it) sits under "ProceduralEntity" or, when that key is absent, is the
    // whole object: its shader and channel addresses are emptied one by one. The pre-checks are the readers' own:
    // the card readers (ImpostorCard.cpp:77, RenderableEntityItem.cpp:687) look only at userData that contains
    // "impostor", so the same contains() here skips what they skip; the procedural keys are checked for the same
    // reason before parsing. userData that isn't JSON is left as it is, as every reader leaves it.
    static void clearUserData(QString& userData, QStringList& cleared) {
        const bool mayHoldImpostor = userData.contains(IMPOSTOR_KEY);
        const bool mayHoldShader = userData.contains(QStringLiteral("shaderUrl")) ||
                                   userData.contains(QStringLiteral("ShaderURL")) ||
                                   userData.contains(QStringLiteral("channels"));
        if (!mayHoldImpostor && !mayHoldShader) {
            return;
        }
        QJsonParseError error;
        QJsonDocument document = QJsonDocument::fromJson(userData.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            return;
        }
        QJsonObject object = document.object();
        bool changed = false;
        if (mayHoldImpostor && object.value(IMPOSTOR_KEY).isObject()) {
            const QJsonObject impostor = object.value(IMPOSTOR_KEY).toObject();
            static const QStringList ATLASES = { QStringLiteral("albedo"), QStringLiteral("normal"),
                                                 QStringLiteral("material") };
            for (const QString& atlas : ATLASES) {
                if (impostor.value(atlas).isString() && isOnThisComputer(impostor.value(atlas).toString())) {
                    object.remove(IMPOSTOR_KEY);
                    cleared << QStringLiteral("userData.impostor");
                    changed = true;
                    break;
                }
            }
        }
        if (mayHoldShader) {
            if (object.contains(PROCEDURAL_ENTITY_KEY)) {
                if (object.value(PROCEDURAL_ENTITY_KEY).isObject()) {
                    QJsonObject procedural = object.value(PROCEDURAL_ENTITY_KEY).toObject();
                    if (clearProceduralAddresses(procedural)) {
                        object.insert(PROCEDURAL_ENTITY_KEY, procedural);
                        cleared << QStringLiteral("userData.procedural");
                        changed = true;
                    }
                }
            } else if (clearProceduralAddresses(object)) {
                cleared << QStringLiteral("userData.procedural");
                changed = true;
            }
        }
        if (changed) {
            userData = compactJson(object);
        }
    }

    QStringList clearThisComputer(EntityItemProperties& properties) {
        // only a property the object carries as changed is looked at: a partial edit ({ name: "x" }) comes back
        // untouched, and no unchanged property is marked changed (that would send "" over the entity's real address)
        QStringList cleared;
        auto clearIf = [&cleared](const char* name, bool changed, const QString& value, auto&& setEmpty,
                                  bool (*test)(const QString&)) {
            if (changed && test(value)) {
                setEmpty();
                cleared << QString::fromLatin1(name);
            }
        };
        AnimationPropertyGroup& animation = properties.getAnimation();
        SkyboxPropertyGroup& skybox = properties.getSkybox();
        AmbientLightPropertyGroup& ambientLight = properties.getAmbientLight();
        GrabPropertyGroup& grab = properties.getGrab();

        // fetched by the resource manager
        clearIf("modelURL", properties.modelURLChanged(), properties.getModelURL(),
                [&] { properties.setModelURL(QString()); }, isOnThisComputer);
        clearIf("compoundShapeURL", properties.compoundShapeURLChanged(), properties.getCompoundShapeURL(),
                [&] { properties.setCompoundShapeURL(QString()); }, isOnThisComputer);
        clearIf("collisionSoundURL", properties.collisionSoundURLChanged(), properties.getCollisionSoundURL(),
                [&] { properties.setCollisionSoundURL(QString()); }, isOnThisComputer);
        clearIf("animation.url", animation.urlChanged(), animation.getURL(),
                [&] { animation.setURL(QString()); }, isOnThisComputer);
        clearIf("skybox.url", skybox.urlChanged(), skybox.getURL(),
                [&] { skybox.setURL(QString()); }, isOnThisComputer);
        clearIf("ambientLight.ambientURL", ambientLight.ambientURLChanged(), ambientLight.getAmbientURL(),
                [&] { ambientLight.setAmbientURL(QString()); }, isOnThisComputer);
        clearIf("imageURL", properties.imageURLChanged(), properties.getImageURL(),
                [&] { properties.setImageURL(QString()); }, isOnThisComputer);
        if (!properties.getMaterialURL().startsWith(INLINE_MATERIAL)) {
            clearIf("materialURL", properties.materialURLChanged(), properties.getMaterialURL(),
                    [&] { properties.setMaterialURL(QString()); }, isOnThisComputer);
        }
        clearIf("terrainURL", properties.terrainURLChanged(), properties.getTerrainURL(),
                [&] { properties.setTerrainURL(QString()); }, isOnThisComputer);
        clearIf("grab.equippableIndicatorURL", grab.equippableIndicatorURLChanged(), grab.getEquippableIndicatorURL(),
                [&] { grab.setEquippableIndicatorURL(QString()); }, isOnThisComputer);
        clearIf("filterURL", properties.filterURLChanged(), properties.getFilterURL(),
                [&] { properties.setFilterURL(QString()); }, isOnThisComputer);

        // loaded by other means: only a real path or file: address reads this computer
        clearIf("sourceUrl", properties.sourceUrlChanged(), properties.getSourceUrl(),
                [&] { properties.setSourceUrl(QString()); }, isPathOnThisComputer);
        clearIf("scriptURL", properties.scriptURLChanged(), properties.getScriptURL(),
                [&] { properties.setScriptURL(QString()); }, isPathOnThisComputer);
        clearIf("font", properties.fontChanged(), properties.getFont(),
                [&] { properties.setFont(QString()); }, isPathOnThisComputer);
        clearIf("href", properties.hrefChanged(), properties.getHref(),
                [&] { properties.setHref(QString()); }, isWorldFileOnThisComputer);

        // several addresses inside one property
        if (properties.texturesChanged()) {
            QString textures = properties.getTextures();
            if (clearTextures(textures)) {
                properties.setTextures(textures);
                cleared << QStringLiteral("textures");
            }
        }
        if (properties.materialDataChanged()) {
            QString materialData = properties.getMaterialData();
            if (clearMaterialData(materialData)) {
                properties.setMaterialData(materialData);
                cleared << QStringLiteral("materialData");
            }
        }
        if (properties.userDataChanged()) {
            QString userData = properties.getUserData();
            const int before = cleared.size();
            clearUserData(userData, cleared);
            if (cleared.size() != before) {
                properties.setUserData(userData);
            }
        }
        return cleared;
    }

    const QStringList& propertyNames() {
        static const QStringList NAMES = {
            QStringLiteral("modelURL"),   QStringLiteral("compoundShapeURL"), QStringLiteral("collisionSoundURL"),
            QStringLiteral("animation.url"), QStringLiteral("skybox.url"), QStringLiteral("ambientLight.ambientURL"),
            QStringLiteral("imageURL"),   QStringLiteral("materialURL"), QStringLiteral("terrainURL"),
            QStringLiteral("grab.equippableIndicatorURL"), QStringLiteral("filterURL"),
            QStringLiteral("sourceUrl"),  QStringLiteral("scriptURL"), QStringLiteral("font"), QStringLiteral("href")
        };
        return NAMES;
    }

}
