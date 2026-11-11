//
//  SignInRefusedTests.h
//  tests/networking/src
//
//  AccountManager's refused sign-in (setEndsRefusedSignIn) against a stand-in metaverse on 127.0.0.1: a refused access
//  token and a refused refresh end the sign-in; an unverified email, a refresh that works, a server error or no answer
//  never do; a reply for an older token or another host says nothing; off, nothing changes.
//
//  Created by servers on 2026-11-11.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_SignInRefusedTests_h
#define hifi_SignInRefusedTests_h

#pragma once

#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

class SignInRefusedTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void unverifiedStaysSignedIn();
    void refusedTokenAndRefreshEnds();
    void refreshThenUnverifiedStaysSignedIn();
    void refreshThenRefusedAgainEnds();
    void serverErrorOrNoAnswerStaysSignedIn();
    void noRefreshTokenEnds();
    void offChangesNothing();
    void olderTokensReplyIsIgnored();
    void otherHostsReplyIsIgnored();
    void reportedRefusalRefreshesOnce();

private:
    QTemporaryDir _appData;
};

#endif // hifi_SignInRefusedTests_h
