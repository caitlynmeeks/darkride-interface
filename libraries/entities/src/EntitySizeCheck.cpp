//
//  EntitySizeCheck.cpp
//  libraries/entities/src
//
//  Created by servers on 2026-11-07.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "EntitySizeCheck.h"

#include <mutex>
#include <set>
#include <utility>

#include <QCoreApplication>
#include <QLocale>

#include <ScriptCaller.h>

#include "EntitiesLogging.h"
#include "EntityItem.h"

namespace {

// Below the least any entity holds in one value (about 1,300 bytes: the stream's room less the entity's own header),
// so a value this size or smaller is never measured.
const int MEASURE_FROM_BYTES = 1200;

// A changed value's size in bytes: exact (its UTF-8, for the message), or an upper bound that costs nothing (UTF-16
// code units take at most 3 bytes each in UTF-8). -1 when the value isn't changed.
using ValueSize = int (*)(const EntityItemProperties& properties, bool exact);

int textSize(bool changed, const QString& text, bool exact) {
    if (!changed) {
        return -1;
    }
    return exact ? (int)text.toUtf8().size() : (int)text.size() * 3;
}

template <typename T>
int vectorSize(bool changed, const QVector<T>& values) {
    return changed ? (int)values.size() * (int)sizeof(T) : -1;
}

#define TEXT_VALUE(PROP, NAME, CHANGED, VALUE) \
    { PROP, NAME, [](const EntityItemProperties& p, bool exact) { return textSize(p.CHANGED, p.VALUE, exact); } }
#define VECTOR_VALUE(PROP, NAME, CHANGED, VALUE) \
    { PROP, NAME, [](const EntityItemProperties& p, bool) { return vectorSize(p.CHANGED, p.VALUE); } }

// `property` alone doesn't name a value: properties of different entity types share numbers (a Model's modelURL and a
// Text's text), so each value carries its own name as scripts know it.
struct LongValue {
    EntityPropertyList property;
    const char* name;
    ValueSize size;
};

// Every value whose size the sender chooses: text, bytes and lists. Fixed-size values (numbers, vectors, colors) are
// never too big.
const LongValue LONG_VALUES[] = {
    TEXT_VALUE(PROP_NAME, "name", nameChanged(), getName()),
    TEXT_VALUE(PROP_USER_DATA, "userData", userDataChanged(), getUserData()),
    TEXT_VALUE(PROP_PRIVATE_USER_DATA, "privateUserData", privateUserDataChanged(), getPrivateUserData()),
    TEXT_VALUE(PROP_HREF, "href", hrefChanged(), getHref()),
    TEXT_VALUE(PROP_DESCRIPTION, "description", descriptionChanged(), getDescription()),
    TEXT_VALUE(PROP_COLLISION_SOUND_URL, "collisionSoundURL", collisionSoundURLChanged(), getCollisionSoundURL()),
    { PROP_ACTION_DATA, "actionData", [](const EntityItemProperties& p, bool) {
        return p.actionDataChanged() ? (int)p.getActionData().size() : -1; } },
    TEXT_VALUE(PROP_SCRIPT, "script", scriptChanged(), getScript()),
    TEXT_VALUE(PROP_SERVER_SCRIPTS, "serverScripts", serverScriptsChanged(), getServerScripts()),
    TEXT_VALUE(PROP_ITEM_NAME, "itemName", itemNameChanged(), getItemName()),
    TEXT_VALUE(PROP_ITEM_DESCRIPTION, "itemDescription", itemDescriptionChanged(), getItemDescription()),
    TEXT_VALUE(PROP_ITEM_CATEGORIES, "itemCategories", itemCategoriesChanged(), getItemCategories()),
    TEXT_VALUE(PROP_ITEM_ARTIST, "itemArtist", itemArtistChanged(), getItemArtist()),
    TEXT_VALUE(PROP_ITEM_LICENSE, "itemLicense", itemLicenseChanged(), getItemLicense()),
    TEXT_VALUE(PROP_MARKETPLACE_ID, "marketplaceID", marketplaceIDChanged(), getMarketplaceID()),
    TEXT_VALUE(PROP_CERTIFICATE_ID, "certificateID", certificateIDChanged(), getCertificateID()),
    TEXT_VALUE(PROP_CERTIFICATE_TYPE, "certificateType", certificateTypeChanged(), getCertificateType()),
    TEXT_VALUE(PROP_COMPOUND_SHAPE_URL, "compoundShapeURL", compoundShapeURLChanged(), getCompoundShapeURL()),
    TEXT_VALUE(PROP_TEXTURES, "textures", texturesChanged(), getTextures()),
    TEXT_VALUE(PROP_MODEL_URL, "modelURL", modelURLChanged(), getModelURL()),
    TEXT_VALUE(PROP_BLENDSHAPE_COEFFICIENTS, "blendshapeCoefficients", blendshapeCoefficientsChanged(), getBlendshapeCoefficients()),
    TEXT_VALUE(PROP_TEXT, "text", textChanged(), getText()),
    TEXT_VALUE(PROP_FONT, "font", fontChanged(), getFont()),
    TEXT_VALUE(PROP_FILTER_URL, "filterURL", filterURLChanged(), getFilterURL()),
    TEXT_VALUE(PROP_SOURCE_URL, "sourceUrl", sourceUrlChanged(), getSourceUrl()),
    TEXT_VALUE(PROP_SCRIPT_URL, "scriptURL", scriptURLChanged(), getScriptURL()),
    TEXT_VALUE(PROP_TRACK_PROFILE, "trackProfile", trackProfileChanged(), getTrackProfile()),
    TEXT_VALUE(PROP_SHAPE, "shape", shapeChanged(), getShape()),
    TEXT_VALUE(PROP_MATERIAL_URL, "materialURL", materialURLChanged(), getMaterialURL()),
    TEXT_VALUE(PROP_PARENT_MATERIAL_NAME, "parentMaterialName", parentMaterialNameChanged(), getParentMaterialName()),
    TEXT_VALUE(PROP_MATERIAL_DATA, "materialData", materialDataChanged(), getMaterialData()),
    TEXT_VALUE(PROP_IMAGE_URL, "imageURL", imageURLChanged(), getImageURL()),
    TEXT_VALUE(PROP_TERRAIN_URL, "terrainURL", terrainURLChanged(), getTerrainURL()),
    TEXT_VALUE(PROP_TERRAIN_LAYERS, "terrainLayers", terrainLayersChanged(), getTerrainLayers()),
    TEXT_VALUE(PROP_ANIMATION_URL, "animation.url", getAnimation().urlChanged(), getAnimation().getURL()),
    TEXT_VALUE(PROP_SKYBOX_URL, "skybox.url", getSkybox().urlChanged(), getSkybox().getURL()),
    TEXT_VALUE(PROP_AMBIENT_LIGHT_URL, "ambientLight.ambientURL", getAmbientLight().ambientURLChanged(), getAmbientLight().getAmbientURL()),
    TEXT_VALUE(PROP_GRAB_EQUIPPABLE_INDICATOR_URL, "grab.equippableIndicatorURL", getGrab().equippableIndicatorURLChanged(), getGrab().getEquippableIndicatorURL()),
    TEXT_VALUE(PROP_VEHICLE_TYPE, "vehicle.type", getVehicle().typeChanged(), getVehicle().getType()),
    TEXT_VALUE(PROP_VEHICLE_WHEELS, "vehicle.wheels", getVehicle().wheelsChanged(), getVehicle().getWheels()),
    TEXT_VALUE(PROP_VEHICLE_SEATS, "vehicle.seats", getVehicle().seatsChanged(), getVehicle().getSeats()),
    TEXT_VALUE(PROP_VEHICLE_GEAR_RATIOS, "vehicle.gearRatios", getVehicle().gearRatiosChanged(), getVehicle().getGearRatios()),
    TEXT_VALUE(PROP_VEHICLE_DRIVE_TYPE, "vehicle.driveType", getVehicle().driveTypeChanged(), getVehicle().getDriveType()),
    TEXT_VALUE(PROP_VEHICLE_WHEEL, "vehicle.wheel", getVehicle().wheelChanged(), getVehicle().getWheel()),
    TEXT_VALUE(PROP_VEHICLE_AXLE, "vehicle.axle", getVehicle().axleChanged(), getVehicle().getAxle()),
    VECTOR_VALUE(PROP_JOINT_ROTATIONS_SET, "jointRotationsSet", jointRotationsSetChanged(), getJointRotationsSet()),
    VECTOR_VALUE(PROP_JOINT_ROTATIONS, "jointRotations", jointRotationsChanged(), getJointRotations()),
    VECTOR_VALUE(PROP_JOINT_TRANSLATIONS_SET, "jointTranslationsSet", jointTranslationsSetChanged(), getJointTranslationsSet()),
    VECTOR_VALUE(PROP_JOINT_TRANSLATIONS, "jointTranslations", jointTranslationsChanged(), getJointTranslations()),
    VECTOR_VALUE(PROP_TRACK_POINTS, "trackPoints", trackPointsChanged(), getTrackPoints()),
    VECTOR_VALUE(PROP_TRACK_BANK, "trackBank", trackBankChanged(), getTrackBank()),
    VECTOR_VALUE(PROP_TRACK_DWELL, "trackDwell", trackDwellChanged(), getTrackDwell()),
    VECTOR_VALUE(PROP_LINE_POINTS, "linePoints", linePointsChanged(), getLinePoints()),
    VECTOR_VALUE(PROP_STROKE_WIDTHS, "strokeWidths", strokeWidthsChanged(), getStrokeWidths()),
    VECTOR_VALUE(PROP_STROKE_NORMALS, "normals", normalsChanged(), getNormals()),
    VECTOR_VALUE(PROP_STROKE_COLORS, "strokeColors", strokeColorsChanged(), getStrokeColors()),
};

#undef TEXT_VALUE
#undef VECTOR_VALUE

// The value `property` names in `properties`: the one of that number that is changed and long (numbers are shared)
const LongValue* longValueFor(EntityPropertyList property, const EntityItemProperties& properties) {
    for (const LongValue& value : LONG_VALUES) {
        if (value.property == property && value.size(properties, false) > MEASURE_FROM_BYTES) {
            return &value;
        }
    }
    return nullptr;
}

// "userData" -> "User Data", "modelURL" -> "Model URL", "animation.url" -> "Animation Url"
QString labelFor(const QString& name) {
    QString label;
    for (int i = 0; i < name.size(); i++) {
        QChar c = name[i];
        if (c == '.') {
            label += ' ';
            continue;
        }
        bool wordStart = i == 0 || name[i - 1] == '.' || (c.isUpper() && name[i - 1].isLower());
        if (wordStart && i > 0 && name[i - 1] != '.') {
            label += ' ';
        }
        label += wordStart ? c.toUpper() : c;
    }
    return label;
}

std::mutex reportedMutex;
std::set<std::pair<QUuid, int>> reported;
const size_t MAX_REPORTED = 1000;

}  // namespace

