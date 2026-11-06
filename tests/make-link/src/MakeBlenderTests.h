//
//  MakeBlenderTests.h
//  tests/make-link/src
//
//  Make's Blender tool: the checks of what comes back (the .glb's structure, the report, the views), and, where Blender
//  is installed, real makes in the sandbox: a model and its views, a script's error coming back, a runaway stopped at
//  its limit, one make at a time, and the box holding against a script that tries the network, the home folder, writing
//  outside its folder, starting programs and reading a key from its environment.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_MakeBlenderTests_h
#define hifi_MakeBlenderTests_h

#include <QtTest/QtTest>

class MakeBlenderTests : public QObject {
    Q_OBJECT
private slots:
    void testGlbCheck();
    void testCleanReport();
    void testDrawViews();
    void testFindBlender();
    void testMakeAModel();
    void testScriptErrorComesBack();
    void testTheBoxHolds();
    void testRunawayIsStopped();
    void testOneMakeAtATime();
    void testTextureNamesChecked();
};

#endif // hifi_MakeBlenderTests_h
