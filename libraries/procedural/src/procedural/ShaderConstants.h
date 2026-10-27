// <!
//  Created by Bradley Austin Davis on 2018/05/25
//  Copyright 2013-2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
// !>

// <@if not PROCEDURAL_SHADER_CONSTANTS_H@>
// <@def PROCEDURAL_SHADER_CONSTANTS_H@>

// Hack comment to absorb the extra '//' scribe prepends

#ifndef PROCEDURAL_SHADER_CONSTANTS_H
#define PROCEDURAL_SHADER_CONSTANTS_H

// 13, not 7 as it was: the forward path binds the haze's parameters at 7 for every item it draws (GRAPHICS_BUFFER_HAZE_PARAMS),
// and a procedural lit in place reads both (simple_procedural.slf's forward variant). 13 is the subsurface scattering's
// parameters, which only the deferred lighting passes read, binding them first, and the terrain's chunk, bound before
// each chunk is drawn; a batch takes slots 0 to 13 only (gpu::Batch's MAX_NUM_UNIFORM_BUFFERS)
#define PROCEDURAL_BUFFER_INPUTS 13

#define PROCEDURAL_UNIFORM_CUSTOM 220

#define PROCEDURAL_TEXTURE_CHANNEL0 2
#define PROCEDURAL_TEXTURE_CHANNEL1 3
#define PROCEDURAL_TEXTURE_CHANNEL2 4
#define PROCEDURAL_TEXTURE_CHANNEL3 5

// the roughness, metallic and occlusion maps of the material beneath a procedural on a model part
// (MeshPartPayload::bindUnderlyingMaterial): the material's own units (4, 2 and 5) are the channels', so these, which no
// procedural program reads otherwise (the lightmap's two, and the fade mask's, which it has no variant for)
#define PROCEDURAL_TEXTURE_UNDERLYING_ROUGHNESS 7
#define PROCEDURAL_TEXTURE_UNDERLYING_METALLIC 8
#define PROCEDURAL_TEXTURE_UNDERLYING_OCCLUSION 10

// <!

namespace procedural { namespace slot {

namespace buffer {
enum Bufffer {
    Inputs = PROCEDURAL_BUFFER_INPUTS,
};
}

namespace uniform {
enum Uniform {
    Custom = PROCEDURAL_UNIFORM_CUSTOM,
};
}

namespace texture {
enum Texture {
    Channel0 = PROCEDURAL_TEXTURE_CHANNEL0,
    Channel1 = PROCEDURAL_TEXTURE_CHANNEL1,
    Channel2 = PROCEDURAL_TEXTURE_CHANNEL2,
    Channel3 = PROCEDURAL_TEXTURE_CHANNEL3,
    UnderlyingRoughness = PROCEDURAL_TEXTURE_UNDERLYING_ROUGHNESS,
    UnderlyingMetallic = PROCEDURAL_TEXTURE_UNDERLYING_METALLIC,
    UnderlyingOcclusion = PROCEDURAL_TEXTURE_UNDERLYING_OCCLUSION,
};
} // namespace texture

} } // namespace procedural::slot

// !>
// Hack Comment

#endif // PROCEDURAL_SHADER_CONSTANTS_H

// <@if 1@>
// Trigger Scribe include
// <@endif@> <!def that !>

// <@endif@>

// Hack Comment
