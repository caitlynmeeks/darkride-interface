//
//  AudioInjectorOptions.cpp
//  libraries/audio/src
//
//  Created by Stephen Birarda on 1/2/2014.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AudioInjectorOptions.h"

#include <RegisteredMetaTypes.h>

#include "AudioLogging.h"

AudioInjectorOptions::AudioInjectorOptions() :
    position(0.0f, 0.0f, 0.0f),
    positionSet(true),  // default to legacy behavior
    volume(1.0f),
    loop(false),
    orientation(glm::vec3(0.0f, 0.0f, 0.0f)),
    stereo(false),
    ambisonic(false),
    ignorePenumbra(false),
    localOnly(false),
    secondOffset(0.0f),
    pitch(1.0f)
{
}

/**jsdoc
 * Configures where and how an audio injector plays its audio.
 * @typedef {object} AudioInjector.AudioInjectorOptions
 * @property {Vec3} position=Vec3.ZERO - The position in the domain to play the sound.
 * @property {Quat} orientation=Quat.IDENTITY - The orientation in the domain to play the sound in.
 * @property {number} volume=1.0 - Playback volume, between <code>0.0</code> and <code>1.0</code>.
 * @property {number} pitch=1.0 - Alter the pitch of the sound, within +/- 2 octaves. The value is the relative sample rate to 
 *     resample the sound at, range <code>0.0625</code> &ndash; <code>16.0</code>.<br />
 *     A value of <code>0.0625</code> lowers the pitch by 2 octaves.<br />
 *     A value of <code>1.0</code> means there is no change in pitch.<br />
 *     A value of <code>16.0</code> raises the pitch by 2 octaves.
 * @property {boolean} loop=false - If <code>true</code>, the sound is played repeatedly until playback is stopped.
 * @property {number} secondOffset=0 - Starts playback from a specified time (seconds) within the sound file, &ge; 
 *     <code>0</code>.
 * @property {boolean} localOnly=false - If <code>true</code>, the sound is played back locally on the client rather than to
 *     others via the audio mixer.
 * @property {boolean} ignorePenumbra=false - <p class="important">Deprecated: This property is deprecated and will be
 *     removed.</p>
 */
