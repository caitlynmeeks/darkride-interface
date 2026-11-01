//
//  HostInterfaceTests.h
//  tests/wasm-scripting/src
//
//  The host interface driven from a module: every import crossed through the module's memory, the calls back
//  into the module (alloc, on_signal, on_callback), errors as records, failures reported.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_HostInterfaceTests_h
#define hifi_HostInterfaceTests_h

#include <QtTest/QtTest>

class HostInterfaceTests : public QObject {
    Q_OBJECT
private slots:
    void testVersionAndGlobals();
    void testDescribe();
    void testCall();
    void testCallErrors();
    void testEnumResults();
    void testProperties();
    void testSignals();
    void testCallbacksAndFailures();
    void testHandlesAndRelease();
    void testLog();
    void testCheckedDeliveries();
    void testMemberGate();
};

#endif // hifi_HostInterfaceTests_h
