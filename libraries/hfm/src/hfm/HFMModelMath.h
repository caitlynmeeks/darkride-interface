//
//  HFMModelMath.h
//  model-baker/src/model-baker
//
//  Created by Sabrina Shanman on 2019/10/04.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_hfm_ModelMath_h
#define hifi_hfm_ModelMath_h

#include "HFM.h"

namespace hfm {

void forEachIndex(const hfm::MeshPart& meshPart, std::function<void(uint32_t)> func);

void initializeExtents(Extents& extents);

void calculateExtentsForTriangleListMesh(TriangleListMesh& triangleListMesh);

// This can't be moved to model-baker until
void calculateExtentsForShape(hfm::Shape& shape, const std::vector<hfm::TriangleListMesh>& triangleListMeshes, const std::vector<hfm::Joint>& joints);

void calculateExtentsForModel(Extents& modelExtents, const std::vector<hfm::Shape>& shapes);

struct ReweightedDeformers {
    std::vector<uint16_t> indices;
    std::vector<uint16_t> weights;
    uint16_t weightsPerVertex { 0 };
    bool trimmedToMatch { false };
};

const uint16_t DEFAULT_SKINNING_WEIGHTS_PER_VERTEX = 4;

ReweightedDeformers getReweightedDeformers(const size_t numMeshVertices, const std::vector<hfm::SkinCluster> skinClusters, const uint16_t weightsPerVertex = DEFAULT_SKINNING_WEIGHTS_PER_VERTEX);

const TriangleListMesh generateTriangleListMesh(const std::vector<glm::vec3>& srcVertices, const std::vector<HFMMeshPart>& srcParts);

struct MeshSanitizeResult {
    int facesRemoved { 0 };
    int listsCleared { 0 };
    int blendshapesCleared { 0 };

    bool changed() const { return facesRemoved > 0 || listsCleared > 0 || blendshapesCleared > 0; }
};

// A reader's last check on a mesh it made (FBX, OBJ): the passes after the reader (the GPU index buffer, Draco, LODs,
// forEachIndex, the blendshape blend) index the vertices unchecked. Removes every triangle and quad naming a vertex
// the mesh does not have (a negative index included) and a face cut short at the end of its list; clears a per-vertex
// list (normals, tangents, colours, texture coordinates) whose length is not the vertex count; empties a blendshape
// naming a vertex the mesh does not have or whose vertices are not one per index (its slot stays), and clears its
// normals or tangents when they are not one per index. Vertices are never removed. A mesh that already holds
// together is only read, never copied.
MeshSanitizeResult sanitizeMesh(hfm::Mesh& mesh);

// The last step of every model read (ModelLoader::load, whichever reader): a mesh the model baker could make no drawable
// for (no vertices, or no triangle or quad-triangle index in any of its parts) while it has parts is kept as one empty
// (zero-area) triangle on its first vertex, so it is built and draws, picks and collides as nothing. The renderer makes
// a draw item for every part of every mesh and reads its drawable unchecked, so such a mesh crashed the app. Meshes and
// parts are never removed: materials and scripts name a model's parts by their number. A mesh with no vertices gets one
// at its origin and loses what could only name vertices it doesn't have (per-vertex lists, skinning, blendshapes).
// Returns the number of meshes changed; a model whose meshes all hold together is only read.
int keepMeshesDrawable(hfm::Model& model);

};

#endif // #define hifi_hfm_ModelMath_h
