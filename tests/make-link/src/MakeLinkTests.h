//
//  MakeLinkTests.h
//  tests/make-link/src
//
//  Make's link: the tool list and its checks (schemas, refused properties, web addresses, the world's text kept short
//  and labelled), the socket only this user can open, MCP over it (initialize, tools/list, tools/call, the gate, Stop,
//  time limits, one client at a time), and the darkride-mcp helper carrying it from standard input and output.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_MakeLinkTests_h
#define hifi_MakeLinkTests_h

#include <QtTest/QtTest>

class MakeLinkTests : public QObject {
    Q_OBJECT
private slots:
    void testToolListLoads();
    void testMcpSchemasStandAlone();
    void testCheckRefusesWhatDoesntFit();
    void testCheckRefusesPropertiesThatSkipChecks();
    void testWebAddressesAskFirst();
    void testWorldContentKeptShortAndLabelled();
    void testSocketIsPrivate();
    void testInitializeAndList();
    void testCallRunsCheckedInputsOnly();
    void testGateCanRefuse();
    void testStopAnswersRunningCalls();
    void testTimeLimit();
    void testSecondClientRefused();
    void testHelperCarriesMcp();
    void testHelperRefusesAnOpenSocket();
    void testKeyHiddenInClaudeCodeErrors();
    void testClaudeStartedOncePerRun();
};

#endif // hifi_MakeLinkTests_h