QString EntitySizeProblem::sentence(const QString& otherLabel) const {
    const QLocale english(QLocale::English, QLocale::UnitedStates);
    return QString("Too big to save: %1 is %2 bytes, and one property can hold at most %3. Nothing changed.")
        .arg(otherLabel.isEmpty() ? label : otherLabel, english.toString(bytes), english.toString(limit));
}

bool EntitySizeCheck::mayCarryTooBigValue(const EntityItemProperties& properties) {
    for (const LongValue& value : LONG_VALUES) {
        if (value.size(properties, false) > MEASURE_FROM_BYTES) {
            return true;
        }
    }
    return false;
}

QStringList EntitySizeCheck::checkedPropertyNames() {
    QStringList names;
    for (const LongValue& value : LONG_VALUES) {
        names << value.name;
    }
    return names;
}

EntitySizeProblem EntitySizeCheck::findProblem(EntityTypes::EntityType type, const EntityItemProperties& properties) {
    EntitySizeProblem problem;
    if (!mayCarryTooBigValue(properties)) {
        return problem;
    }
    // the entity server's own test (EntityTree::processEditPacketData), on the same kind of stand-in
    EntityItemPointer probe = EntityTypes::constructBaseEntityItem(type, QUuid::createUuid(), properties);
    if (!probe) {
        return problem;
    }
    const EntityPropertyFlags tooBig =
        probe->propertiesTooBigToStream(properties.getChangedProperties(), EntityItem::STREAM_HEADER_GROWTH, true);
    for (int flag = (int)tooBig.firstFlag(); flag <= (int)tooBig.lastFlag(); flag++) {
        const EntityPropertyList property = (EntityPropertyList)flag;
        if (!tooBig.getHasProperty(property)) {
            continue;
        }
        const LongValue* value = longValueFor(property, properties);
        problem.property = property;
        problem.name = value ? QString(value->name) : EntityItem::propertyName(property);
        problem.label = labelFor(problem.name);
        const int allowed = EntityItem::streamSectionRoom() - EntityItem::STREAM_HEADER_GROWTH;
        const int needed = probe->streamRoomNeededAlone(property, true);
        problem.bytes = value ? std::max(value->size(properties, true), 0) : 0;
        // what this value could be: its size less what it needs beyond the room; past what any value can take, the
        // room less the most an entity's header takes (as the server's streamCheckSize)
        const int HEADER_AT_MOST = 6 + 128;
        int limit = needed > 0 && problem.bytes > 0 ? problem.bytes - (needed - allowed) : allowed - HEADER_AT_MOST;
        problem.limit = std::max(std::min(limit, allowed) / 10 * 10, 0);
        break;
    }
    return problem;
}

void EntitySizeCheck::reportRefusal(const EntitySizeProblem& problem, const QUuid& entityID, bool isAdd) {
    const bool byWorldScript = ScriptCaller::isWorld();
    bool log = true;
    {
        std::lock_guard<std::mutex> lock(reportedMutex);
        if (reported.size() >= MAX_REPORTED || !reported.insert({ entityID, (int)problem.property }).second) {
            log = false;
        }
    }
    if (log) {
        qCWarning(entities).noquote() << (isAdd ? QString("add of a new entity") : "edit of entity " + entityID.toString())
            << "refused before it was applied: property" << problem.name << "is" << problem.bytes
            << "bytes, more than the" << problem.limit << "one property can hold";
    }
    emit EntitySizeNotices::instance()->refused(entityID, problem.name, problem.bytes, problem.limit,
                                                problem.sentence(), byWorldScript);
}

EntitySizeNotices* EntitySizeNotices::instance() {
    static EntitySizeNotices* notices = [] {
        auto created = new EntitySizeNotices();
        if (QCoreApplication::instance()) {
            created->moveToThread(QCoreApplication::instance()->thread());
        }
        return created;
    }();
    return notices;
}
