//
//  FstBake.h
//  libraries/auto-rigging/src
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_FstBake_h
#define hifi_FstBake_h

#include <QStringList>

#include <hfm/HFM.h>

namespace autorig {

// A model's morph target names as the loaders read them: its engine-named shapes and its own (the model read with
// "ownBlendshapes = true", as Import Avatar reads it)
QStringList morphTargetNames(const HFMModel& model);

// The face lines for a model whose morph targets have these names: a bs line for each engine blendshape it has no
// shape of that name for but another, such as ARKit's (findBlendshapeSource), one to one, and a bsGain line for each
// the procedural face drives less than fully. The same as wearing it without an .fst gives it.
QStringList bakeFaceLines(const QStringList& morphTargetNames, int* count = nullptr);

// What Darkride works out about a model when it's worn without an .fst, as .fst lines, so that every client, older ones
// and other High Fidelity descendants among them, shows the avatar the same way, and the work isn't done on every load:
// the joints' names (jointMap), offsets for joints whose axes aren't the ones animations expect (jointRotationOffset2),
// the morph targets for its face (bs and bsGain; not a VRM's, whose expressions are read from the file), and a VRM's
// spring bones and colliders (flowPhysicsData, flowCollisionsData), the colliders turned into the rig's axes. What an
// .fst can't say, as a joint name used twice, is left out. Takes the model as Import Avatar reads it; gives the lines,
// and what they say for the importer's question.
void bakeFst(const HFMModel& model, bool isVRM, QStringList& lines, QStringList& found);

}

#endif // hifi_FstBake_h
