//
//  EntitySizeCheckTests.cpp
//  tests/entity-content/src
//
//  Created by servers on 2026-11-07.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "EntitySizeCheckTests.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <EntityItem.h>
#include <EntityItemProperties.h>
#include <EntitySizeCheck.h>

QTEST_GUILESS_MAIN(EntitySizeCheckTests)

namespace {

// libraries/entities/src, found from this file's own place in the source tree
QDir entitiesSource() {
    QDir dir = QFileInfo(QString(__FILE__)).absoluteDir();  // tests/entity-content/src
    dir.cdUp();
    dir.cdUp();
    dir.cdUp();
    return QDir(dir.filePath("libraries/entities/src"));
}

// "userData" from EntityItemProperties.h, "animation.url" from AnimationPropertyGroup.h: every property defined with
// a text, bytes or list type
QSet<QString> variableSizeNamesInDefinitions() {
    static const QRegularExpression DEFINITION(
        R"(DEFINE_PROPERTY\w*\(\s*PROP_\w+\s*,\s*\w+\s*,\s*(\w+)\s*,\s*(QString|QByteArray|QVector<[^>]*>)\s*,)");
    QSet<QString> names;
    const QDir source = entitiesSource();
    QStringList files { "EntityItemProperties.h" };
    files += source.entryList({ "*PropertyGroup.h" }, QDir::Files);
    for (const QString& fileName : files) {
        QFile file(source.filePath(fileName));
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        QString group;
        if (fileName != "EntityItemProperties.h") {
            group = fileName.left(fileName.indexOf("PropertyGroup.h"));
            group[0] = group[0].toLower();
        }
        const QString text = QString::fromUtf8(file.readAll());
        auto matches = DEFINITION.globalMatch(text);
        while (matches.hasNext()) {
            const QString name = matches.next().captured(1);
            names.insert(group.isEmpty() ? name : group + "." + name);
        }
    }
    return names;
}

}  // namespace

void EntitySizeCheckTests::everyVariableSizePropertyIsChecked() {
    const QSet<QString> defined = variableSizeNamesInDefinitions();
    QVERIFY2(defined.size() > 30, qPrintable(QString("found only %1 definitions in %2")
        .arg(defined.size()).arg(entitiesSource().path())));
    QSet<QString> checked;
    for (EntityPropertyList property : EntitySizeCheck::checkedProperties()) {
        checked.insert(EntityItem::propertyName(property));
    }
    // a property defined in a group no entity carries has no name in the properties' table: nothing to check there
    QSet<QString> named;
    EntityItemProperties all;
    all.markAllChanged();
    for (const QString& name : all.listChangedProperties()) {
        named.insert(name);
    }
    QStringList missing;
    for (const QString& name : defined) {
        if (named.contains(name) && !checked.contains(name)) {
            missing << name;
        }
    }
    missing.sort();
    QVERIFY2(missing.isEmpty(), qPrintable("not in EntitySizeCheck's list: " + missing.join(", ")));
}

void EntitySizeCheckTests::smallEditsAreNotMeasured() {
    EntityItemProperties move;
    move.setPosition(glm::vec3(1.0f, 2.0f, 3.0f));
    move.setVelocity(glm::vec3(0.0f, 0.0f, 5.0f));
    QVERIFY(!EntitySizeCheck::mayCarryTooBigValue(move));

    EntityItemProperties shortText;
    shortText.setUserData(QString(1000, 'x'));
    QVERIFY(!EntitySizeCheck::mayCarryTooBigValue(shortText));

    EntityItemProperties longText;
    longText.setUserData(QString(500, 'x'));  // 500 UTF-16 units can be 1,500 bytes: measured
    QVERIFY(EntitySizeCheck::mayCarryTooBigValue(longText));
}

void EntitySizeCheckTests::tooBigValueIsFound() {
    EntityItemProperties fits;
    fits.setType(EntityTypes::Box);
    fits.setUserData(QString(1200, 'x'));
    QVERIFY(EntitySizeCheck::findProblem(EntityTypes::Box, fits).isEmpty());

    EntityItemProperties tooBig;
    tooBig.setType(EntityTypes::Box);
    tooBig.setUserData(QString(2000, 'x'));
    const EntitySizeProblem problem = EntitySizeCheck::findProblem(EntityTypes::Box, tooBig);
    QVERIFY(!problem.isEmpty());
    QCOMPARE(problem.name, QString("userData"));
    QCOMPARE(problem.label, QString("User Data"));
    QCOMPARE(problem.bytes, 2000);
    QVERIFY2(problem.limit >= 1200 && problem.limit < 1407, qPrintable(QString::number(problem.limit)));
    QVERIFY(problem.sentence().startsWith("Too big to save: User Data is 2,000 bytes, and one property can hold at most"));

    EntityItemProperties huge;
    huge.setType(EntityTypes::Box);
    huge.setUserData(QString(70000, 'x'));
    const EntitySizeProblem hugeProblem = EntitySizeCheck::findProblem(EntityTypes::Box, huge);
    QVERIFY(!hugeProblem.isEmpty());
    QCOMPARE(hugeProblem.bytes, 70000);
    QVERIFY(hugeProblem.limit > 0 && hugeProblem.limit < 1407);
}
