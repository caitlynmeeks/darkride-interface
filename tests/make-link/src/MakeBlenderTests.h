//
//  MakeBlenderTests.h
//  tests/make-link/src
//
//  Make's Blender tool: the checks of what comes back (the .glb's structure, the report, the views), and, where Blender
//  is installed, real makes in the sandbox: a model and its views, a script's error coming back, a runaway stopped at
//  its limit, one make at a time, and the box holding against a script that tries the network, the home folder, writing
//  outside its folder, starting programs and reading a key from its environment. And Blender's parent, the watcher:
//  what ends its child (a stop line, the app gone, its cap, a signal), what it passes on and what it refuses.
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
    void testKeychainUnreachableInTheBox();
    void testRunawayIsStopped();
    void testCpuCapInTheBox();
    void testBlenderEndsWithTheApp();
    void testWatcherPassesTheExitOn();
    void testWatcherEndsItsChild();
    void testWatcherRefuses();
    void testWatcherIsBlendersParent();
    void testOneMakeAtATime();
    void testTextureNamesChecked();
    void testVersionAndWords();
    void testSelfTestResult();
    void testSelfTestAnswers();
    void testBlenderIdentityRules();
    void testBlenderIsBlenderLive();
    void testLinuxParsesAndCaps();
    void testLinuxWatcherRefusesOtherLines();
};

#endif // hifi_MakeBlenderTests_h
