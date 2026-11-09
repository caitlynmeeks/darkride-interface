//
//  EntitySizeCheckTests.h
//  tests/entity-content/src
//
//  Created by servers on 2026-11-07.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_EntitySizeCheckTests_h
#define hifi_EntitySizeCheckTests_h

#include <QtTest/QtTest>

// EntitySizeCheck: the client's check before an add or edit
class EntitySizeCheckTests : public QObject {
    Q_OBJECT
private slots:
    // every property whose size the sender chooses (text, bytes, lists) is in the check's list
    void everyVariableSizePropertyIsChecked();
    // an edit without a long value costs no measuring; one with a long value is measured
    void smallEditsAreNotMeasured();
    // a value over what an entity holds is found, with its size and a limit below it; one under it is not
    void tooBigValueIsFound();
};

#endif // hifi_EntitySizeCheckTests_h
