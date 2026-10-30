//
//  MixedProcessedAudioStreamTests.h
//
//  Created by social on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_MixedProcessedAudioStreamTests_h
#define hifi_MixedProcessedAudioStreamTests_h

#include <QtTest/QtTest>

class MixedProcessedAudioStreamTests : public QObject {
    Q_OBJECT
private slots:
    void receivedFrames_data();
    void receivedFrames();
    void lostFrames_data();
    void lostFrames();
};

#endif // hifi_MixedProcessedAudioStreamTests_h
