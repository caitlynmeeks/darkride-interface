//
//  FileRequestPathTests.h
//
//  Created by servers on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_FileRequestPathTests_h
#define hifi_FileRequestPathTests_h

#include <QtTest/QtTest>

// Which file URLs name another machine, in the forms a world could write them.
class FileRequestPathTests : public QObject {
    Q_OBJECT
private slots:
    void networkPaths_data();
    void networkPaths();
};

#endif // hifi_FileRequestPathTests_h
