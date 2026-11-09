//
//  EditFilterTests.cpp
//  tests/edit-filters/src
//
//  The entity server's edit filters on the WebAssembly engine: each filter script over the same edits gives the outcome
//  (accepted, rejected, the properties out) it gave on QtScript, the engine before it.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "EditFilterTests.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>

#include <AddressManager.h>
#include <EntityEditFilters.h>
#include <EntityTree.h>
#include <NodeList.h>
#include <EntityTypes.h>
#include <PlainScriptValue.h>
#include <SharedUtil.h>
#include <WasmEditFilterEngine.h>
#include <WasmScriptEngine.h>

QTEST_GUILESS_MAIN(EditFilterTests)

namespace {

const EntityItemID GLOBAL_FILTER;  // the null id: the domain's filter

const char* ACCEPT_ALL = "function filter(properties, type) { return true; }";
const char* REJECT_ALL = "function filter(properties, type) { return false; }";
const char* ADDS_ONLY = R"(
function filter(properties, type) {
    if (properties.name === "forbidden") { return false; }
    properties.name = (properties.name || "") + "-filtered";
    properties.position = { x: 1, y: 2, z: 3 };
    return properties;
}
filter.wantsToFilterEdit = false;
filter.wantsToFilterDelete = false;
// a trailing comment without a newline)";
const char* REPORTS_ARGUMENTS = R"(
function filter(properties, type, original, zone) {
    var seen = {
        count: arguments.length,
        type: type,
        isAdd: type === Entities.ADD_FILTER_TYPE,
        isEdit: type === Entities.EDIT_FILTER_TYPE,
        keys: Object.keys(properties).sort(),
        original: original ? [original.name, original.position.x] : null,
        zone: zone ? [zone.name, zone.boundingBox ? zone.boundingBox.dimensions.x : null, zone.dimensions ? "dimensions too" : "no dimensions"] : null
    };
    return { userData: JSON.stringify(seen) };
}
filter.wantsOriginalProperties = ["name", "position"];
filter.wantsZoneProperties = ["boundingBox", "name"];
filter.wantsToFilterDelete = true;
)";
const char* ALL_ORIGINALS = R"(
function filter(properties, type, original) {
    return { userData: original ? String(Object.keys(original).length > 20) + " " + original.name : "none" };
}
filter.wantsOriginalProperties = true;
filter.wantsZoneProperties = "boundingBox";
)";
const char* THROWS = "function filter(properties, type) { throw new Error('no edits today'); }";
const char* NO_FUNCTION = "var filter = 3;";
const char* SYNTAX_ERROR = "function filter(properties, type { return true; }";
// what a filter sees of Script: the engine's own members, without QObject's
const char* REPORTS_SCRIPT = R"(
function filter(properties, type) {
    return { userData: typeof Script.deleteLater + " " + typeof Script.destroyed };
}
)";

struct Outcome {
    QString filter;
    QString edit;
    bool loaded { false };
    bool accepted { false };
    bool wasChanged { false };
    QVariantMap out;
    QString line() const {
        return filter + " / " + edit + ": " + (loaded ? "loaded" : "not loaded") + ", " + (accepted ? "accepted" : "rejected") + ", " +
               (wasChanged ? "changed" : "unchanged") + ", " + QString::fromUtf8(QJsonDocument::fromVariant(out).toJson(QJsonDocument::Compact));
    }
};

struct World {
    EntityTreePointer tree;
    EntityItemID zoneID { QUuid("{aaaaaaaa-0000-0000-0000-00000000000a}") };
    EntityItemID boxID { QUuid("{bbbbbbbb-0000-0000-0000-00000000000b}") };

    World() {
        tree = EntityTreePointer(new EntityTree(true));
        tree->createRootElement();
        tree->setIsServerlessMode(true);  // a local world: no rez rights asked of a node list
        EntityItemProperties zone;
        zone.setType(EntityTypes::Zone);
        zone.setName("the zone");
        zone.setPosition(glm::vec3(100.0f));  // well inside the octree's domain, which starts at the origin
        zone.setDimensions(glm::vec3(10.0f));
        zone.setCreated(usecTimestampNow());
        tree->addEntity(zoneID, zone);
        EntityItemProperties box;
        box.setType(EntityTypes::Box);
        box.setName("box");
        box.setPosition(glm::vec3(101.0f, 100.0f, 100.0f));
        box.setDimensions(glm::vec3(0.5f));
        box.setCreated(usecTimestampNow());
        tree->addEntity(boxID, box);
    }
};

struct Edit {
    const char* name;
    EntityTree::FilterType type;
    bool existing;  // the box, else a new entity
    QString newName;
};

