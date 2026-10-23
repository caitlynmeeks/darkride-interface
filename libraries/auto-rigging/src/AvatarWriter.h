//
//  AvatarWriter.h
//  libraries/auto-rigging/src
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_AvatarWriter_h
#define hifi_AvatarWriter_h

#include <QString>
#include <QStringList>

#include "AutoRig.h"

namespace autorig {

struct WrittenAvatar {
    bool ok { false };
    QString error;
    QString fbxPath;
    QString fstPath;
    QStringList notes;
};

// Writes a rigged character as a Darkride avatar in a folder: <name>.fbx (binary FBX 7.4 in centimetres, Y up, facing
// +Z, the standard skeleton under one root with Mixamo's joint axes, the meshes with their materials, skin clusters and
// a bind pose), the textures the model uses copied beside it, and <name>.fst. sourcePath is the file the model was
// read from, for finding its textures. The body must be the one gathered from the model and rigged.
// Whether the writer flips texture V, so a model whose textures the engine draws upside down comes out the right way
// up. Detect decides from the textures' seams (a note says what it measured); Always and Never decide by hand. A model
// read right, as Chloe's rip is, is kept: her upside-down textures (10-13) came from the writer, now fixed.
enum class FlipV { Detect, Always, Never };

WrittenAvatar writeAvatar(const hfm::Model& model, const Body& body, const Result& rig, const QString& sourcePath,
                          const QString& folder, const QString& name, FlipV flipV = FlipV::Detect);

// Writes an .fst: the avatar's name, its model file (a path beside the .fst, or a URL) at a scale of one, and the
// joints the engine treats specially, by their standard names. False if it cannot be written.
bool writeFst(const QString& fstPath, const QString& name, const QString& modelFilename, float scale = 1.0f);

}

#endif // hifi_AvatarWriter_h
