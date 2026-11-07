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

#define TEXT_VALUE(PROP, CHANGED, VALUE) \
    { PROP, [](const EntityItemProperties& p, bool exact) { return textSize(p.CHANGED, p.VALUE, exact); } }
#define VECTOR_VALUE(PROP, CHANGED, VALUE) \
    { PROP, [](const EntityItemProperties& p, bool) { return vectorSize(p.CHANGED, p.VALUE); } }

struct LongValue {
    EntityPropertyList property;
    ValueSize size;
};

// Every value whose size the sender chooses: text, bytes and lists. Fixed-size values (numbers, vectors, colors) are
// never too big.
const LongValue LONG_VALUES[] = {
    TEXT_VALUE(PROP_NAME, nameChanged(), getName()),
    TEXT_VALUE(PROP_USER_DATA, userDataChanged(), getUserData()),
    TEXT_VALUE(PROP_PRIVATE_USER_DATA, privateUserDataChanged(), getPrivateUserData()),
    TEXT_VALUE(PROP_HREF, hrefChanged(), getHref()),
    TEXT_VALUE(PROP_DESCRIPTION, descriptionChanged(), getDescription()),
    TEXT_VALUE(PROP_COLLISION_SOUND_URL, collisionSoundURLChanged(), getCollisionSoundURL()),
    { PROP_ACTION_DATA, [](const EntityItemProperties& p, bool) {
        return p.actionDataChanged() ? (int)p.getActionData().size() : -1; } },
    TEXT_VALUE(PROP_SCRIPT, scriptChanged(), getScript()),
    TEXT_VALUE(PROP_SERVER_SCRIPTS, serverScriptsChanged(), getServerScripts()),
    TEXT_VALUE(PROP_ITEM_NAME, itemNameChanged(), getItemName()),
    TEXT_VALUE(PROP_ITEM_DESCRIPTION, itemDescriptionChanged(), getItemDescription()),
    TEXT_VALUE(PROP_ITEM_CATEGORIES, itemCategoriesChanged(), getItemCategories()),
    TEXT_VALUE(PROP_ITEM_ARTIST, itemArtistChanged(), getItemArtist()),
    TEXT_VALUE(PROP_ITEM_LICENSE, itemLicenseChanged(), getItemLicense()),
    TEXT_VALUE(PROP_MARKETPLACE_ID, marketplaceIDChanged(), getMarketplaceID()),
    TEXT_VALUE(PROP_CERTIFICATE_ID, certificateIDChanged(), getCertificateID()),
    TEXT_VALUE(PROP_CERTIFICATE_TYPE, certificateTypeChanged(), getCertificateType()),
    TEXT_VALUE(PROP_COMPOUND_SHAPE_URL, compoundShapeURLChanged(), getCompoundShapeURL()),
    TEXT_VALUE(PROP_TEXTURES, texturesChanged(), getTextures()),
    TEXT_VALUE(PROP_MODEL_URL, modelURLChanged(), getModelURL()),
    TEXT_VALUE(PROP_BLENDSHAPE_COEFFICIENTS, blendshapeCoefficientsChanged(), getBlendshapeCoefficients()),
    TEXT_VALUE(PROP_TEXT, textChanged(), getText()),
    TEXT_VALUE(PROP_FONT, fontChanged(), getFont()),
    TEXT_VALUE(PROP_FILTER_URL, filterURLChanged(), getFilterURL()),
    TEXT_VALUE(PROP_SOURCE_URL, sourceUrlChanged(), getSourceUrl()),
    TEXT_VALUE(PROP_SCRIPT_URL, scriptURLChanged(), getScriptURL()),
    TEXT_VALUE(PROP_TRACK_PROFILE, trackProfileChanged(), getTrackProfile()),
    TEXT_VALUE(PROP_SHAPE, shapeChanged(), getShape()),
    TEXT_VALUE(PROP_MATERIAL_URL, materialURLChanged(), getMaterialURL()),
    TEXT_VALUE(PROP_PARENT_MATERIAL_NAME, parentMaterialNameChanged(), getParentMaterialName()),
    TEXT_VALUE(PROP_MATERIAL_DATA, materialDataChanged(), getMaterialData()),
    TEXT_VALUE(PROP_IMAGE_URL, imageURLChanged(), getImageURL()),
    TEXT_VALUE(PROP_TERRAIN_URL, terrainURLChanged(), getTerrainURL()),
    TEXT_VALUE(PROP_TERRAIN_LAYERS, terrainLayersChanged(), getTerrainLayers()),
    TEXT_VALUE(PROP_ANIMATION_URL, getAnimation().urlChanged(), getAnimation().getURL()),
    TEXT_VALUE(PROP_SKYBOX_URL, getSkybox().urlChanged(), getSkybox().getURL()),
    TEXT_VALUE(PROP_AMBIENT_LIGHT_URL, getAmbientLight().ambientURLChanged(), getAmbientLight().getAmbientURL()),
    TEXT_VALUE(PROP_GRAB_EQUIPPABLE_INDICATOR_URL, getGrab().equippableIndicatorURLChanged(),
               getGrab().getEquippableIndicatorURL()),
    TEXT_VALUE(PROP_VEHICLE_TYPE, getVehicle().typeChanged(), getVehicle().getType()),
    TEXT_VALUE(PROP_VEHICLE_WHEELS, getVehicle().wheelsChanged(), getVehicle().getWheels()),
    TEXT_VALUE(PROP_VEHICLE_SEATS, getVehicle().seatsChanged(), getVehicle().getSeats()),
    TEXT_VALUE(PROP_VEHICLE_GEAR_RATIOS, getVehicle().gearRatiosChanged(), getVehicle().getGearRatios()),
    TEXT_VALUE(PROP_VEHICLE_DRIVE_TYPE, getVehicle().driveTypeChanged(), getVehicle().getDriveType()),
    TEXT_VALUE(PROP_VEHICLE_WHEEL, getVehicle().wheelChanged(), getVehicle().getWheel()),
    TEXT_VALUE(PROP_VEHICLE_AXLE, getVehicle().axleChanged(), getVehicle().getAxle()),
    VECTOR_VALUE(PROP_JOINT_ROTATIONS_SET, jointRotationsSetChanged(), getJointRotationsSet()),
    VECTOR_VALUE(PROP_JOINT_ROTATIONS, jointRotationsChanged(), getJointRotations()),
    VECTOR_VALUE(PROP_JOINT_TRANSLATIONS_SET, jointTranslationsSetChanged(), getJointTranslationsSet()),
    VECTOR_VALUE(PROP_JOINT_TRANSLATIONS, jointTranslationsChanged(), getJointTranslations()),
    VECTOR_VALUE(PROP_TRACK_POINTS, trackPointsChanged(), getTrackPoints()),
    VECTOR_VALUE(PROP_TRACK_BANK, trackBankChanged(), getTrackBank()),
    VECTOR_VALUE(PROP_TRACK_DWELL, trackDwellChanged(), getTrackDwell()),
    VECTOR_VALUE(PROP_LINE_POINTS, linePointsChanged(), getLinePoints()),
    VECTOR_VALUE(PROP_STROKE_WIDTHS, strokeWidthsChanged(), getStrokeWidths()),
    VECTOR_VALUE(PROP_STROKE_NORMALS, normalsChanged(), getNormals()),
    VECTOR_VALUE(PROP_STROKE_COLORS, strokeColorsChanged(), getStrokeColors()),
};

#undef TEXT_VALUE
#undef VECTOR_VALUE

int exactSize(EntityPropertyList property, const EntityItemProperties& properties) {
    for (const LongValue& value : LONG_VALUES) {
        if (value.property == property) {
            return value.size(properties, true);
        }
    }
    return -1;
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

std::vector<EntityPropertyList> EntitySizeCheck::checkedProperties() {
    std::vector<EntityPropertyList> properties;
    for (const LongValue& value : LONG_VALUES) {
        properties.push_back(value.property);
    }
    return properties;
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
        problem.property = property;
        problem.name = EntityItem::propertyName(property);
        problem.label = labelFor(problem.name);
        const int allowed = EntityItem::streamSectionRoom() - EntityItem::STREAM_HEADER_GROWTH;
        const int needed = probe->streamRoomNeededAlone(property, true);
        problem.bytes = std::max(exactSize(property, properties), 0);
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