const Edit EDITS[] = {
    { "add", EntityTree::FilterType::Add, false, "cube" },
    { "add forbidden", EntityTree::FilterType::Add, false, "forbidden" },
    { "edit", EntityTree::FilterType::Edit, true, "renamed" },
    { "physics", EntityTree::FilterType::Physics, true, QString() },
    { "delete", EntityTree::FilterType::Delete, true, QString() },
};

// one filter over the edits, on the engine the factory makes
QList<Outcome> runFilter(const QString& filterName, const char* source, const EntityItemID& filterID, World& world) {
    QList<Outcome> outcomes;
    EntityEditFilters filters(world.tree);
    bool loaded = false;
    QObject::connect(&filters, &EntityEditFilters::filterAdded, [&](EntityItemID, bool success) { loaded = success; });
    filters.addFilterSource(filterID, "https://example.invalid/" + filterName + ".js", QString::fromUtf8(source));
    for (const Edit& edit : EDITS) {
        EntityItemProperties in;
        if (!edit.newName.isEmpty()) {
            in.setName(edit.newName);
        }
        if (!edit.existing) {
            in.setType(EntityTypes::Box);
            in.setPosition(glm::vec3(100.5f, 100.5f, 100.5f));
            in.setDimensions(glm::vec3(0.25f));
            in.setCreated(usecTimestampNow());
        }
        if (edit.type == EntityTree::FilterType::Physics) {
            in.setVelocity(glm::vec3(0.0f, 1.0f, 0.0f));
        }
        EntityItemProperties out;
        bool wasChanged = false;
        glm::vec3 position = edit.existing ? glm::vec3(101.0f, 100.0f, 100.0f) : in.getPosition();
        EntityItemID itemID = edit.existing ? world.boxID : EntityItemID();
        EntityItemPointer existing = edit.existing ? world.tree->findEntityByEntityItemID(world.boxID) : nullptr;
        Outcome outcome;
        outcome.filter = filterName;
        outcome.edit = edit.name;
        outcome.loaded = loaded;
        outcome.accepted = filters.filter(position, in, out, wasChanged, edit.type, itemID, existing);
        outcome.wasChanged = wasChanged;
        PlainScriptEngine plain;
        outcome.out = out.copyToScriptValue(&plain, true, true).toVariant().toMap();
        for (const char* clock : { "age", "ageAsText", "lastEdited", "created" }) {
            outcome.out.remove(clock);
        }
        outcomes << outcome;
    }
    return outcomes;
}

struct Filter {
    const char* name;
    const char* source;
    bool global;  // the domain's filter, else the zone's
};

const Filter FILTERS[] = {
    { "accept-all", ACCEPT_ALL, true },
    { "reject-all", REJECT_ALL, true },
    { "adds-only", ADDS_ONLY, true },
    { "reports-arguments", REPORTS_ARGUMENTS, false },
    { "reports-arguments-globally", REPORTS_ARGUMENTS, true },
    { "all-originals", ALL_ORIGINALS, false },
    { "throws", THROWS, true },
    { "no-function", NO_FUNCTION, true },
    { "syntax-error", SYNTAX_ERROR, true },
};

QList<Outcome> runAll(World& world) {
    QList<Outcome> outcomes;
    for (const Filter& filter : FILTERS) {
        outcomes << runFilter(filter.name, filter.source, filter.global ? GLOBAL_FILTER : world.zoneID, world);
    }
    return outcomes;
}

const Outcome& find(const QList<Outcome>& outcomes, const QString& filter, const QString& edit) {
    for (const Outcome& outcome : outcomes) {
        if (outcome.filter == filter && outcome.edit == edit) {
            return outcome;
        }
    }
    static Outcome none;
    return none;
}

QString guestModulePath() {
    QString fromEnvironment = qEnvironmentVariable("DARKRIDE_JS_WASM");
    if (!fromEnvironment.isEmpty()) {
        return fromEnvironment;
    }
    return QFileInfo(QString(__FILE__)).dir().absoluteFilePath("../../../tools/wasm-js/build/darkride-js.wasm");
}

void useWebAssembly(std::chrono::milliseconds callTimeout) {
    EntityEditFilters::setEngineFactory([callTimeout](const EntityItemID& id) -> EntityEditFilterEnginePointer {
        return std::make_unique<WasmEditFilterEngine>(id, callTimeout);
    });
}

}  // namespace

void EditFilterTests::initTestCase() {
    // the tree adds an entity only with a node list about (the animation and networking tests set one up the same way)
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent, INVALID_PORT);
    if (qEnvironmentVariable("DARKRIDE_JS_WASM").isEmpty()) {
        qputenv("DARKRIDE_JS_WASM", guestModulePath().toUtf8());
    }
}

