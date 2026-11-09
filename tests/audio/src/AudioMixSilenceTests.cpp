//
//  AudioMixSilenceTests.cpp
//  tests/audio/src
//
//  Created by social on 2026-11-07.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "AudioMixSilenceTests.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <AudioConstants.h>
#include <AudioHRTF.h>
#include <AudioLimiter.h>
#include <AudioMixSilence.h>

QTEST_MAIN(AudioMixSilenceTests)

namespace {

// the audio mixer's sizes: one 10 ms network frame per listener, rendered and mixed as in AudioMixerWorker::prepareMix
const int FRAMES = AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL;  // 240 at 24 kHz
const int SAMPLES = AudioConstants::NETWORK_FRAME_SAMPLES_STEREO;      // 480 floats in the mix
const int HRTF_INDEX = 1;                                              // the mixer's HRTF_DATASET_INDEX
const float STEP = 1.0f / 32768.0f;                                    // one 16-bit step in the mix's floats

// the check the mixer used before: any float not exactly 0 is sound
bool exactlyNonZero(const float* mix) {
    for (int i = 0; i < SAMPLES; ++i) {
        if (mix[i] != 0.0f) {
            return true;
        }
    }
    return false;
}

float peakOf(const float* mix) {
    float peak = 0.0f;
    for (int i = 0; i < SAMPLES; ++i) {
        peak = std::max(peak, std::fabs(mix[i]));
    }
    return peak;
}

}

// the mix's floats against 16-bit steps: the stereo path (no filter) at gain 1 turns a sample of v steps into v / 32768
void AudioMixSilenceTests::scale_data() {
    QTest::addColumn<int>("value");
    QTest::addColumn<bool>("sound");
    QTest::newRow("silence") << 0 << false;
    QTest::newRow("one step") << 1 << true;
    QTest::newRow("minus one step") << -1 << true;
    QTest::newRow("full scale") << 32767 << true;
}

void AudioMixSilenceTests::scale() {
    QFETCH(int, value);
    QFETCH(bool, sound);

    int16_t input[SAMPLES];
    for (int i = 0; i < SAMPLES; ++i) {
        input[i] = (int16_t)value;
    }
    float mix[SAMPLES] = {};
    AudioHRTF hrtf;
    hrtf.mixStereo(input, mix, 1.0f, FRAMES);

    QCOMPARE(mix[0], value * STEP);
    QCOMPARE(AudioMixSilence::hasAudio(mix, SAMPLES), sound);

    // the threshold, half a step, in the same floats
    for (int i = 0; i < SAMPLES; ++i) {
        mix[i] = 0.49f * STEP;
    }
    QVERIFY(!AudioMixSilence::hasAudio(mix, SAMPLES));
    mix[SAMPLES - 1] = 0.51f * STEP;
    QVERIFY(AudioMixSilence::hasAudio(mix, SAMPLES));
}

// the limiter (the mixer's AudioLimiter, 24 kHz stereo) turns those floats into the 16-bit output: 100 steps in comes out
// as about 100 (its ceiling is -0.3 dB), and a mix under half a step leaves nothing but its dither of at most one step
void AudioMixSilenceTests::limiterScale() {
    AudioLimiter limiter(AudioConstants::SAMPLE_RATE, AudioConstants::STEREO);
    float mix[SAMPLES];
    int16_t output[SAMPLES];

    for (int frame = 0; frame < 20; ++frame) {  // past the limiter's look-ahead
        for (int i = 0; i < SAMPLES; ++i) {
            mix[i] = 100.0f * STEP;
        }
        limiter.render(mix, output, FRAMES);
    }
    for (int i = 0; i < SAMPLES; ++i) {
        QVERIFY2(output[i] >= 94 && output[i] <= 100, qPrintable(QString("sample %1 is %2").arg(i).arg(output[i])));
    }

    for (int frame = 0; frame < 20; ++frame) {
        for (int i = 0; i < SAMPLES; ++i) {
            mix[i] = 0.49f * STEP;
        }
        limiter.render(mix, output, FRAMES);
    }
    for (int i = 0; i < SAMPLES; ++i) {
        QVERIFY2(std::abs(output[i]) <= 1, qPrintable(QString("sample %1 is %2").arg(i).arg(output[i])));
    }
    QVERIFY(!AudioMixSilence::hasAudio(mix, SAMPLES));
}

// a silent stream rendered once through the HRTF (a new stream's first frame, the mixer's forced silent block, and the
// move to skipped at gain 0) is silence. On every chip but Intel and AMD the HRTF's portable filters add 1e-20 against
// denormals, so that render isn't exactly 0: the mixer's old check called it sound and sent a near-silent packet + flush
void AudioMixSilenceTests::silentRender() {
    for (float gain : { 0.5f, 0.0f }) {
        int16_t silent[FRAMES] = {};
        float mix[SAMPLES] = {};
        AudioHRTF hrtf;
        hrtf.render(silent, mix, HRTF_INDEX, 0.5f, 3.0f, gain, FRAMES);

        QVERIFY(!AudioMixSilence::hasAudio(mix, SAMPLES));
        QVERIFY(peakOf(mix) < 1e-10f);
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
        QVERIFY(!exactlyNonZero(mix));  // the SIMD filters flush to zero: the old check was right here
#else
        QVERIFY(exactlyNonZero(mix));   // the portable filters: the old check called this sound (the clicks after a join)
#endif
    }
}

// a real voice stays sound, however quiet, as long as it reaches the output: a 300 Hz tone of the given peak (16-bit
// steps) through the HRTF at the given distance gain, 5 frames, each mixed into a cleared buffer as prepareMix does
void AudioMixSilenceTests::quietVoice_data() {
    QTest::addColumn<int>("peak");
    QTest::addColumn<float>("gain");
    QTest::addColumn<float>("azimuth");
    QTest::addColumn<bool>("sound");
    QTest::newRow("a voice nearby") << 3000 << 0.5f << 0.0f << true;
    QTest::newRow("a quiet voice far off (about 5 steps)") << 100 << 0.05f << 1.2f << true;
    QTest::newRow("a whisper at the edge (about 2 steps)") << 8 << 0.25f << -2.0f << true;
    QTest::newRow("two steps at full gain") << 2 << 1.0f << 0.0f << true;
    QTest::newRow("a tenth of a step (rounds to 0 at the output)") << 1 << 0.1f << 0.0f << false;
}

void AudioMixSilenceTests::quietVoice() {
    QFETCH(int, peak);
    QFETCH(float, gain);
    QFETCH(float, azimuth);
    QFETCH(bool, sound);

    AudioHRTF hrtf;
    long t = 0;
    for (int frame = 0; frame < 5; ++frame) {
        int16_t input[FRAMES];
        for (int i = 0; i < FRAMES; ++i, ++t) {
            input[i] = (int16_t)std::lround(peak * std::sin(2.0 * M_PI * 300.0 * t / AudioConstants::SAMPLE_RATE));
        }
        float mix[SAMPLES] = {};
        hrtf.render(input, mix, HRTF_INDEX, azimuth, 2.0f, gain, FRAMES);
        QVERIFY2(AudioMixSilence::hasAudio(mix, SAMPLES) == sound,
                 qPrintable(QString("frame %1: mix peak %2 steps").arg(frame).arg(peakOf(mix) / STEP)));
    }
}
