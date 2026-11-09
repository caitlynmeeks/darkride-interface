//
//  AudioMixSilence.h
//  libraries/audio/src
//
//  Created by social on 2026-11-07.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_AudioMixSilence_h
#define hifi_AudioMixSilence_h

#include <cmath>

#include "AudioConstants.h"

namespace AudioMixSilence {

// A mix (floats, 1.0 = full scale) carries sound when a sample reaches half a 16-bit step; anything smaller rounds to 0
// at the 16-bit output, under the limiter's dither. Not "exactly zero": the HRTF's portable filters (every chip but
// Intel and AMD) add 1e-20 against denormals, so a silent render isn't 0, and a mix counted as sound for it sends a
// near-silent packet that leaves the listener's Opus decoder loud (the clicks after a join).
const float HALF_STEP = 0.5f / AudioConstants::MAX_SAMPLE_VALUE;

inline bool hasAudio(const float* mix, int numSamples) {
    bool audible = false;
    for (int i = 0; i < numSamples; ++i) {
        audible |= std::fabs(mix[i]) >= HALF_STEP;  // no early exit, so it vectorizes: cheapest in silence
    }
    return audible;
}

}

#endif // hifi_AudioMixSilence_h
