//
//  FstBake.h
//  libraries/auto-rigging/src
//
//  Created on 2026-10-19 for Darkride's avatar support.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_FstBake_h
#define hifi_FstBake_h

#include <QByteArray>
#include <QStringList>

#include <hfm/HFM.h>
#include <VRMReader.h>

namespace autorig {

// A glTF's (or VRM's) morph target names, from its meshes' or primitives' extras, as the loaders read them
QStringList gltfMorphTargetNames(const QByteArray& data);

// What Darkride works out about a model when it's worn, as .fst lines, so that every client, older ones and other High
// Fidelity descendants among them, shows the avatar the same way, and the work isn't done on every load: the joints'
// names (jointMap), offsets for joints whose axes aren't Mixamo's (jointRotationOffset2), the morph targets for its face
// (bs; not a VRM's, whose expressions are read from the file), and a VRM's spring bones and colliders (flowPhysicsData,
// flowCollisionsData). What an FST can't say, as a joint name used twice, is left to the client to work out as it does
// without one. Takes the model as the client reads it to wear it, the VRM's data if it is one, and its morph target names;
// gives the lines, and what they say for the importer's question.
void bakeFst(const HFMModel& model, const VRMInfo& vrm, const QStringList& morphTargetNames, QStringList& lines, QStringList& found);

}

#endif // hifi_FstBake_h
