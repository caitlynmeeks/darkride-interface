//
//  ResourceSizeLimitTests.h
//
//  Created by servers on 2026-11-02.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_ResourceSizeLimitTests_h
#define hifi_ResourceSizeLimitTests_h

#include <QtTest/QtTest>

// The size limit a web download gets by the kind of file its address names (ResourceSizeLimit.h).
class ResourceSizeLimitTests : public QObject {
    Q_OBJECT

private slots:
    void kindByEnding_data();
    void kindByEnding();
    void limitsLeaveRoomAndStayUnderTheUnpackCeiling();
};

#endif // hifi_ResourceSizeLimitTests_h
