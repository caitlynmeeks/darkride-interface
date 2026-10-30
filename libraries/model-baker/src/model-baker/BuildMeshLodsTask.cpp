//
//  BuildMeshLodsTask.cpp
//  model-baker/src/model-baker
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "BuildMeshLodsTask.h"

#include <algorithm>
#include <limits>

#include <QElapsedTimer>

#include <meshoptimizer.h>

#include "ModelBakerLogging.h"

namespace {
    // AUTOMATIC-LODS.md 3.1
    const float LOD_RATIOS[] = { 0.5f, 0.2f };   // each level's share of the full mesh's triangles, finest first
    const size_t LOD_COUNT = sizeof(LOD_RATIOS) / sizeof(LOD_RATIOS[0]);
    const size_t MIN_PART_TRIANGLES = 128;        // a smaller part repeats its full range in every level
    const float MIN_SAVING = 0.15f;               // a level must have this share fewer triangles than the one before
    const size_t INDICES_PER_TRIANGLE = 3;
    // the normals and texture coordinates count in a level's error beside its positions: a normal turned by a tenth of
    // a radian weighs as a twentieth of the mesh's size, so a level that changes the shading (a smooth panel whose
    // bevels are gone stretching its corners' normals across it) shows only from where the mesh is small on screen.
    // Measured by step3-tour.js: with positions alone, the 20 % levels of a chair, a shelf and the chessboard changed
    // the shading of 15 to 29 % of their pixels where their surface strayed under a pixel.
    const float NORMAL_WEIGHT = 0.5f;
    const float TEXCOORD_WEIGHT = 0.5f;

    // a part's triangles as BuildGraphicsMeshTask lays them out: its quads' triangles, then its triangles
    std::vector<uint32_t> partIndices(const hfm::MeshPart& part) {
        std::vector<uint32_t> indices;
        indices.reserve(part.quadTrianglesIndices.size() + part.triangleIndices.size());
        for (int index : part.quadTrianglesIndices) {
            indices.push_back((uint32_t)index);
        }
        for (int index : part.triangleIndices) {
            indices.push_back((uint32_t)index);
        }
        return indices;
    }

    // the attributes the simplifier weighs beside positions, interleaved per vertex: the normal (the baker's, which
    // computes them for meshes that have none) and the first texture coordinates, where the mesh has them
    std::vector<float> lodAttributes(const hfm::Mesh& hfmMesh, const baker::MeshNormals& normals, std::vector<float>& weights) {
        const size_t vertexCount = (size_t)hfmMesh.vertices.size();
        const bool hasNormals = normals.size() == vertexCount;
        const bool hasTexCoords = (size_t)hfmMesh.texCoords.size() == vertexCount;
        weights.clear();
        if (hasNormals) {
            weights.insert(weights.end(), 3, NORMAL_WEIGHT);
        }
        if (hasTexCoords) {
            weights.insert(weights.end(), 2, TEXCOORD_WEIGHT);
        }
        std::vector<float> attributes;
        if (weights.empty()) {
            return attributes;
        }
        attributes.reserve(vertexCount * weights.size());
        for (size_t i = 0; i < vertexCount; i++) {
            if (hasNormals) {
                attributes.push_back(normals[i].x);
                attributes.push_back(normals[i].y);
                attributes.push_back(normals[i].z);
            }
            if (hasTexCoords) {
                attributes.push_back(hfmMesh.texCoords[(int)i].x);
                attributes.push_back(hfmMesh.texCoords[(int)i].y);
            }
        }
        return attributes;
    }

