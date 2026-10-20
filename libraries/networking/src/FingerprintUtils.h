//
//  FingerprintUtils.h
//  libraries/networking/src
//
//  Created by David Kelly on 2016-12-02.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_FingerprintUtils_h
#define hifi_FingerprintUtils_h

#include <QString>
#include <QUuid>

class FingerprintUtils {
public:
    static QUuid getMachineFingerprint();

    // What a domain is told: an ID and a network-card address of this computer's own for that domain, made from the
    // machine fingerprint and the domain's address. Each domain sees the same ones every time (so its bans by
    // computer keep working), and no two domains see the same ones (so they can't match a visitor between them).
    static QUuid getDomainFingerprint(const QString& domain);
    static QString getDomainHardwareAddress(const QString& domain);

private:
    static QString getMachineFingerprintString();
    static QUuid _machineFingerprint;
};

#endif // hifi_FingerprintUtils_h

