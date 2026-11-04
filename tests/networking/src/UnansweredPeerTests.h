//
//  UnansweredPeerTests.h
//  tests/networking/src
//
//  An address that isn't one of a domain's nodes, and a peer that never answers a handshake (security #60); a server
//  answering a node before its ping is answered, and the deadline of an unanswered upload or mapping operation (qa's
//  putAsset bug).
//
//  Created by servers on 2026-11-03.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_UnansweredPeerTests_h
#define hifi_UnansweredPeerTests_h

#pragma once

#include <QtTest/QtTest>

class UnansweredPeerTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void onlyANodesAddressIsANode();
    void anUnansweredHandshakeIsGivenUp();
    void aGiveUpWithPacketsStillComing();
    void aLateAnswerAfterTheGiveUp();
    void aVerifiedPacketsAddressIsUsedBeforeAnyPing();
    void anUnansweredUploadOrMappingHasADeadline();
    void anUnansweredUploadFailsAtItsDeadline();
};

#endif // hifi_UnansweredPeerTests_h
