//
//  DomainIdTests.cpp
//
//  Created by servers on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "DomainIdTests.h"

#include <FingerprintUtils.h>

QTEST_MAIN(DomainIdTests)

void DomainIdTests::stablePerDomain() {
    QCOMPARE(FingerprintUtils::getDomainFingerprint("one.example"), FingerprintUtils::getDomainFingerprint(" ONE.example "));
    QCOMPARE(FingerprintUtils::getDomainHardwareAddress("one.example"), FingerprintUtils::getDomainHardwareAddress("one.example"));
}

void DomainIdTests::differsBetweenDomains() {
    QVERIFY(FingerprintUtils::getDomainFingerprint("one.example") != FingerprintUtils::getDomainFingerprint("two.example"));
    QVERIFY(FingerprintUtils::getDomainHardwareAddress("one.example") != FingerprintUtils::getDomainHardwareAddress("two.example"));
    QVERIFY(FingerprintUtils::getDomainFingerprint("") != QUuid());
}

void DomainIdTests::looksLikeAnAddress() {
    const QString address = FingerprintUtils::getDomainHardwareAddress("one.example");
    QCOMPARE(address.split(':').size(), 6);
    QCOMPARE(address.left(2).toInt(nullptr, 16) & 0x03, 0x02); // locally administered, unicast
    QCOMPARE(FingerprintUtils::getDomainFingerprint("one.example").version(), QUuid::Sha1);
}

void DomainIdTests::secretIsUnderAProtectedPrefix() {
    // ScriptSettingsInterface refuses "private/" to scripts from the network; the app's own test covers that refusal
    QVERIFY(FingerprintUtils::DOMAIN_ID_SECRET_KEY.startsWith("private/"));
}
