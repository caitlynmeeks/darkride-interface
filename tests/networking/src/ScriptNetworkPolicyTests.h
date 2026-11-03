//
//  ScriptNetworkPolicyTests.h
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_ScriptNetworkPolicyTests_h
#define hifi_ScriptNetworkPolicyTests_h

#include <QtTest/QtTest>

// The rule for a script's own web requests: which addresses count as private, which names as local, which script
// origins as local, and what a request from a script on the network is allowed, refused, or looked up.
class ScriptNetworkPolicyTests : public QObject {
    Q_OBJECT
private slots:
    void privateAddresses_data();
    void privateAddresses();
    void localHostNames_data();
    void localHostNames();
    void localOrigins_data();
    void localOrigins();
    void requestsFromTheNetwork_data();
    void requestsFromTheNetwork();
    void requestsFromLocalScripts();
    void resolvedAddresses();
    void lookupOfAPublicName();
    void blockingDecisions();
    void appScripts();
    void testRequestsGoToTheCheckedAddress();
    void testPinnedNameIsCheckedOnEveryRequest();
    void everyWebRequestRuled();
    void worldLeft();
};

#endif // hifi_ScriptNetworkPolicyTests_h
