//
//  HFMModelMath.cpp
//  model-baker/src/model-baker
//
//  Created by Sabrina Shanman on 2019/10/04.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "HFMModelMath.h"
#include "ModelFormatLogging.h"

#include <LogHandler.h>

#include <unordered_map>

#include <GLMHelpers.h>
#include <glm/gtx/hash.hpp>

namespace hfm {

void forEachIndex(const hfm::MeshPart& meshPart, std::function<void(uint32_t)> func) {
    for (int i = 0; i <= meshPart.quadIndices.size() - 4; i += 4) {
        func((uint32_t)meshPart.quadIndices[i]);
        func((uint32_t)meshPart.quadIndices[i+1]);
        func((uint32_t)meshPart.quadIndices[i+2]);
        func((uint32_t)meshPart.quadIndices[i+3]);
    }
    for (int i = 0; i <= meshPart.triangleIndices.size() - 3; i += 3) {
        func((uint32_t)meshPart.triangleIndices[i]);
        func((uint32_t)meshPart.triangleIndices[i+1]);
        func((uint32_t)meshPart.triangleIndices[i+2]);
    }
}

void thickenFlatExtents(Extents& extents) {
    // Add epsilon to extents to compensate for flat plane
    extents.minimum -= glm::vec3(EPSILON, EPSILON, EPSILON);
    extents.maximum += glm::vec3(EPSILON, EPSILON, EPSILON);
}

void calculateExtentsForTriangleListMesh(TriangleListMesh& triangleListMesh) {
    triangleListMesh.partExtents.resize(triangleListMesh.parts.size());
    for (size_t partIndex = 0; partIndex < triangleListMesh.parts.size(); ++partIndex) {
        const auto& part = triangleListMesh.parts[partIndex];
        auto& extents = triangleListMesh.partExtents[partIndex];
        int partEnd = part.x + part.y;
        for (int i = part.x; i < partEnd; ++i) {
            auto index = triangleListMesh.indices[i];
            const auto& position = triangleListMesh.vertices[index];
            extents.addPoint(position);
        }
    }
}

void calculateExtentsForShape(hfm::Shape& shape, const std::vector<hfm::TriangleListMesh>& triangleListMeshes, const std::vector<hfm::Joint>& joints) {
    auto& shapeExtents = shape.transformedExtents;
    shapeExtents.reset();

    const auto& triangleListMesh = triangleListMeshes[shape.mesh];
    const auto& partExtent = triangleListMesh.partExtents[shape.meshPart];

    const glm::mat4& transform = joints[shape.joint].transform;
    shapeExtents = partExtent;
    shapeExtents.transform(transform);

    thickenFlatExtents(shapeExtents);
}

void calculateExtentsForModel(Extents& modelExtents, const std::vector<hfm::Shape>& shapes) {
    modelExtents.reset();

    for (size_t i = 0; i < shapes.size(); ++i) {
        const auto& shape = shapes[i];
        const auto& shapeExtents = shape.transformedExtents;
        modelExtents.addExtents(shapeExtents);
    }
}

ReweightedDeformers getReweightedDeformers(const size_t numMeshVertices, const std::vector<hfm::SkinCluster> skinClusters, const uint16_t weightsPerVertex) {
    ReweightedDeformers reweightedDeformers;
    if (skinClusters.size() == 0) {
        return reweightedDeformers;
    }

    size_t numClusterIndices = numMeshVertices * weightsPerVertex;
    reweightedDeformers.indices.resize(numClusterIndices, (uint16_t)(skinClusters.size() - 1));
    reweightedDeformers.weights.resize(numClusterIndices, 0);
    reweightedDeformers.weightsPerVertex = weightsPerVertex;

    std::vector<float> weightAccumulators;
    weightAccumulators.resize(numClusterIndices, 0.0f);
    for (uint16_t i = 0; i < (uint16_t)skinClusters.size(); ++i) {
        const hfm::SkinCluster& skinCluster = skinClusters[i];

        if (skinCluster.indices.size() != skinCluster.weights.size()) {
            reweightedDeformers.trimmedToMatch = true;
        }
        size_t numIndicesOrWeights = std::min(skinCluster.indices.size(), skinCluster.weights.size());
        for (size_t j = 0; j < numIndicesOrWeights; ++j) {
            uint32_t index = skinCluster.indices[j];
            float weight = skinCluster.weights[j];

            // look for an unused slot in the weights vector
            uint32_t weightIndex = index * weightsPerVertex;
            uint32_t lowestIndex = -1;
            float lowestWeight = FLT_MAX;
            uint16_t k = 0;
            for (; k < weightsPerVertex; k++) {
                if (weightAccumulators[weightIndex + k] == 0.0f) {
                    reweightedDeformers.indices[weightIndex + k] = i;
                    weightAccumulators[weightIndex + k] = weight;
                    break;
                }
                if (weightAccumulators[weightIndex + k] < lowestWeight) {
                    lowestIndex = k;
                    lowestWeight = weightAccumulators[weightIndex + k];
                }
            }
            if (k == weightsPerVertex && weight > lowestWeight) {
                // no space for an additional weight; we must replace the lowest
                weightAccumulators[weightIndex + lowestIndex] = weight;
                reweightedDeformers.indices[weightIndex + lowestIndex] = i;
            }
        }
    }

    // now that we've accumulated the most relevant weights for each vertex
    // normalize and compress to 16-bits
    for (size_t i = 0; i < numMeshVertices; ++i) {
        size_t j = i * weightsPerVertex;

        // normalize weights into uint16_t
        float totalWeight = 0.0f;
        for (size_t k = j; k < j + weightsPerVertex; ++k) {
            totalWeight += weightAccumulators[k];
        }

        const float ALMOST_HALF = 0.499f;
        if (totalWeight > 0.0f) {
            float weightScalingFactor = (float)(UINT16_MAX) / totalWeight;
            for (size_t k = j; k < j + weightsPerVertex; ++k) {
                reweightedDeformers.weights[k] = (uint16_t)(weightScalingFactor * weightAccumulators[k] + ALMOST_HALF);
            }
        } else {
            reweightedDeformers.weights[j] = (uint16_t)((float)(UINT16_MAX) + ALMOST_HALF);
        }
    }

    return reweightedDeformers;
}

const TriangleListMesh generateTriangleListMesh(const std::vector<glm::vec3>& srcVertices, const std::vector<HFMMeshPart>& srcParts) {

    TriangleListMesh dest;

    // copy vertices for now
    dest.vertices = srcVertices;

    std::vector<uint32_t> oldToNewIndex(srcVertices.size());
    {
        std::unordered_map<glm::vec3, uint32_t> uniqueVertexToNewIndex;
        int oldIndex = 0;
        int newIndex = 0;
        for (const auto& srcVertex : srcVertices) {
            auto foundIndex = uniqueVertexToNewIndex.find(srcVertex);
            if (foundIndex != uniqueVertexToNewIndex.end()) {
                oldToNewIndex[oldIndex] = foundIndex->second;
            } else {
                uniqueVertexToNewIndex[srcVertex] = newIndex;
                oldToNewIndex[oldIndex] = newIndex;
                dest.vertices[newIndex] = srcVertex;
                ++newIndex;
            }
            ++oldIndex;
        }
        if (uniqueVertexToNewIndex.size() < srcVertices.size()) {
            dest.vertices.resize(uniqueVertexToNewIndex.size());
            dest.vertices.shrink_to_fit();
        }
    }

    auto newIndicesCount = 0;
    for (const auto& part : srcParts) {
        newIndicesCount += part.triangleIndices.size() + part.quadTrianglesIndices.size();
    }

    {
        // a triangle at a time: one with a vertex past the source's is left out rather than read past oldToNewIndex
        dest.indices.resize(newIndicesCount);
        int i = 0;
        int trianglesLeftOut = 0;
        const auto copyTriangles = [&](const QVector<int>& indices) {
            for (int t = 0; t + 2 < indices.size(); t += 3) {
                const uint32_t a = (uint32_t)indices[t], b = (uint32_t)indices[t + 1], c = (uint32_t)indices[t + 2];
                if (a >= srcVertices.size() || b >= srcVertices.size() || c >= srcVertices.size()) {
                    ++trianglesLeftOut;
                    continue;
                }
                dest.indices[i++] = oldToNewIndex[a];
                dest.indices[i++] = oldToNewIndex[b];
                dest.indices[i++] = oldToNewIndex[c];
            }
        };
        for (const auto& part : srcParts) {
            glm::ivec2 spart(i, 0);
            copyTriangles(part.quadTrianglesIndices);
            copyTriangles(part.triangleIndices);
            spart.y = i - spart.x;
            dest.parts.push_back(spart);
        }
        dest.indices.resize(i);
        if (trianglesLeftOut > 0) {
            qCWarning(modelformat) << "generateTriangleListMesh:" << trianglesLeftOut << "triangles with a vertex past the mesh's"
                       << srcVertices.size() << "left out";
        }
    }

    calculateExtentsForTriangleListMesh(dest);

    return dest;
}

// Removes the faces of `indices` (`faceSize` indices each) that name a vertex at or past vertexCount, and a face cut
// short at the end. Returns how many faces it removed. The list is read through a const reference first, so a list
// shared with another mesh is not copied unless something has to go.
static int removeFacesPastVertexCount(QVector<int>& indices, int faceSize, uint32_t vertexCount) {
    const QVector<int>& in = indices;
    const int wholeFaces = in.size() / faceSize;
    const bool cutShort = (in.size() % faceSize) != 0;
    const auto faceInside = [&](int start) {
        for (int corner = 0; corner < faceSize; ++corner) {
            if ((uint32_t)in[start + corner] >= vertexCount) {  // a negative index wraps past any vertex count
                return false;
            }
        }
        return true;
    };

    int firstBad = 0;
    while (firstBad < wholeFaces && faceInside(firstBad * faceSize)) {
        ++firstBad;
    }
    if (firstBad == wholeFaces && !cutShort) {
        return 0;
    }

    int removed = cutShort ? 1 : 0;
    int kept = firstBad * faceSize;
    for (int face = firstBad; face < wholeFaces; ++face) {
        const int start = face * faceSize;
        if (!faceInside(start)) {
            ++removed;
            continue;
        }
        for (int corner = 0; corner < faceSize; ++corner) {
            indices[kept + corner] = in[start + corner];
        }
        kept += faceSize;
    }
    indices.resize(kept);
    return removed;
}

template <typename T>
static int clearIfNotPerVertex(QVector<T>& list, int vertexCount) {
    if (!list.isEmpty() && list.size() != vertexCount) {
        list.clear();
        return 1;
    }
    return 0;
}

// the indices BuildGraphicsMeshTask counts: without one it makes no drawable for the mesh
static bool hasBakedIndex(const hfm::Mesh& mesh) {
    for (const auto& part : mesh.parts) {
        if (!part.triangleIndices.isEmpty() || !part.quadTrianglesIndices.isEmpty()) {
            return true;
        }
    }
    return false;
}

int keepMeshesDrawable(hfm::Model& model) {
    int changed = 0;
    for (auto& mesh : model.meshes) {
        if (mesh.parts.empty() || (!mesh.vertices.isEmpty() && hasBakedIndex(mesh))) {
            continue;
        }
        if (mesh.vertices.isEmpty()) {
            // nothing can name a vertex the mesh doesn't have: what would is dropped, then one vertex at the origin
            mesh.normals.clear();
            mesh.tangents.clear();
            mesh.colors.clear();
            mesh.texCoords.clear();
            mesh.texCoords1.clear();
            mesh.clusterIndices.clear();
            mesh.clusterWeights.clear();
            mesh.originalIndices.clear();
            for (auto& blendshape : mesh.blendshapes) {
                blendshape = hfm::Blendshape();  // the slot stays: the blendshape channels are numbered by it
            }
            for (auto& part : mesh.parts) {
                part.triangleIndices.clear();
                part.quadTrianglesIndices.clear();
                part.quadIndices.clear();
            }
            mesh.vertices.push_back(glm::vec3(0.0f));
        }
        mesh.parts[0].triangleIndices = { 0, 0, 0 };
        ++changed;
    }
    return changed;
}

MeshSanitizeResult sanitizeMesh(hfm::Mesh& mesh) {
    MeshSanitizeResult result;
    const int vertexCount = mesh.vertices.size();
    for (auto& part : mesh.parts) {
        result.facesRemoved += removeFacesPastVertexCount(part.triangleIndices, 3, (uint32_t)vertexCount);
        result.facesRemoved += removeFacesPastVertexCount(part.quadTrianglesIndices, 3, (uint32_t)vertexCount);
        result.facesRemoved += removeFacesPastVertexCount(part.quadIndices, 4, (uint32_t)vertexCount);
    }
    result.listsCleared += clearIfNotPerVertex(mesh.normals, vertexCount);
    result.listsCleared += clearIfNotPerVertex(mesh.tangents, vertexCount);
    result.listsCleared += clearIfNotPerVertex(mesh.colors, vertexCount);
    result.listsCleared += clearIfNotPerVertex(mesh.texCoords, vertexCount);
    result.listsCleared += clearIfNotPerVertex(mesh.texCoords1, vertexCount);

    // blendshapes: Model's blend and the baker's normals and tangents index the mesh's vertices with a blendshape's
    // indices, and its vertices, normals and tangents with the position in that list, all unchecked. A blendshape with
    // an index past the vertices, or a vertex list not one per index, is emptied (its slot stays: the blendshape
    // channels are numbered by it); normals or tangents not one per index are cleared, and the baker makes them again.
    // Read through a const reference first, so a good mesh's list is not copied.
    const QVector<hfm::Blendshape>& shapes = mesh.blendshapes;
    for (int i = 0; i < shapes.size(); ++i) {
        const int count = shapes[i].indices.size();
        bool indicesInside = shapes[i].vertices.size() == count;
        for (int j = 0; indicesInside && j < count; ++j) {
            indicesInside = (uint32_t)shapes[i].indices[j] < (uint32_t)vertexCount;
        }
        if (!indicesInside) {
            mesh.blendshapes[i] = hfm::Blendshape();
            ++result.blendshapesCleared;
            continue;
        }
        const bool normalsWrong = !shapes[i].normals.isEmpty() && shapes[i].normals.size() != count;
        const bool tangentsWrong = !shapes[i].tangents.isEmpty() && shapes[i].tangents.size() != count;
        if (normalsWrong) {
            mesh.blendshapes[i].normals.clear();
            ++result.listsCleared;
        }
        if (tangentsWrong) {
            mesh.blendshapes[i].tangents.clear();
            ++result.listsCleared;
        }
    }
    return result;
}

};