void EditFilterTests::testFilters() {
    World world;
    QVERIFY(world.tree->findEntityByEntityItemID(world.zoneID));
    QVERIFY(world.tree->findEntityByEntityItemID(world.boxID));
    if (!QFileInfo::exists(guestModulePath())) {
        QSKIP("darkride-js.wasm is not built (tools/wasm-js/build.sh)");
    }
    useWebAssembly(std::chrono::milliseconds(10000));
    const QList<Outcome> onWebAssembly = runAll(world);

    // what the filters do, as they always did on QtScript
    QVERIFY(find(onWebAssembly, "accept-all", "add").accepted);
    QVERIFY(!find(onWebAssembly, "accept-all", "add").wasChanged);
    QVERIFY(!find(onWebAssembly, "reject-all", "add").accepted);
    QVERIFY(find(onWebAssembly, "adds-only", "add").accepted);
    QVERIFY(find(onWebAssembly, "adds-only", "add").wasChanged);
    QCOMPARE(find(onWebAssembly, "adds-only", "add").out.value("name").toString(), QString("cube-filtered"));
    QCOMPARE(find(onWebAssembly, "adds-only", "add").out.value("position").toMap().value("y").toDouble(), 2.0);
    QVERIFY(!find(onWebAssembly, "adds-only", "add forbidden").accepted);
    QVERIFY(find(onWebAssembly, "adds-only", "edit").accepted);
    QVERIFY(!find(onWebAssembly, "adds-only", "edit").wasChanged);
    const QVariantMap seen = QJsonDocument::fromJson(find(onWebAssembly, "reports-arguments", "edit").out.value("userData").toString().toUtf8()).toVariant().toMap();
    QCOMPARE(seen.value("count").toInt(), 4);
    QVERIFY(seen.value("isEdit").toBool());
    QCOMPARE(seen.value("original").toList().value(0).toString(), QString("box"));
    QCOMPARE(seen.value("zone").toList().value(0).toString(), QString("the zone"));
    QCOMPARE(seen.value("zone").toList().value(1).toDouble(), 10.0);
    // the edit's own properties, with the pseudo-properties every script object carries, and not the rest
    QVERIFY(seen.value("keys").toStringList().contains("name"));
    QVERIFY(!seen.value("keys").toStringList().contains("position"));
    const QVariantMap seenAdd = QJsonDocument::fromJson(find(onWebAssembly, "reports-arguments", "add").out.value("userData").toString().toUtf8()).toVariant().toMap();
    QCOMPARE(seenAdd.value("count").toInt(), 4);  // no original: padded with undefined before the zone
    QVERIFY(seenAdd.value("isAdd").toBool());
    QVERIFY(seenAdd.value("original").isNull());
    const QVariantMap seenGlobal = QJsonDocument::fromJson(find(onWebAssembly, "reports-arguments-globally", "edit").out.value("userData").toString().toUtf8()).toVariant().toMap();
    QCOMPARE(seenGlobal.value("count").toInt(), 3);  // the domain's filter has no zone
    QVERIFY(!find(onWebAssembly, "throws", "add").accepted);
    QVERIFY(!find(onWebAssembly, "no-function", "add").loaded);
    QVERIFY(!find(onWebAssembly, "no-function", "add").accepted);
    QVERIFY(!find(onWebAssembly, "syntax-error", "add").loaded);
    QVERIFY(!find(onWebAssembly, "syntax-error", "add").accepted);
}

void EditFilterTests::testFilterScriptObject() {
    if (!QFileInfo::exists(guestModulePath())) {
        QSKIP("darkride-js.wasm is not built (tools/wasm-js/build.sh)");
    }
    useWebAssembly(std::chrono::milliseconds(10000));
    World world;
    const auto reports = runFilter("reports-script", REPORTS_SCRIPT, world.zoneID, world);
    QVERIFY(find(reports, "reports-script", "edit").loaded);
    // a filter can't delete the Script object (no deleteLater). QObject's own destroyed signal shows on WebAssembly,
    // where QtScript hid it ("undefined undefined" there): listening to it does nothing a filter could use
    QCOMPARE(find(reports, "reports-script", "edit").out.value("userData").toString().section(' ', 0, 0), QString("undefined"));
}

void EditFilterTests::testRunawayFilterOnWebAssembly() {
    if (!QFileInfo::exists(guestModulePath())) {
        QSKIP("darkride-js.wasm is not built (tools/wasm-js/build.sh)");
    }
    // a filter that never returns: the call's time limit traps, the edit is refused, and so is every later one
    World world;
    useWebAssembly(std::chrono::milliseconds(300));
    const QList<Outcome> outcomes = runFilter("runaway", "function filter(properties, type) { while (true) {} }", GLOBAL_FILTER, world);
    QVERIFY(find(outcomes, "runaway", "add").loaded);
    for (const Outcome& outcome : outcomes) {
        // a delete is not filtered unless the filter asks (wantsToFilterDelete), so it passes without a call
        QVERIFY2(outcome.accepted == (outcome.edit == "delete"), qPrintable(outcome.line()));
    }
}
