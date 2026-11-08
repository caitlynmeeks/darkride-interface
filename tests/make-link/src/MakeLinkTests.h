//
//  MakeLinkTests.h
//  tests/make-link/src
//
//  Make's link: the tool list and its checks (schemas, refused properties, web addresses, the world's text kept short
//  and labelled), the socket only this user can open, MCP over it (initialize, tools/list, tools/call, the gate, Stop,
//  time limits, one client at a time), the darkride-mcp helper carrying it from standard input and output, the channel
//  to Claude Code (the capability, its events, the protocol held at 2025-11-25) and the judgement of a connected
//  Claude Code's start line (MakePeer).
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
    void testChannelCapabilityAndEvents();
    void testPeerLineJudgement();
    void testPeerOfThisProcess();
    void testPlainTextForTheChannel();
    void testMakeFolderIsPrivate();
    void testLinkAccessIsThisUserOnly();
    void testLinkNameAlreadyTakenIsRefused();
    void testHelperCopyIsPrivateAndIdentical();
    void testToolListWaitsForTheOffer();
    void testClaudePathIsSystemFoldersOnly();
    void testRequestsFollowTheSend();
    void testUndoneRequestRefusesLateChanges();
    void testEveryToolSaysWhetherItChanges();
    void testMyFilesFolderName();
    void testMyFilesLinkIsTheFilesHosts();
    void testMyFilesUsedBy();
};

#endif // hifi_MakeLinkTests_h
