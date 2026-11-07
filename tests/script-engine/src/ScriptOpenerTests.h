//
//  ScriptOpenerTests.h
//  tests/script-engine/src
//
//  ScriptOpener's judgement of a script field's value, without a world or a network: code is written in, a file on
//  this computer is refused, a web or world-files address is opened under the world's rule, the rest can't be opened.
//
//  Created by scripting on 2026-11-07.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_ScriptOpenerTests_h
#define hifi_ScriptOpenerTests_h

#include <QtCore/QObject>
#include <QtTest/QtTest>

class ScriptOpenerTests : public QObject {
    Q_OBJECT
private slots:
    void testIsAddress();
    void testCodeIsWrittenIn();
    void testFileOnThisComputerRefused();
    void testWebAndWorldFilesOpen();
    void testPrivateAddressByTheWorld();
    void testOtherSchemesCantOpen();
    void testWasmNotText();
};

#endif // hifi_ScriptOpenerTests_h