    // the levels of one mesh, their indices appended to lodIndices; empty when no level saves enough
    std::vector<graphics::Mesh::Lod> buildLods(const hfm::Mesh& hfmMesh, const baker::MeshNormals& normals,
                                               std::vector<uint32_t>& lodIndices) {
        std::vector<graphics::Mesh::Lod> lods;
        std::vector<std::vector<uint32_t>> fullParts;
        size_t fullTriangles = 0;
        bool anyLargePart = false;
        for (const auto& part : hfmMesh.parts) {
            fullParts.push_back(partIndices(part));
            size_t triangles = fullParts.back().size() / INDICES_PER_TRIANGLE;
            fullTriangles += triangles;
            anyLargePart = anyLargePart || triangles >= MIN_PART_TRIANGLES;
        }
        if (!anyLargePart) {
            return lods;
        }

        const float* positions = reinterpret_cast<const float*>(hfmMesh.vertices.constData());
        const size_t vertexCount = (size_t)hfmMesh.vertices.size();
        // a broken file may index vertices the mesh does not have; the simplifier trusts its indices, so such a
        // mesh gets no levels
        for (const auto& source : fullParts) {
            for (uint32_t index : source) {
                if (index >= vertexCount) {
                    return {};
                }
            }
        }
        std::vector<float> weights;
        const std::vector<float> attributes = lodAttributes(hfmMesh, normals, weights);
        // the error comes back in the mesh's units; the parts of a mesh share their edges, which stay where they are so
        // that no crack opens between two materials, and each part is a subset of the mesh's vertices
        unsigned int options = meshopt_SimplifyErrorAbsolute;
        if (fullParts.size() > 1) {
            options |= meshopt_SimplifyLockBorder | meshopt_SimplifySparse;
        }

        size_t previousTriangles = fullTriangles;
        for (float ratio : LOD_RATIOS) {
            graphics::Mesh::Lod lod;
            std::vector<uint32_t> levelIndices;
            for (const auto& source : fullParts) {
                size_t triangles = source.size() / INDICES_PER_TRIANGLE;
                std::vector<uint32_t> simplified;
                if (triangles >= MIN_PART_TRIANGLES) {
                    simplified.resize(source.size());
                    float error = 0.0f;
                    size_t targetIndices = (size_t)(triangles * ratio) * INDICES_PER_TRIANGLE;
                    size_t count = attributes.empty()
                        ? meshopt_simplify(simplified.data(), source.data(), source.size(), positions, vertexCount,
                                           sizeof(glm::vec3), targetIndices, std::numeric_limits<float>::max(), options, &error)
                        : meshopt_simplifyWithAttributes(simplified.data(), source.data(), source.size(), positions,
                                                         vertexCount, sizeof(glm::vec3), attributes.data(),
                                                         weights.size() * sizeof(float), weights.data(), weights.size(),
                                                         nullptr, targetIndices, std::numeric_limits<float>::max(), options,
                                                         &error);
                    if (count > 0) {
                        simplified.resize(count);
                        meshopt_optimizeVertexCache(simplified.data(), simplified.data(), count, vertexCount);
                        lod.error = std::max(lod.error, error);
                    } else {
                        simplified = source;   // a part never vanishes from a level
                    }
                } else {
                    simplified = source;
                }
                graphics::Mesh::Part part((graphics::Index)(lodIndices.size() + levelIndices.size()),
                                          (graphics::Index)simplified.size(), 0, graphics::Mesh::TRIANGLES);
                lod.parts.push_back(part);
                levelIndices.insert(levelIndices.end(), simplified.begin(), simplified.end());
            }
            size_t levelTriangles = levelIndices.size() / INDICES_PER_TRIANGLE;
            if ((float)levelTriangles > (float)previousTriangles * (1.0f - MIN_SAVING)) {
                continue;   // too close to the level before; a coarser ratio may still save enough
            }
            previousTriangles = levelTriangles;
            lodIndices.insert(lodIndices.end(), levelIndices.begin(), levelIndices.end());
            lods.push_back(lod);
        }
        return lods;
    }
}

