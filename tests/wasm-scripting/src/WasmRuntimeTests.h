//
//  WasmRuntimeTests.h
//  tests/wasm-scripting/src
//
//  The runtime wrapper: compiling, calling, host functions, traps, the time limit, the memory limit, WASI.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_WasmRuntimeTests_h
#define hifi_WasmRuntimeTests_h

#include <QtTest/QtTest>

class WasmRuntimeTests : public QObject {
    Q_OBJECT
private slots:
    void testCompileAndCall();
    void testCompileErrorIsReported();
    void testTrapIsAnError();
    void testHostFunction();
    void testHostFunctionError();
    void testInfiniteLoopIsInterrupted();
    void testRequestStop();
    void testCooperativeStop();
    void testResumeAfterStop();
    void testStopAfterLetsCallFinish();
    void testStopAfterEndsRunawayAtDeadline();
    void testStopAfterEndsAtOnceWhenWorldLeft();
    void testStopAfterRefusesNewCall();
    void testCooperativeTimeLimit();
    void testCallWarningGoesOn();
    void testTimeLimitCountsModuleTimeOnly();
    void testMemoryLimit();
    void testMemoryReadWrite();
    void testWasiImports();
    void testImportsAndExports();
    void testHostFunctionMayCallBackIn();
};

#endif // hifi_WasmRuntimeTests_h
