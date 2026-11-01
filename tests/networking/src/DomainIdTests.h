//
//  DomainIdTests.h
//
//  Created by servers on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_DomainIdTests_h
#define hifi_DomainIdTests_h

#include <QtTest/QtTest>

// What a domain is told about this computer: stable there, different elsewhere, and not the machine's own ID.
class DomainIdTests : public QObject {
    Q_OBJECT
private slots:
    void stablePerDomain();
    void differsBetweenDomains();
    void looksLikeAnAddress();
    void secretIsUnderAProtectedPrefix();
};

#endif // hifi_DomainIdTests_h