void BuildMeshLodsTask::run(const baker::BakeContextPointer& context, const Input& input) {
    const auto& meshes = input.get0();
    const auto& graphicsMeshes = input.get1();
    const auto& url = input.get2();
    const auto& normalsPerMesh = input.get3();

    QElapsedTimer timer;
    timer.start();
    int meshesWithLods = 0;
    size_t levelsBuilt = 0;   // the most levels any mesh has
    // for the log line: the triangles drawn and the largest error when every mesh takes its level i, or its coarsest
    // one below that
    size_t fullTriangles = 0;
    std::vector<size_t> trianglesAtLevel(LOD_COUNT, 0);
    std::vector<float> errorAtLevel(LOD_COUNT, 0.0f);

    size_t count = std::min(meshes.size(), graphicsMeshes.size());
    for (size_t i = 0; i < count; i++) {
        const hfm::Mesh& hfmMesh = meshes[i];
        const graphics::MeshPointer& graphicsMesh = graphicsMeshes[i];
        if (!graphicsMesh) {
            continue;
        }
        size_t meshTriangles = 0;
        for (const auto& part : hfmMesh.parts) {
            meshTriangles += (part.quadTrianglesIndices.size() + part.triangleIndices.size()) / INDICES_PER_TRIANGLE;
        }
        // skinned and blendshape meshes (avatars) are not in phase 2 (AUTOMATIC-LODS.md 3.6)
        bool eligible = !hfmMesh.vertices.empty() && hfmMesh.clusterIndices.empty() && hfmMesh.blendshapes.empty() &&
            graphicsMesh->getNumParts() == hfmMesh.parts.size();
        std::vector<graphics::Mesh::Lod> lods;
        std::vector<uint32_t> lodIndices;
        if (eligible) {
            static const baker::MeshNormals NO_NORMALS;
            lods = buildLods(hfmMesh, i < normalsPerMesh.size() ? normalsPerMesh[i] : NO_NORMALS, lodIndices);
        }
        fullTriangles += meshTriangles;
        for (size_t level = 0; level < trianglesAtLevel.size(); level++) {
            if (lods.empty()) {
                trianglesAtLevel[level] += meshTriangles;
                continue;
            }
            const auto& lod = lods[std::min(level, lods.size() - 1)];
            size_t triangles = 0;
            for (const auto& part : lod.parts) {
                triangles += part._numIndices / INDICES_PER_TRIANGLE;
            }
            trianglesAtLevel[level] += triangles;
            errorAtLevel[level] = std::max(errorAtLevel[level], lod.error);
        }
        if (lods.empty()) {
            continue;
        }
        auto buffer = std::make_shared<gpu::Buffer>();
        buffer->setData(lodIndices.size() * sizeof(uint32_t), (const gpu::Byte*)lodIndices.data());
        graphicsMesh->setLods(gpu::BufferView(buffer, gpu::Element(gpu::SCALAR, gpu::UINT32, gpu::INDEX)), lods);
        meshesWithLods++;
        levelsBuilt = std::max(levelsBuilt, lods.size());
    }

    if (meshesWithLods > 0) {
        // the error is in the mesh's units, metres for glTF (the normals' and texture coordinates' share counted as a
        // distance: NORMAL_WEIGHT)
        QString line = QString("automatic LODs: %1: %2 of %3 meshes, full %4 triangles")
            .arg(url.toDisplayString()).arg(meshesWithLods).arg(count).arg(fullTriangles);
        for (size_t level = 0; level < levelsBuilt; level++) {
            line += QString("; level %1: %2 triangles (%3 %), error up to %4 mm")
                .arg(level + 1).arg(trianglesAtLevel[level])
                .arg(fullTriangles > 0 ? 100.0 * trianglesAtLevel[level] / fullTriangles : 0.0, 0, 'f', 0)
                .arg(errorAtLevel[level] * 1000.0f, 0, 'f', 2);
        }
        line += QString("; built in %1 ms").arg(timer.nsecsElapsed() / 1.0e6, 0, 'f', 2);
        qCDebug(model_baker).noquote() << line;
    }
}
