//
//  EntitySizeCheck.h
//  libraries/entities/src
//
//  Created by servers on 2026-11-07.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_EntitySizeCheck_h
#define hifi_EntitySizeCheck_h

#include <vector>

#include <QObject>
#include <QString>
#include <QUuid>

#include "EntityItemProperties.h"
#include "EntityTypes.h"

/// A value too big for a world, found before an add or edit is applied anywhere: the property, its size and the most
/// that property can hold in this entity. Empty when every value fits.
struct EntitySizeProblem {
    EntityPropertyList property { PROP_LAST_ITEM };
    QString name;   // as scripts know it ("userData")
    QString label;  // as people read it ("User Data")
    int bytes { 0 };
    int limit { 0 };  // rounded down to a multiple of 10

    bool isEmpty() const { return property == PROP_LAST_ITEM; }
    /// "Too big to save: User Data is 1,532 bytes, and one property can hold at most 1,330. Nothing changed."
    /// `label` replaces this->label (the Inspector's own names for its fields).
    QString sentence(const QString& label = QString()) const;
};

/// The client's check before an add or edit, the same one the entity server makes (EntityItem::propertiesTooBigToStream
/// on a stand-in of the entity's type, with EntityItem::STREAM_HEADER_GROWTH): a value the server would refuse, or the
/// client could not send, is refused before anything changes, so nothing is applied on the builder's own copy either.
namespace EntitySizeCheck {
    /// Whether any changed value of `properties` could be too big: compares each text, byte and vector value's size
    /// against a bound below the least any entity can hold. No allocation and no encoding: most edits stop here.
    bool mayCarryTooBigValue(const EntityItemProperties& properties);

    /// The properties mayCarryTooBigValue looks at: every one whose size the sender chooses (a test holds it to the
    /// property definitions, EntitySizeCheckTests).
    std::vector<EntityPropertyList> checkedProperties();

    /// The first value of `properties` too big for an entity of `type`; empty when every value fits. Measures only
    /// when mayCarryTooBigValue is true. For an Inspector before it commits; addEntity and editEntity call it too.
    EntitySizeProblem findProblem(EntityTypes::EntityType type, const EntityItemProperties& properties);

    /// Logs the refusal (the entity, the property and its size, never the value; once per entity and property) and
    /// tells EntitySizeNotices. `entityID` is null for an add.
    void reportRefusal(const EntitySizeProblem& problem, const QUuid& entityID, bool isAdd);
}

/// Refusals of EntitySizeCheck, for the app's own editor: C++ only, not part of any script API.
class EntitySizeNotices : public QObject {
    Q_OBJECT
public:
    static EntitySizeNotices* instance();

signals:
    /// An add (`entityID` null) or edit refused before it was applied. `byWorldScript` when a world's own script made it
    /// (ScriptCaller::isWorld): no message on screen for those, only the log's warning.
    void refused(const QUuid& entityID, const QString& property, int bytes, int limit, const QString& sentence,
                 bool byWorldScript);
};

#endif // hifi_EntitySizeCheck_h
