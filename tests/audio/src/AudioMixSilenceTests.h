//
//  AudioMixSilenceTests.h
//  tests/audio/src
//
//  Created by social on 2026-11-07.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_AudioMixSilenceTests_h
#define hifi_AudioMixSilenceTests_h

#include <QtTest/QtTest>

class AudioMixSilenceTests : public QObject {
    Q_OBJECT
private slots:
    void scale_data();
    void scale();
    void limiterScale();
    void silentRender();
    void quietVoice_data();
    void quietVoice();
};

#endif // hifi_AudioMixSilenceTests_h
