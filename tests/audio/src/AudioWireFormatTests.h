//
//  AudioWireFormatTests.h
//  tests/audio/src
//
//  The audio wire and file formats, byte for byte as a Qt 5.15 build writes them (the Qt 6 move's stage 0): the
//  InjectAudio header an injector sends and the audio mixer reads, and the WAV file a recording writes.
//
//  Created by social on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_AudioWireFormatTests_h
#define hifi_AudioWireFormatTests_h

#pragma once

#include <QtTest/QtTest>

class AudioWireFormatTests : public QObject {
    Q_OBJECT
private slots:
    void injectAudioHeader();
    void theMixerReadsAnInjector();
    void wavFile();
};

#endif // hifi_AudioWireFormatTests_h
