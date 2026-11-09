//
//  AvatarMixerChecksTests.h
//  tests/avatars/src
//
//  The avatar mixer's checks on a skeleton and an identity before the shared avatar code reads them (security #52).
//
//  Created by servers on 2026-11-02.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_AvatarMixerChecksTests_h
#define hifi_AvatarMixerChecksTests_h

#pragma once

#include <QtTest/QtTest>

class AvatarMixerChecksTests : public QObject {
    Q_OBJECT
private slots:
    void anAppsSkeletonPasses();
    void aSkeletonWithNoJointsPasses();
    void aSkeletonShorterThanItsHeaderIsDropped();
    void jointsPastTheEndAreDropped();
    void aNameTablePastTheEndIsDropped();
    void aNameOutsideTheTableIsDropped();
    void aNameEndingAtTheTablesEndPasses();
    void namesAreCountedInCharacters();
    void aByteOrderMarkCountsForNothing();
    void bytesAfterTheTablePass();

    void anAppsIdentityPasses();
    void anIdentityWithNullStringsPasses();
    void anIdentityWithAttachmentsPasses();
    void aHugeAttachmentCountIsDropped();
    void moreAttachmentsThanTheDataHoldsIsDropped();
    void aLongAddressIsDropped();
    void aLongNameIsDropped();
    void aNameAtTheLimitPasses();
    void aShortIdentityIsDropped();

    void newIdsWithinTheLimitsAreTaken();
    void tooManyLiveIdsAreRefused();
    void aDeleteForAnUnlistedIdIsRefused();
    void theCapSaysToReconnect();
    void tooManyListedIdsAreRefused();
    void aBurstOfNewIdsIsTakenThenTheRate();
    void theRateComesBackNoHigherThanTheBurst();
    void aClockGoingBackGivesNothing();
};

#endif // hifi_AvatarMixerChecksTests_h
