//
//  TerrainTests.cpp
//  tests/terrain/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "TerrainTests.h"

#include <atomic>
#include <cmath>
#include <map>

#include <QElapsedTimer>
#include <QThreadPool>

#include <terrain/AsyncMesher.h>
#include <terrain/Brush.h>
#include <terrain/LevelOfDetail.h>
#include <terrain/PaintMap.h>
#include <terrain/Resample.h>
#include <terrain/SurfaceNets.h>
#include <terrain/TerrainFile.h>
#include <terrain/TopSurface.h>
#include <terrain/VoxelGrid.h>

QTEST_MAIN(TerrainTests)

using namespace terrain;

namespace {

// Rolling ground: a few voxels of hills in both directions.
float hills(int x, int z) {
    return 10.0f + 3.0f * std::sin(x * 0.3f) + 2.0f * std::cos(z * 0.4f);
}

bool near(const glm::vec3& a, const glm::vec3& b, float tolerance) {
    return glm::length(a - b) <= tolerance;
}

} // namespace

void TerrainTests::chunkStaysUniformUntilAVoxelDiffers() {
    Chunk chunk(Voxel::air());
    QVERIFY(chunk.isUniform());
    QVERIFY(!chunk.set(1, 2, 3, Voxel::air()));
    QVERIFY(chunk.isUniform());

    QVERIFY(chunk.set(1, 2, 3, Voxel::ground(7)));
    QVERIFY(!chunk.isUniform());
    QVERIFY(chunk.get(1, 2, 3).isGround());
    QCOMPARE((int)chunk.get(1, 2, 3).material, 7);
    QVERIFY(!chunk.get(0, 0, 0).isGround());
    QCOMPARE((int)chunk.getVoxels().size(), CHUNK_VOXEL_COUNT);

    QVERIFY(chunk.set(1, 2, 3, Voxel::air()));
    QVERIFY(!chunk.isUniform());
    QVERIFY(chunk.compact());
    QVERIFY(chunk.isUniform());
    QVERIFY(chunk.getUniformValue() == Voxel::air());
}

void TerrainTests::gridFillsBelowAHeightAndDirtiesNeighboursAcrossChunkFaces() {
    VoxelGrid grid(glm::ivec3(64, 32, 64));
    QCOMPARE(grid.getChunkCount(), glm::ivec3(2, 1, 2));
    QCOMPARE((int)grid.getDirtyChunks().size(), 4);

    grid.fillBelow(10.5f, 3);
    QVERIFY(grid.get(glm::ivec3(5, 10, 5)).isGround());
    QVERIFY(!grid.get(glm::ivec3(5, 11, 5)).isGround());
    QCOMPARE((int)grid.get(glm::ivec3(5, 10, 5)).material, 3);
    QVERIFY(std::abs(grid.get(glm::ivec3(5, 10, 5)).getDistance() + 0.5f) < 1.0e-6f);
    // Every chunk holds the surface, so none is uniform.
    QCOMPARE(grid.countDenseChunks(), 4);
    // Outside the grid the nearest voxel in it is returned.
    QVERIFY(grid.get(glm::ivec3(-5, 10, 5)) == grid.get(glm::ivec3(0, 10, 5)));
    QVERIFY(grid.get(glm::ivec3(5, 100, 5)) == grid.get(glm::ivec3(5, 31, 5)));

    grid.clearDirtyChunks();
    QVERIFY(grid.set(glm::ivec3(5, 10, 5), Voxel::air()));
    QCOMPARE(grid.getDirtyChunks(), ChunkKeySet({ glm::ivec3(0, 0, 0) }));
    QVERIFY(!grid.set(glm::ivec3(5, 10, 5), Voxel::air()));

    grid.clearDirtyChunks();
    QVERIFY(grid.set(glm::ivec3(31, 10, 5), Voxel::air()));
    QCOMPARE(grid.getDirtyChunks(), ChunkKeySet({ glm::ivec3(0, 0, 0), glm::ivec3(1, 0, 0) }));

    grid.clearDirtyChunks();
    QVERIFY(grid.set(glm::ivec3(32, 10, 32), Voxel::ground()));
    QCOMPARE(grid.getDirtyChunks(), ChunkKeySet({ glm::ivec3(0, 0, 0), glm::ivec3(1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(1, 0, 1) }));

    QVERIFY(!grid.set(glm::ivec3(64, 10, 5), Voxel::air()));
}

void TerrainTests::flatGroundMeshesToOneLevelPlane() {
    VoxelGrid grid(glm::ivec3(32, 32, 32));
    grid.fillBelow(10.3f, 2);
    ChunkMesh mesh = meshChunk(grid, glm::ivec3(0, 0, 0));

    // One vertex per cell in the chunk and the layer outside its low faces, one quad per column.
    QCOMPARE((int)mesh.getVertexCount(), 33 * 33);
    QCOMPARE((int)mesh.getTriangleCount(), 2 * 32 * 32);
    for (size_t i = 0; i < mesh.getVertexCount(); ++i) {
        QVERIFY(std::abs(mesh.positions[i].y - 10.3f) < 0.02f);
        QVERIFY(mesh.positions[i].x >= -1.0f && mesh.positions[i].x < 32.0f);
        QVERIFY(mesh.positions[i].z >= -1.0f && mesh.positions[i].z < 32.0f);
        QVERIFY(near(mesh.normals[i], glm::vec3(0.0f, 1.0f, 0.0f), 1.0e-4f));
        QCOMPARE((int)mesh.materials[i], 2);
    }
    for (uint32_t index : mesh.indices) {
        QVERIFY(index < mesh.getVertexCount());
    }

    // A chunk with no surface in it meshes to nothing.
    QVERIFY(meshChunk(VoxelGrid(glm::ivec3(32, 32, 32), Voxel::ground()), glm::ivec3(0, 0, 0)).isEmpty());
}

void TerrainTests::neighbouringChunksMeetWithoutASeam() {
    VoxelGrid grid(glm::ivec3(64, 32, 32));
    grid.fillHeightField(hills);
    ChunkMesh left = meshChunk(grid, glm::ivec3(0, 0, 0));
    ChunkMesh right = meshChunk(grid, glm::ivec3(1, 0, 0));
    QVERIFY(!left.isEmpty() && !right.isEmpty());

    // The right chunk's vertices in the layer outside its low x face are the left chunk's in its last column:
    // where both have a vertex for a cell, it is the same vertex.
    std::map<std::tuple<int, int, int>, size_t> leftByCell;
    for (size_t i = 0; i < left.getVertexCount(); ++i) {
        const glm::vec3& p = left.positions[i];
        if (p.x >= 31.0f) {
            leftByCell[std::make_tuple((int)std::floor(p.x), (int)std::floor(p.y), (int)std::floor(p.z))] = i;
        }
    }
    int matched = 0;
    for (size_t i = 0; i < right.getVertexCount(); ++i) {
        const glm::vec3 p = right.positions[i] + glm::vec3(32.0f, 0.0f, 0.0f);
        if (p.x >= 32.0f) {
            continue;
        }
        auto found = leftByCell.find(std::make_tuple((int)std::floor(p.x), (int)std::floor(p.y), (int)std::floor(p.z)));
        if (found == leftByCell.end()) {
            continue;
        }
        QVERIFY(near(left.positions[found->second], p, 1.0e-5f));
        QVERIFY(near(left.normals[found->second], right.normals[i], 1.0e-5f));
        ++matched;
    }
    QVERIFY(matched >= 30);
}

void TerrainTests::trianglesFaceTheAirLikeTheirNormals() {
    VoxelGrid grid(glm::ivec3(32, 32, 32));
    grid.fillHeightField(hills);
    ChunkMesh mesh = meshChunk(grid, glm::ivec3(0, 0, 0));
    QVERIFY(mesh.getTriangleCount() > 1000);

    glm::vec3 meanNormal(0.0f);
    for (size_t t = 0; t < mesh.getTriangleCount(); ++t) {
        const uint32_t a = mesh.indices[3 * t], b = mesh.indices[3 * t + 1], c = mesh.indices[3 * t + 2];
        const glm::vec3 faceNormal = glm::cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
        if (glm::length(faceNormal) < 1.0e-8f) {
            continue;
        }
        const glm::vec3 vertexNormal = mesh.normals[a] + mesh.normals[b] + mesh.normals[c];
        QVERIFY(glm::dot(faceNormal, vertexNormal) > 0.0f);
        meanNormal += glm::normalize(faceNormal);
    }
    meanNormal = glm::normalize(meanNormal);
    QVERIFY(meanNormal.y > 0.9f);
}

void TerrainTests::brushesDigFillAndRaise() {
    VoxelGrid grid(glm::ivec3(64, 64, 64));
    grid.fillBelow(40.5f, 1);
    grid.clearDirtyChunks();

    Brush brush;
    brush.center = glm::vec3(32.0f, 40.5f, 32.0f);
    brush.radius = 8.0f;
    brush.material = 2;
    QVERIFY(sculpt(grid, brush, SculptMode::Dig) > 0);
    QVERIFY(!grid.get(glm::ivec3(32, 38, 32)).isGround());   // inside the sphere, below the old surface
    QVERIFY(grid.get(glm::ivec3(32, 30, 32)).isGround());    // below the sphere
    QVERIFY(grid.get(glm::ivec3(50, 38, 32)).isGround());    // beside it
    QVERIFY(!grid.getDirtyChunks().empty());

    // The crater's floor is at the sphere's bottom, 32.5; the mesh's positions are relative to the chunk's origin.
    ChunkMesh mesh = meshChunk(grid, glm::ivec3(1, 1, 1));
    float lowest = 1000.0f;
    for (const glm::vec3& p : mesh.positions) {
        lowest = std::min(lowest, p.y + 32.0f);
    }
    QVERIFY(std::abs(lowest - 32.5f) < 0.6f);

    brush.center = glm::vec3(10.0f, 44.0f, 10.0f);
    brush.radius = 3.0f;
    QVERIFY(sculpt(grid, brush, SculptMode::Fill) > 0);
    QVERIFY(grid.get(glm::ivec3(10, 44, 10)).isGround());
    QCOMPARE((int)grid.get(glm::ivec3(10, 44, 10)).material, 2);
    QVERIFY(!grid.get(glm::ivec3(10, 48, 10)).isGround());

    brush.center = glm::vec3(50.0f, 40.5f, 50.0f);
    brush.radius = 4.0f;
    brush.strength = 1.0f;
    QVERIFY(!grid.get(glm::ivec3(50, 41, 50)).isGround());
    QVERIFY(sculpt(grid, brush, SculptMode::Raise) > 0);
    QVERIFY(grid.get(glm::ivec3(50, 41, 50)).isGround());
    // raised ground grows in the ground's own material, not the brush's: the chosen layer left voxel-sized patches
    // of it along the rim of a raise, where the soft edge brought single voxels over (2026-10-18)
    QCOMPARE((int)grid.get(glm::ivec3(50, 41, 50)).material, 1);
    QVERIFY(!grid.get(glm::ivec3(50, 43, 50)).isGround());

    brush.material = 5;
    QVERIFY(sculpt(grid, brush, SculptMode::PaintVoxels) > 0);
    QCOMPARE((int)grid.get(glm::ivec3(50, 40, 50)).material, 5);
    QCOMPARE((int)grid.get(glm::ivec3(50, 43, 50)).material, 1);   // air keeps its material

    // a hill raised in several steps with another material chosen: every new voxel is in the ground's own
    brush.material = 2;
    for (int i = 0; i < 6; ++i) {
        sculpt(grid, brush, SculptMode::Raise);
    }
    int raisedVoxels = 0;
    int inTheBrushMaterial = 0;
    for (int y = 42; y < 50; ++y) {
        for (int x = 44; x <= 56; ++x) {
            for (int z = 44; z <= 56; ++z) {
                const Voxel voxel = grid.get(glm::ivec3(x, y, z));
                if (voxel.isGround()) {
                    ++raisedVoxels;
                    inTheBrushMaterial += voxel.material == 2 ? 1 : 0;
                }
            }
        }
    }
    QVERIFY(raisedVoxels > 20);
    QCOMPARE(inTheBrushMaterial, 0);
}

void TerrainTests::fileRoundTripKeepsEveryVoxel() {
    VoxelGrid grid(glm::ivec3(70, 40, 50));
    QCOMPARE(grid.getChunkCount(), glm::ivec3(3, 2, 2));
    grid.fillHeightField(hills, 1);
    Brush brush;
    brush.center = glm::vec3(20.0f, 10.0f, 20.0f);
    brush.radius = 6.0f;
    sculpt(grid, brush, SculptMode::Dig);
    QVERIFY(grid.countDenseChunks() > 0);
    QVERIFY(grid.countDenseChunks() < 12);

    Bytes bytes = saveTerrain(grid);
    QVERIFY(bytes.size() > 32);
    QVERIFY(bytes.size() < (size_t)(70 * 40 * 50 * 2) / 4);

    VoxelGrid loaded;
    std::string error;
    QVERIFY2(loadTerrain(bytes, loaded, nullptr, nullptr, &error), error.c_str());
    QCOMPARE(loaded.getVoxelCount(), grid.getVoxelCount());
    QCOMPARE(loaded.countDenseChunks(), grid.countDenseChunks());
    for (int z = 0; z < 50; ++z) {
        for (int y = 0; y < 40; ++y) {
            for (int x = 0; x < 70; ++x) {
                QVERIFY(loaded.get(glm::ivec3(x, y, z)) == grid.get(glm::ivec3(x, y, z)));
            }
        }
    }
    QCOMPARE((int)loaded.getDirtyChunks().size(), 12);

    Bytes cut(bytes.begin(), bytes.begin() + bytes.size() / 2);
    QVERIFY(!loadTerrain(cut, loaded, nullptr, nullptr, &error));
    QVERIFY(!error.empty());
    QVERIFY(loaded.isEmpty());

    Bytes wrong = bytes;
    wrong[0] = 'X';
    QVERIFY(!loadTerrain(wrong, loaded, nullptr, nullptr, &error));
}

void TerrainTests::asyncMesherMatchesTheMesherAndRedoesChunksEditedMeanwhile() {
    VoxelGrid grid(glm::ivec3(64, 32, 64));
    grid.fillHeightField(hills);
    const ChunkKeySet dirty = grid.getDirtyChunks();
    QCOMPARE((int)dirty.size(), 4);
    std::map<ChunkKey, ChunkMesh> expected;
    for (const ChunkKey& chunk : dirty) {
        expected[chunk] = meshChunk(grid, chunk);
    }

    QThreadPool pool;
    pool.setMaxThreadCount(3);
    AsyncMesher mesher(&pool);
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(mesher.submit(grid), (int)dirty.size());
    qDebug() << "sampling and sending" << dirty.size() << "chunks took" << timer.nsecsElapsed() / 1000 << "us on this thread";
    QVERIFY(grid.getDirtyChunks().empty());
    QCOMPARE(mesher.getInFlightCount(), dirty.size());
    QVERIFY(!mesher.isIdle());
    QCOMPARE(mesher.submit(grid), 0);
    pool.waitForDone();
    QVERIFY(mesher.hasResults());
    std::vector<AsyncMesher::Result> results = mesher.collect();
    QCOMPARE(results.size(), dirty.size());
    QVERIFY(mesher.isIdle());
    int meshed = 0;
    for (const AsyncMesher::Result& result : results) {
        const ChunkMesh& wanted = expected.at(result.key);
        QVERIFY(result.mesh.positions == wanted.positions);
        QVERIFY(result.mesh.normals == wanted.normals);
        QVERIFY(result.mesh.materials == wanted.materials);
        QVERIFY(result.mesh.indices == wanted.indices);
        if (!result.mesh.isEmpty()) {
            ++meshed;
        }
    }
    QCOMPARE(meshed, 4);

    // A chunk edited while its job is out stays dirty, is not sent again meanwhile, and goes out once its mesh is
    // back; that mesh is the one of the samples taken, the next one of the edit.
    const glm::ivec3 voxel(40, 10, 40);   // inside chunk (1, 0, 1), above the hills there
    QVERIFY(grid.set(voxel, Voxel::ground()));
    QCOMPARE(grid.getDirtyChunks(), ChunkKeySet({ glm::ivec3(1, 0, 1) }));
    QCOMPARE(mesher.submit(grid), 1);
    QVERIFY(grid.set(voxel + glm::ivec3(1, 0, 0), Voxel::ground()));
    QCOMPARE(grid.getDirtyChunks(), ChunkKeySet({ glm::ivec3(1, 0, 1) }));
    QCOMPARE(mesher.submit(grid), 0);
    pool.waitForDone();
    results = mesher.collect();
    QCOMPARE(results.size(), (size_t)1);
    QVERIFY(results[0].key == ChunkKey(glm::ivec3(1, 0, 1)));
    QVERIFY(mesher.isIdle());
    QCOMPARE(grid.getDirtyChunks(), ChunkKeySet({ glm::ivec3(1, 0, 1) }));
    QCOMPARE(mesher.submit(grid), 1);
    pool.waitForDone();
    results = mesher.collect();
    QCOMPARE(results.size(), (size_t)1);
    QVERIFY(results[0].mesh.positions == meshChunk(grid, glm::ivec3(1, 0, 1)).positions);
    QVERIFY(grid.getDirtyChunks().empty());

    // A chunk with no surface in its samples is answered at once, with an empty mesh.
    VoxelGrid solid(glm::ivec3(32, 32, 32), Voxel::ground());
    QCOMPARE(mesher.submit(solid), 1);
    QVERIFY(mesher.hasResults());
    results = mesher.collect();
    QCOMPARE(results.size(), (size_t)1);
    QVERIFY(results[0].mesh.isEmpty());

    // reset() forgets the jobs out: a replaced grid gets none of the old one's meshes.
    QVERIFY(grid.set(voxel + glm::ivec3(0, 1, 0), Voxel::ground()));
    QCOMPARE(mesher.submit(grid), 1);
    mesher.reset();
    QVERIFY(mesher.isIdle());
    pool.waitForDone();
    QVERIFY(!mesher.hasResults());
    QVERIFY(mesher.collect().empty());
}

void TerrainTests::paintMapPaintsErasesTilesAndRoundTripsThroughTheFile() {
    // two texels a voxel over a footprint of 70 by 50 voxels: three columns by two
    PaintMap map(glm::ivec2(70, 50), 2);
    QVERIFY(!map.isEmpty());
    QVERIFY(!map.isPainted());
    QCOMPARE(map.getSize(), glm::ivec2(140, 100));
    QCOMPARE(map.getVoxelFootprint(), glm::ivec2(70, 50));
    QCOMPARE(map.getColumnCount(), glm::ivec2(3, 2));
    QCOMPARE((int)map.getDirtyColumns().size(), 6);
    map.clearDirtyColumns();

    // a hard full-strength brush paints its layer alone, within its radius only
    PaintBrush brush;
    brush.center = glm::vec2(20.0f, 20.0f);
    brush.radius = 6.0f;
    brush.strength = 1.0f;
    brush.hardness = 1.0f;
    brush.layer = 2;
    QVERIFY(paint(map, brush, false) > 0);
    QVERIFY(map.isPainted());
    const uint8_t* centre = map.at(glm::ivec2(40, 40));
    QCOMPARE((int)centre[2], 255);
    QCOMPARE((int)centre[0] + centre[1] + centre[3] + centre[4] + centre[5] + centre[6] + centre[7], 0);
    const uint8_t* outside = map.at(glm::ivec2(80, 40));
    for (int k = 0; k < PAINT_LAYERS; ++k) {
        QCOMPARE((int)outside[k], 0);
    }
    // it lies well inside column (0, 0): that column alone is dirty
    QCOMPARE((int)map.getDirtyColumns().size(), 1);
    QVERIFY(map.getDirtyColumns().count(glm::ivec2(0, 0)));
    map.clearDirtyColumns();

    // half strength of another layer gives that layer half its coverage and takes nothing from the first: each
    // layer's coverage is its own, drawn over the layers below it in the stack's order
    brush.layer = 5;
    brush.strength = 0.5f;
    QVERIFY(paint(map, brush, false) > 0);
    QCOMPARE((int)centre[2], 255);
    QVERIFY(std::abs((int)centre[5] - 128) <= 1);

    // erasing at full strength takes a layer's coverage away on its own; both erased, the voxel material is alone again
    brush.strength = 1.0f;
    QVERIFY(paint(map, brush, true) > 0);
    QCOMPARE((int)centre[5], 0);
    QCOMPARE((int)centre[2], 255);
    brush.layer = 2;
    QVERIFY(paint(map, brush, true) > 0);
    for (int k = 0; k < PAINT_LAYERS; ++k) {
        QCOMPARE((int)centre[k], 0);
    }
    QVERIFY(!map.isPainted());
    map.clearDirtyColumns();

    // a texel within a voxel of the next column lands in that column's apron: both columns' tiles are out of date
    brush.center = glm::vec2(31.75f, 10.25f);   // texel (63, 20): voxel 31, the last of column 0
    brush.radius = 0.4f;
    brush.layer = 0;
    QCOMPARE(paint(map, brush, false), 1);
    QCOMPARE((int)map.at(glm::ivec2(63, 20))[0], 255);
    QCOMPARE((int)map.getDirtyColumns().size(), 2);
    QVERIFY(map.getDirtyColumns().count(glm::ivec2(0, 0)));
    QVERIFY(map.getDirtyColumns().count(glm::ivec2(1, 0)));

    // the tile of column (1, 0): 36 voxels a side at two texels each, its texel (0, 0) at the map's (60, -4)
    QCOMPARE(map.getTileSize(), 72);
    std::vector<uint8_t> low, high;
    map.fillTile(glm::ivec2(1, 0), low, high);
    QCOMPARE((int)low.size(), 72 * 72 * 4);
    QCOMPARE((int)high.size(), 72 * 72 * 4);
    QCOMPARE((int)low[((20 + 4) * 72 + (63 - 60)) * 4 + 0], 255);   // the painted texel, through the apron
    QCOMPARE((int)low[((20 + 4) * 72 + (63 - 60)) * 4 + 1], 0);
    // and the tile of column (0, 0) repeats the map's edge beyond it
    map.at(glm::ivec2(0, 5))[7] = 99;
    map.fillTile(glm::ivec2(0, 0), low, high);
    QCOMPARE((int)high[((5 + 4) * 72 + 0) * 4 + 3], 99);   // tile texel x 0 is map texel -4, clamped to 0
    QCOMPARE((int)high[((5 + 4) * 72 + 4) * 4 + 3], 99);   // tile texel x 4 is map texel 0 itself
    QCOMPARE((int)high[((5 + 4) * 72 + 5) * 4 + 3], 0);
    map.at(glm::ivec2(0, 5))[7] = 0;

    // a block copied out comes back as it was
    brush.center = glm::vec2(20.0f, 20.0f);
    brush.radius = 6.0f;
    brush.layer = 3;
    paint(map, brush, false);
    const PaintBlock block = map.copy(glm::ivec2(20, 20), glm::ivec2(60, 60));
    QCOMPARE(block.low, glm::ivec2(20, 20));
    QCOMPARE(block.size, glm::ivec2(41, 41));
    const std::vector<uint8_t> painted = map.getWeights();
    paint(map, brush, true);
    QVERIFY(map.getWeights() != painted);
    map.paste(block);
    QVERIFY(map.getWeights() == painted);
    QVERIFY(map.copy(glm::ivec2(200, 200), glm::ivec2(300, 300)).isEmpty());

    // the file keeps the map with the grid, and a never-painted map keeps the file at version 1
    VoxelGrid grid(glm::ivec3(70, 40, 50));
    grid.fillHeightField(hills, 1);
    const Bytes withPaint = saveTerrain(grid, &map);
    VoxelGrid loadedGrid;
    PaintMap loadedMap;
    std::string error;
    QVERIFY2(loadTerrain(withPaint, loadedGrid, &loadedMap, nullptr, &error), error.c_str());
    QCOMPARE(loadedGrid.getVoxelCount(), grid.getVoxelCount());
    QCOMPARE(loadedMap.getSize(), map.getSize());
    QCOMPARE(loadedMap.getTexelsPerVoxel(), 2);
    QVERIFY(loadedMap.getWeights() == map.getWeights());
    QCOMPARE((int)loadedMap.getDirtyColumns().size(), 6);

    PaintMap blank(glm::ivec2(70, 50), 2);
    const Bytes withoutPaint = saveTerrain(grid, &blank);
    QVERIFY(withoutPaint.size() < withPaint.size());
    QVERIFY(withoutPaint == saveTerrain(grid));
    QVERIFY2(loadTerrain(withoutPaint, loadedGrid, &loadedMap, nullptr, &error), error.c_str());
    QVERIFY(loadedMap.isEmpty());

    // a map over another footprint is not the grid's: left out of the file
    PaintMap other(glm::ivec2(10, 10), 2);
    other.at(glm::ivec2(1, 1))[0] = 255;
    QVERIFY(saveTerrain(grid, &other) == saveTerrain(grid));

    // a cut file fails cleanly
    Bytes cut(withPaint.begin(), withPaint.begin() + withPaint.size() - 10);
    QVERIFY(!loadTerrain(cut, loadedGrid, &loadedMap, nullptr, &error));
    QVERIFY(loadedGrid.isEmpty());
    QVERIFY(loadedMap.isEmpty());
}

void TerrainTests::resampledGridKeepsTheSurfaceAndItsMaterials() {
    // where a column's distance crosses zero: the height of the surface there
    auto crossing = [](const VoxelGrid& grid, int x, int z) {
        for (int y = 0; y + 1 < grid.getVoxelCount().y; ++y) {
            const float below = grid.get(glm::ivec3(x, y, z)).getDistance();
            const float above = grid.get(glm::ivec3(x, y + 1, z)).getDistance();
            if (below < 0.0f && above >= 0.0f) {
                return y + below / (below - above);
            }
        }
        return -1.0f;
    };
    const int columns[][2] = { { 3, 3 }, { 16, 16 }, { 20, 45 }, { 31, 32 }, { 48, 7 }, { 60, 60 } };

    // hills in the lower half of a 64-voxel cube: the chunks above them hold one value, air at the limit
    VoxelGrid source(glm::ivec3(64, 64, 64));
    source.fillHeightField(hills, 3);
    QCOMPARE(source.countDenseChunks(), 4);
    for (int z = 10; z <= 20; ++z) {
        for (int x = 10; x <= 20; ++x) {
            for (int y = 0; y < 4; ++y) {
                source.set(glm::ivec3(x, y, z), Voxel(source.get(glm::ivec3(x, y, z)).distance, 5));
            }
        }
    }

    // twice the voxels along each side over the same box: the surface lies at twice the height in voxels
    const VoxelGrid fine = resampleGrid(source, glm::ivec3(128, 128, 128));
    QCOMPARE(fine.getVoxelCount(), glm::ivec3(128, 128, 128));
    QCOMPARE(fine.getChunkCount(), glm::ivec3(4, 4, 4));
    QCOMPARE((int)fine.getDirtyChunks().size(), 64);
    for (const auto& column : columns) {
        const float expected = 2.0f * crossing(source, column[0], column[1]);
        QVERIFY(expected > 0.0f);
        QVERIFY(std::abs(crossing(fine, 2 * column[0], 2 * column[1]) - expected) < 0.1f);
    }
    // the chunks around the surface are sampled, those read from the uniform source chunks stay one value
    QVERIFY(!fine.getChunk(glm::ivec3(1, 0, 1)).isUniform());
    QVERIFY(fine.getChunk(glm::ivec3(1, 3, 1)).isUniform());
    QVERIFY(fine.getChunk(glm::ivec3(1, 3, 1)).getUniformValue() == Voxel(DISTANCE_MAX, 3));
    QVERIFY(fine.countDenseChunks() <= 32);
    // far from the surface the distances stay at the byte's limit
    QVERIFY(fine.get(glm::ivec3(5, 127, 5)).distance == DISTANCE_MAX);
    QVERIFY(fine.get(glm::ivec3(5, 0, 5)).distance == -DISTANCE_MAX);
    // a voxel takes the nearest source voxel's material
    QCOMPARE((int)fine.get(glm::ivec3(30, 4, 30)).material, 5);
    QCOMPARE((int)fine.get(glm::ivec3(60, 4, 60)).material, 3);

    // and back down, on one thread: the surface returns to where it was
    const VoxelGrid back = resampleGrid(fine, glm::ivec3(64, 64, 64), 1);
    QCOMPARE(back.getVoxelCount(), source.getVoxelCount());
    for (const auto& column : columns) {
        QVERIFY(std::abs(crossing(back, column[0], column[1]) - crossing(source, column[0], column[1])) < 0.1f);
    }
    QCOMPARE((int)back.get(glm::ivec3(15, 1, 15)).material, 5);

    // an uneven ratio: 64 voxels into 100, the surface at 100/64 of its height
    const VoxelGrid odd = resampleGrid(source, glm::ivec3(100, 100, 100));
    for (int x : { 16, 32, 48 }) {
        const float expected = (100.0f / 64.0f) * crossing(source, x, x);
        QVERIFY(std::abs(crossing(odd, x * 100 / 64, x * 100 / 64) - expected) < 0.15f);
    }

    // ground through and through stays one value a chunk, at the limit
    VoxelGrid solid(glm::ivec3(32, 32, 32), Voxel::ground(2));
    const VoxelGrid solidFine = resampleGrid(solid, glm::ivec3(64, 64, 64));
    QCOMPARE(solidFine.getChunkCount(), glm::ivec3(2, 2, 2));
    QCOMPARE(solidFine.countDenseChunks(), 0);
    QVERIFY(solidFine.get(glm::ivec3(40, 40, 40)) == Voxel::ground(2));

    // the same count gives the same grid; no grid gives none
    const VoxelGrid same = resampleGrid(source, source.getVoxelCount());
    QVERIFY(same.get(glm::ivec3(16, 9, 16)) == source.get(glm::ivec3(16, 9, 16)));
    QVERIFY(resampleGrid(VoxelGrid(), glm::ivec3(32)).isEmpty());
}

void TerrainTests::resampledPaintMapKeepsItsPaint() {
    PaintMap map(glm::ivec2(64, 64), 2);
    PaintBrush brush;
    brush.center = glm::vec2(20.0f, 20.0f);
    brush.radius = 8.0f;
    brush.strength = 1.0f;
    brush.hardness = 1.0f;
    brush.layer = 2;
    QVERIFY(paint(map, brush, false) > 0);
    brush.center = glm::vec2(44.0f, 44.0f);
    brush.radius = 6.0f;
    brush.strength = 0.5f;
    brush.layer = 5;
    QVERIFY(paint(map, brush, false) > 0);
    auto painted = [](const PaintMap& m, int layer) {
        int count = 0;
        for (int j = 0; j < m.getSize().y; ++j) {
            for (int i = 0; i < m.getSize().x; ++i) {
                count += m.at(glm::ivec2(i, j))[layer] > 127 ? 1 : 0;
            }
        }
        return count;
    };

    // the same footprint at four texels a voxel: the same picture, four times the texels across
    const PaintMap fine = resamplePaint(map, glm::ivec2(64, 64), 8);
    QCOMPARE(fine.getSize(), glm::ivec2(512, 512));
    QCOMPARE(fine.getTexelsPerVoxel(), 8);
    QCOMPARE((int)fine.getDirtyColumns().size(), 4);
    QVERIFY(fine.isPainted());
    QCOMPARE((int)fine.at(glm::ivec2(160, 160))[2], 255);
    QVERIFY(std::abs((int)fine.at(glm::ivec2(352, 352))[5] - 128) <= 1);
    for (int k = 0; k < PAINT_LAYERS; ++k) {
        QCOMPARE((int)fine.at(glm::ivec2(400, 100))[k], 0);
    }
    const float grown = (float)painted(fine, 2) / (float)painted(map, 2);
    QVERIFY(grown > 14.5f && grown < 17.5f);   // sixteen times the texels, give or take the edge
    for (int j = 0; j < fine.getSize().y; ++j) {
        for (int i = 0; i < fine.getSize().x; ++i) {
            int sum = 0;
            for (int k = 0; k < PAINT_LAYERS; ++k) {
                sum += fine.at(glm::ivec2(i, j))[k];
            }
            QVERIFY(sum <= 255);
        }
    }

    // another footprint (the terrain resampled to more voxels) at the texel count the editor would pick
    const PaintMap moved = resamplePaint(map, glm::ivec2(100, 100), 3);
    QCOMPARE(moved.getSize(), glm::ivec2(300, 300));
    QCOMPARE((int)moved.at(glm::ivec2(93, 93))[2], 255);   // voxel (20, 20) of 64 is voxel (31.25, 31.25) of 100

    // an unpainted map gives an unpainted one of the size asked
    const PaintMap blank = resamplePaint(PaintMap(glm::ivec2(64, 64), 2), glm::ivec2(32, 32), 1);
    QCOMPARE(blank.getSize(), glm::ivec2(32, 32));
    QVERIFY(!blank.isPainted());
}

// --- chunk levels -------------------------------------------------------------------------------

namespace {

// Where a column's field crosses zero between two heights, read between the voxels: the surface's height there.
float surfaceHeight(const VoxelGrid& grid, float x, float z, float from, float to, float step = 0.25f) {
    float below = grid.fieldAt(glm::vec3(x, from, z)).distance;
    for (float y = from + step; y <= to + 1.0e-4f; y += step) {
        const float above = grid.fieldAt(glm::vec3(x, y, z)).distance;
        if (below < 0.0f && above >= 0.0f) {
            return y - step + step * below / (below - above);
        }
        below = above;
    }
    return -1.0f;
}

// Every chunk touching another differs from it by at most one level.
bool twoToOneRuleHolds(const VoxelGrid& grid) {
    bool holds = true;
    grid.forEachChunk([&](const ChunkKey& key, const Chunk&) {
        const glm::ivec3 origin = key.getOrigin();
        for (const ChunkKey& neighbour : grid.chunksCovering(origin - 1, origin + key.getSpan())) {
            if (std::abs(neighbour.level - key.level) > 1) {
                holds = false;
            }
        }
    });
    return holds;
}

// Vertices of different chunks' meshes, in grid units, told apart: two within a hair of each other are one
// vertex (each chunk places its own in its own units, so the same vertex comes out a rounding apart).
class VertexMerger {
public:
    int idOf(const glm::vec3& p) {
        const glm::ivec3 q(glm::floor(p * 16.0f));
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    auto found = _cells.find(std::make_tuple(q.x + dx, q.y + dy, q.z + dz));
                    if (found == _cells.end()) {
                        continue;
                    }
                    for (const auto& entry : found->second) {
                        if (near(entry.first, p, 1.0e-4f)) {
                            return entry.second;
                        }
                    }
                }
            }
        }
        _cells[std::make_tuple(q.x, q.y, q.z)].push_back(std::make_pair(p, _next));
        _positions.push_back(p);
        return _next++;
    }
    const glm::vec3& positionOf(int id) const { return _positions[id]; }

private:
    std::map<std::tuple<int, int, int>, std::vector<std::pair<glm::vec3, int>>> _cells;
    std::vector<glm::vec3> _positions;
    int _next { 0 };
};

using KeyedMeshes = std::vector<std::pair<ChunkKey, ChunkMesh>>;

// Counts the edges of the surface some meshes make together that lie on an odd number of triangles: an open edge,
// a crack between chunks, or a hole. Each mesh is in its chunk's (or node's) own voxels. Edges along the grid's
// sides are left out, within `side` grid voxels of them: the surface runs out of the grid open there (a coarse
// node's ends up to a voxel of its own short of the grid's last voxel).
int countOpenEdges(const VoxelGrid& grid, const KeyedMeshes& meshes, int* triangles = nullptr, float side = 0.0f,
                   std::vector<std::pair<glm::vec3, glm::vec3>>* openEdges = nullptr) {
    VertexMerger merger;
    std::map<std::pair<int, int>, int> edgeCounts;
    int triangleCount = 0;
    for (const auto& pair : meshes) {
        const ChunkKey& key = pair.first;
        const ChunkMesh& mesh = pair.second;
        std::vector<int> ids;
        for (const glm::vec3& p : mesh.positions) {
            ids.push_back(merger.idOf(glm::vec3(key.getOrigin()) + p * key.getVoxelSize()));
        }
        for (size_t t = 0; t < mesh.getTriangleCount(); ++t) {
            ++triangleCount;
            for (int k = 0; k < 3; ++k) {
                const int a = ids[mesh.indices[3 * t + k]];
                const int b = ids[mesh.indices[3 * t + (k + 1) % 3]];
                if (a != b) {
                    ++edgeCounts[std::make_pair(std::min(a, b), std::max(a, b))];
                }
            }
        }
    }
    if (triangles) {
        *triangles = triangleCount;
    }
    const glm::vec3 high = glm::vec3(grid.getVoxelCount() - 1) - side;
    int open = 0;
    for (const auto& entry : edgeCounts) {
        if (entry.second % 2 == 0) {
            continue;
        }
        const glm::vec3& a = merger.positionOf(entry.first.first);
        const glm::vec3& b = merger.positionOf(entry.first.second);
        bool onSide = false;
        for (int axis = 0; axis < 3; ++axis) {
            if ((a[axis] <= side && b[axis] <= side) || (a[axis] >= high[axis] && b[axis] >= high[axis])) {
                onSide = true;
            }
        }
        if (!onSide) {
            ++open;
            if (openEdges) {
                openEdges->emplace_back(a, b);
            }
        }
    }
    return open;
}

// Meshes every chunk of the grid and counts the open edges of the surface they make together.
int countOpenEdges(const VoxelGrid& grid, int* triangles = nullptr) {
    KeyedMeshes meshes;
    grid.forEachChunk([&](const ChunkKey& key, const Chunk&) { meshes.emplace_back(key, meshChunk(grid, key)); });
    return countOpenEdges(grid, meshes, triangles);
}

} // namespace

void TerrainTests::chunkKeysNameTheirGridVoxels() {
    const ChunkKey coarse(0, glm::ivec3(1, 2, 3));
    QCOMPARE(coarse.getScale(), 1);
    QCOMPARE(coarse.getSpan(), 32);
    QCOMPARE(coarse.getOrigin(), glm::ivec3(32, 64, 96));
    QCOMPARE(coarse.getSlot(), glm::ivec3(1, 2, 3));
    const ChunkKey child = coarse.getChild(5);   // bits: x and z
    QCOMPARE(child.level, 1);
    QCOMPARE(child.coords, glm::ivec3(3, 4, 7));
    QCOMPARE(child.getSpan(), 16);
    QCOMPARE(child.getOrigin(), glm::ivec3(48, 64, 112));
    QCOMPARE(child.getSlot(), glm::ivec3(1, 2, 3));
    QVERIFY(child.getParent() == coarse);
    const ChunkKey grandchild = child.getChild(7);
    QCOMPARE(grandchild.level, 2);
    QCOMPARE(grandchild.getSpan(), 8);
    QCOMPARE(grandchild.getOrigin(), glm::ivec3(56, 72, 120));
    QVERIFY(grandchild.intersects(glm::ivec3(60, 72, 120), glm::ivec3(60, 72, 120)));
    QVERIFY(grandchild.intersects(glm::ivec3(0), glm::ivec3(56, 79, 127)));
    QVERIFY(!grandchild.intersects(glm::ivec3(64, 72, 120), glm::ivec3(70, 80, 130)));
    QVERIFY(ChunkKey(0, 1, 0, 0) < ChunkKey(1, 0, 0, 0));
    QVERIFY(ChunkKey(1, 0, 0, 0) < ChunkKey(1, 0, 0, 1));
    QVERIFY(ChunkKey(glm::ivec3(2, 0, 0)) == ChunkKey(0, 2, 0, 0));

    VoxelGrid grid(glm::ivec3(70, 40, 50));
    QVERIFY(grid.containsChunk(ChunkKey(0, 2, 1, 1)));
    QVERIFY(!grid.containsChunk(ChunkKey(0, 3, 1, 1)));
    QVERIFY(grid.containsChunk(ChunkKey(2, 11, 7, 7)));    // slot (2, 1, 1)
    QVERIFY(!grid.containsChunk(ChunkKey(2, 12, 7, 7)));   // slot (3, 1, 1)
    QVERIFY(!grid.containsChunk(ChunkKey(MAX_CHUNK_LEVEL + 1, 0, 0, 0)));
    QVERIFY(grid.hasChunk(ChunkKey(0, 2, 1, 1)));
    QVERIFY(!grid.hasChunk(ChunkKey(1, 4, 2, 2)));
    QVERIFY(grid.chunkCovering(glm::ivec3(69, 39, 49)) == ChunkKey(0, 2, 1, 1));
    QVERIFY(grid.chunkCovering(glm::ivec3(-5, 100, 0)) == ChunkKey(0, 0, 1, 0));
    QCOMPARE((int)grid.chunksCovering(glm::ivec3(0), glm::ivec3(69, 39, 49)).size(), 12);
    QCOMPARE((int)grid.chunksCovering(glm::ivec3(31, 0, 0), glm::ivec3(32, 0, 0)).size(), 2);
    QCOMPARE((int)grid.chunksCovering(glm::ivec3(80, 0, 0), glm::ivec3(90, 0, 0)).size(), 1);   // the last chunks reach to 96
    QVERIFY(grid.chunksCovering(glm::ivec3(96, 0, 0), glm::ivec3(99, 0, 0)).empty());
    QCOMPARE(grid.countChunks(), 12);
    QCOMPARE(grid.getFinestLevel(), 0);
    QCOMPARE(grid.getMaxLevel(), MAX_CHUNK_LEVEL);
}

void TerrainTests::splitChunkKeepsTheGroundAndWhatTheNeighboursSample() {
    VoxelGrid grid(glm::ivec3(64, 32, 64));
    grid.fillHeightField(hills, 2);
    const VoxelGrid before = grid;
    const ChunkKey parent(0, 1, 0, 1);
    ChunkSamples neighbourBefore;
    grid.sampleChunk(ChunkKey(0, 0, 0, 1), neighbourBefore);
    const uint64_t structure = grid.getStructureVersion();
    grid.clearDirtyChunks();

    QVERIFY(grid.splitChunk(parent));
    QVERIFY(grid.getStructureVersion() > structure);
    QVERIFY(!grid.hasChunk(parent));
    QVERIFY(!grid.findChunk(parent));
    QCOMPARE(grid.countChunks(), 11);
    QCOMPARE(grid.getFinestLevel(), 1);
    for (int i = 0; i < 8; ++i) {
        QVERIFY(grid.hasChunk(parent.getChild(i)));
        QVERIFY(grid.getDirtyChunks().count(parent.getChild(i)) > 0);
    }
    // the chunks that sample the split one are dirty too, and nothing dirty is a chunk that is gone
    QCOMPARE((int)grid.getDirtyChunks().size(), 11);
    for (const ChunkKey& key : grid.getDirtyChunks()) {
        QVERIFY(grid.hasChunk(key));
    }
    QVERIFY(grid.chunkCovering(glm::ivec3(40, 10, 40)) == ChunkKey(1, 2, 0, 2));
    QVERIFY(grid.chunkCovering(glm::ivec3(50, 10, 40)) == ChunkKey(1, 3, 0, 2));
    QVERIFY(grid.chunkCovering(glm::ivec3(10, 10, 40)) == ChunkKey(0, 0, 0, 1));
    QCOMPARE((int)grid.chunksCovering(glm::ivec3(31, 0, 31), glm::ivec3(48, 0, 48)).size(), 7);
    QVERIFY(!grid.splitChunk(parent));   // gone
    QVERIFY(twoToOneRuleHolds(grid));

    // The ground is where it was: at a child's even voxels the parent's, between them the field between.
    const Chunk& child = grid.getChunk(ChunkKey(1, 2, 0, 2));
    QVERIFY(!child.isUniform());
    for (int z = 0; z < 32; z += 5) {
        for (int y = 0; y < 32; y += 3) {
            for (int x = 0; x < 32; x += 7) {
                const Voxel& fine = child.get(x, y, z);
                const glm::vec3 position(32.0f + x * 0.5f, y * 0.5f, 32.0f + z * 0.5f);
                const VoxelGrid::FieldSample field = before.fieldAt(position);
                if (std::abs(field.distance) < 1.9f) {
                    QVERIFY(std::abs(fine.getDistance() - 2.0f * field.distance) < 0.05f);
                } else {
                    QCOMPARE(fine.isGround(), field.distance < 0.0f);
                }
                QCOMPARE((int)fine.material, 2);
                // and the grid reads the same back, at its own points and at the fine ones
                QVERIFY(grid.sampleAt(1, glm::ivec3(64 + x, y, 64 + z)) == fine);
                if (std::abs(field.distance) < 1.9f) {
                    QVERIFY(std::abs(grid.fieldAt(position).distance - field.distance) < 0.03f);
                }
            }
        }
    }
    for (int x = 33; x < 63; x += 6) {
        QVERIFY(std::abs(surfaceHeight(grid, (float)x, 40.0f, 2.0f, 20.0f) - surfaceHeight(before, (float)x, 40.0f, 2.0f, 20.0f)) < 0.05f);
        QVERIFY(std::abs(surfaceHeight(grid, x + 0.5f, 40.5f, 2.0f, 20.0f) - surfaceHeight(before, x + 0.5f, 40.5f, 2.0f, 20.0f)) < 0.05f);
    }
    // The level-0 neighbour samples the same ground across the join as before, where the byte can say so.
    ChunkSamples neighbourAfter;
    grid.sampleChunk(ChunkKey(0, 0, 0, 1), neighbourAfter);
    int compared = 0;
    for (int z = -1; z <= 32; ++z) {
        for (int y = -1; y <= 32; ++y) {
            const Voxel& was = neighbourBefore.at(32, y, z);
            const Voxel& is = neighbourAfter.at(32, y, z);
            if (std::abs(was.getDistance()) < 1.9f) {
                QVERIFY(was == is);
                ++compared;
            } else {
                QCOMPARE(was.isGround(), is.isGround());
            }
        }
    }
    QVERIFY(compared > 50);
    // and the fine chunk's samples of the coarse neighbour lie between its voxels
    ChunkSamples fineSamples;
    grid.sampleChunk(ChunkKey(1, 2, 0, 2), fineSamples);
    for (int y = 0; y < 32; y += 2) {
        const float coarse0 = before.get(glm::ivec3(31, y / 2, 40)).getDistance();
        const float coarse1 = before.get(glm::ivec3(32, y / 2, 40)).getDistance();
        if (std::abs(coarse0) < 1.9f && std::abs(coarse1) < 1.9f) {
            QVERIFY(std::abs(fineSamples.at(-1, y, 16).getDistance() - (coarse0 + coarse1)) < 0.05f);
            QVERIFY(std::abs(fineSamples.at(0, y, 16).getDistance() - 2.0f * coarse1) < 0.05f);
        }
    }
    // the fine chunk meshes in its own voxels (half grid voxels, from its origin), over the same ground, reaching
    // two of its voxels into the coarser neighbours on its low sides (their cells along the join are its to draw)
    ChunkMesh mesh = meshChunk(grid, ChunkKey(1, 2, 0, 2));
    QVERIFY(mesh.getTriangleCount() > 1000);
    for (const glm::vec3& p : mesh.positions) {
        QVERIFY(p.x >= -2.0f && p.x <= 32.0f && p.z >= -2.0f && p.z <= 32.0f);
        const float groundHeight = hills((int)std::round(32.0f + p.x * 0.5f), (int)std::round(32.0f + p.z * 0.5f));
        QVERIFY(std::abs(p.y * 0.5f - groundHeight) < 1.5f);
    }

    // a grid voxel in the split region reads and sets the fine chunk's voxel there
    QVERIFY(grid.get(glm::ivec3(40, 10, 40)) == child.get(16, 20, 16));
    QVERIFY(grid.set(glm::ivec3(40, 10, 40), Voxel::ground(7)));
    QVERIFY(grid.getChunk(ChunkKey(1, 2, 0, 2)).get(16, 20, 16) == Voxel::ground(7));
}

void TerrainTests::refineKeepsTheTwoToOneRule() {
    VoxelGrid grid(glm::ivec3(96, 96, 96));
    grid.fillBelow(50.0f);
    QCOMPARE(grid.countChunks(), 27);

    // A box inside the middle chunk, at its far corner: the level-2 chunk there touches seven other level-0 chunks,
    // which must come to level 1 for it.
    grid.refine(glm::ivec3(60), glm::ivec3(62), 2);
    const std::vector<ChunkKey> covering = grid.chunksCovering(glm::ivec3(60), glm::ivec3(62));
    QCOMPARE((int)covering.size(), 1);
    QVERIFY(covering[0] == ChunkKey(2, 7, 7, 7));
    QVERIFY(grid.hasChunk(ChunkKey(2, 7, 7, 7)));
    QVERIFY(grid.hasChunk(ChunkKey(1, 2, 2, 2)));   // another child of the middle chunk, not split further
    QVERIFY(grid.hasChunk(ChunkKey(1, 4, 2, 2)));   // the chunk beyond, split once for the rule
    QVERIFY(grid.hasChunk(ChunkKey(0, 0, 0, 0)));   // far away, untouched
    QVERIFY(grid.hasChunk(ChunkKey(0, 0, 1, 1)));   // beside the middle chunk on its low side, untouched
    QCOMPARE(grid.countChunks(), 90);
    QVERIFY(twoToOneRuleHolds(grid));
    QCOMPARE(grid.getFinestLevel(), 2);
    // every chunk's mesh is to be remade: the split ones and those that sample them
    for (const ChunkKey& key : grid.getDirtyChunks()) {
        QVERIFY(grid.hasChunk(key));
    }

    // Refining what is fine already changes nothing.
    const uint64_t structure = grid.getStructureVersion();
    grid.refine(glm::ivec3(60), glm::ivec3(62), 2);
    grid.refine(glm::ivec3(60), glm::ivec3(62), 1);
    QCOMPARE(grid.getStructureVersion(), structure);
    QCOMPARE(grid.countChunks(), 90);

    // The grid's own finest level caps the splitting.
    VoxelGrid capped(glm::ivec3(32, 32, 32));
    capped.setMaxLevel(1);
    capped.refine(glm::ivec3(0), glm::ivec3(3), 2);
    QCOMPARE(capped.getFinestLevel(), 1);
    QCOMPARE(capped.countChunks(), 8);
    capped.setMaxLevel(0);
    capped.refine(glm::ivec3(20), glm::ivec3(23), 2);
    QCOMPARE(capped.countChunks(), 8);
    QVERIFY(twoToOneRuleHolds(capped));
    // the whole grid refined to level 2: sixty-four chunks a level-0 chunk
    VoxelGrid whole(glm::ivec3(64, 32, 32));
    whole.refine(glm::ivec3(0), glm::ivec3(63, 31, 31), 2);
    QCOMPARE(whole.countChunks(), 128);
    QCOMPARE(whole.countDenseChunks(), 0);
}

void TerrainTests::smallBrushDigsFineDetailWithoutResamplingTheGrid() {
    VoxelGrid grid(glm::ivec3(96, 96, 96));
    grid.fillBelow(40.5f, 1);
    grid.clearDirtyChunks();
    QCOMPARE(grid.countChunks(), 27);

    // In chunk (1, 1, 1), toward its low corner: one of its eight children is split for the brush, and the
    // level-2 chunks there touch the seven level-0 chunks at that corner, which come to level 1 for them.
    Brush small;
    small.center = glm::vec3(40.0f, 40.5f, 40.0f);
    small.radius = 1.5f;
    QCOMPARE(brushLevel(grid, small), 2);
    Brush big = small;
    big.radius = 8.0f;
    QCOMPARE(brushLevel(grid, big), 0);
    big.radius = 2.5f;
    QCOMPARE(brushLevel(grid, big), 1);
    grid.setMaxLevel(1);
    QCOMPARE(brushLevel(grid, small), 1);
    grid.setMaxLevel(MAX_CHUNK_LEVEL);

    // A small dig: the chunks under the brush go to level 2 and the hole is round at that level.
    const uint64_t structure = grid.getStructureVersion();
    const int changed = sculpt(grid, small, SculptMode::Dig);
    QVERIFY(changed > 0);
    QVERIFY(grid.getStructureVersion() > structure);
    QVERIFY(grid.chunkCovering(glm::ivec3(40, 40, 40)).level == 2);
    QVERIFY(grid.chunkCovering(glm::ivec3(39, 39, 39)).level == 2);
    QVERIFY(grid.chunkCovering(glm::ivec3(45, 40, 40)).level == 2);
    QVERIFY(grid.chunkCovering(glm::ivec3(50, 40, 40)).level == 1);   // another child of chunk (1, 1, 1), not reached
    QVERIFY(grid.chunkCovering(glm::ivec3(5, 10, 5)).level == 1);     // chunk (0, 0, 0): split once for the 2:1 rule
    QVERIFY(grid.chunkCovering(glm::ivec3(80, 10, 80)).level == 0);   // beyond the rule's reach
    QVERIFY(grid.chunkCovering(glm::ivec3(80, 40, 40)).level == 0);
    QCOMPARE(grid.countChunks(), 90);
    QVERIFY(twoToOneRuleHolds(grid));
    QCOMPARE(grid.getVoxelCount(), glm::ivec3(96, 96, 96));   // the grid itself is as it was
    for (const ChunkKey& key : grid.getDirtyChunks()) {
        QVERIFY(grid.hasChunk(key));
    }
    // inside the sphere, below the old surface: air; below the sphere: ground; the floor at the sphere's bottom
    QVERIFY(grid.fieldAt(glm::vec3(40.0f, 39.5f, 40.0f)).distance > 0.0f);
    QVERIFY(grid.fieldAt(glm::vec3(40.0f, 38.5f, 40.0f)).distance < 0.0f);
    QVERIFY(std::abs(surfaceHeight(grid, 40.0f, 40.0f, 37.0f, 40.0f) - 39.0f) < 0.15f);
    // the hole is round: the floor rises toward the rim as a sphere's does
    QVERIFY(std::abs(surfaceHeight(grid, 41.0f, 40.0f, 37.0f, 40.0f) - (40.5f - std::sqrt(1.5f * 1.5f - 1.0f))) < 0.2f);
    QVERIFY(std::abs(surfaceHeight(grid, 42.0f, 40.0f, 37.0f, 41.0f) - 40.5f) < 0.1f);   // outside the hole
    // the fine chunk's mesh has the hole's floor, in quarter voxels (the centre sits on eight chunks' corner)
    const ChunkKey fine = grid.chunkCovering(glm::ivec3(40, 39, 40));
    ChunkMesh mesh = meshChunk(grid, fine);
    QVERIFY(!mesh.isEmpty());
    float lowest = 1000.0f;
    for (const glm::vec3& p : mesh.positions) {
        lowest = std::min(lowest, (float)fine.getOrigin().y + p.y / (float)fine.getScale());
    }
    QVERIFY(std::abs(lowest - 39.0f) < 0.3f);

    // A big brush stays coarse where the ground is coarse and works the fine chunks finely.
    Brush wide;
    wide.center = glm::vec3(80.0f, 40.5f, 80.0f);
    wide.radius = 6.0f;
    const int chunksBefore = grid.countChunks();
    QVERIFY(sculpt(grid, wide, SculptMode::Dig) > 0);
    QCOMPARE(grid.countChunks(), chunksBefore);
    QVERIFY(grid.chunkCovering(glm::ivec3(80, 40, 80)).level == 0);
    wide.center = glm::vec3(44.0f, 40.5f, 44.0f);   // over the fine chunks: worked in their voxels, split no further
    QVERIFY(sculpt(grid, wide, SculptMode::Dig) > 0);
    QCOMPARE(grid.countChunks(), chunksBefore);
    QVERIFY(std::abs(surfaceHeight(grid, 44.0f, 44.0f, 32.0f, 40.0f) - 34.5f) < 0.15f);
    // another step of the small brush splits nothing more
    const uint64_t refined = grid.getStructureVersion();
    small.center.y -= 0.5f;
    QVERIFY(sculpt(grid, small, SculptMode::Dig) > 0);
    QCOMPARE(grid.getStructureVersion(), refined);

    // Raise lifts by the same height whatever the level: a step of strength 1 is one grid voxel.
    VoxelGrid lifted(glm::ivec3(64, 64, 64));
    lifted.fillBelow(40.5f, 1);
    Brush raise;
    raise.center = glm::vec3(32.0f, 40.5f, 32.0f);
    raise.radius = 1.5f;
    raise.hardness = 1.0f;
    raise.strength = 1.0f;
    QVERIFY(sculpt(lifted, raise, SculptMode::Raise) > 0);
    QVERIFY(std::abs(surfaceHeight(lifted, 32.0f, 32.0f, 39.0f, 44.0f) - 41.5f) < 0.1f);
    Brush coarseRaise = raise;
    coarseRaise.center = glm::vec3(10.0f, 40.5f, 10.0f);
    coarseRaise.radius = 8.0f;
    QVERIFY(sculpt(lifted, coarseRaise, SculptMode::Raise) > 0);
    QVERIFY(std::abs(surfaceHeight(lifted, 10.0f, 10.0f, 39.0f, 44.0f) - 41.5f) < 0.1f);
}

void TerrainTests::chunksOfDifferentLevelsMeetWithoutACrack() {
    // Hills over nine chunks, all at level 0: the surface is closed but for the grid's sides.
    VoxelGrid grid(glm::ivec3(96, 32, 96));
    grid.fillHeightField(hills, 2);
    int triangles = 0;
    QCOMPARE(countOpenEdges(grid, &triangles), 0);
    QVERIFY(triangles > 9 * 2000);

    // The middle chunk split: level 1 beside 0 across faces, edges and corners, in every orientation.
    QVERIFY(grid.splitChunk(ChunkKey(0, 1, 0, 1)));
    QCOMPARE(grid.countChunks(), 16);
    QCOMPARE(countOpenEdges(grid, &triangles), 0);
    QVERIFY(triangles > 9 * 2000);

    // What the samples say about the neighbours: the child at the split chunk's low corner has level-0 chunks
    // below it in x and z and at that edge, nothing below in y (the grid ends there, and samples beyond it read
    // its bottom row: the chunk's own, or across the low x face the level-0 neighbour's), and its blocks hold
    // those chunks' own voxels.
    ChunkSamples samples;
    grid.sampleChunk(ChunkKey(1, 2, 0, 2), samples);   // origin (32, 0, 32)
    QCOMPARE(samples.getNeighbourLevel(1), -1);
    QCOMPARE(samples.getNeighbourLevel(4), -1);
    QCOMPARE(samples.getNeighbourLevel(5), -1);
    QCOMPARE(samples.getNeighbourLevel(2), 0);
    QCOMPARE(samples.getNeighbourLevel(3), -1);
    QCOMPARE(samples.getNeighbourLevel(6), -1);
    QCOMPARE((int)samples.getBlocks().size(), 5);
    for (const SampleBlock& block : samples.getBlocks()) {
        QCOMPARE(block.levelDelta, -1);
    }
    QCOMPARE(samples.getBlocks()[0].size, glm::ivec3(2, 17, 17));
    QCOMPARE(samples.getBlocks()[1].size, glm::ivec3(2, 2, 17));
    const Voxel* corner = samples.findAt(-1, glm::ivec3(-1, 5, 3));
    QVERIFY(corner);
    QVERIFY(*corner == grid.get(glm::ivec3(31, 5, 35)));
    QVERIFY(!samples.findAt(-1, glm::ivec3(-2, 5, 3)));
    QVERIFY(!samples.findAt(1, glm::ivec3(-1, 5, 3)));
    // and the level-0 chunk beyond the split one in x sees level-1 chunks below it in x: their own voxels at
    // its face, and between its own at the face
    grid.sampleChunk(ChunkKey(0, 2, 0, 1), samples);   // origin (64, 0, 32)
    QCOMPARE(samples.getNeighbourLevel(1), 1);
    QCOMPARE(samples.getNeighbourLevel(3), 1);   // beyond the grid's bottom: the low x neighbours' bottom row again
    QCOMPARE(samples.getNeighbourLevel(4), 0);
    QCOMPARE(samples.getNeighbourLevel(5), 0);
    QCOMPARE((int)samples.getBlocks().size(), 2);
    QCOMPARE(samples.getBlocks()[0].size, glm::ivec3(2, 65, 65));
    QCOMPARE(samples.getBlocks()[1].size, glm::ivec3(2, 2, 65));
    const Voxel* fine = samples.findAt(1, glm::ivec3(-1, 7, 9));
    QVERIFY(fine);
    QVERIFY(*fine == grid.getChunk(ChunkKey(1, 3, 0, 2)).get(31, 7, 9));
    QVERIFY(samples.findAt(1, glm::ivec3(0, 7, 64)));
    QVERIFY(!samples.findAt(1, glm::ivec3(1, 7, 9)));
    // a resampling of the chunk alone asks for none of this
    grid.sampleChunk(ChunkKey(0, 2, 0, 1), samples, false);
    QCOMPARE(samples.getNeighbourLevel(1), 0);
    QVERIFY(samples.getBlocks().empty());

    // The child at the split chunk's low corner split again: level 2 beside 1 beside 0, the level-0 chunks at
    // that corner split once for the 2:1 rule.
    QVERIFY(grid.splitChunk(ChunkKey(1, 2, 0, 2)));
    QVERIFY(twoToOneRuleHolds(grid));
    QCOMPARE(grid.getFinestLevel(), 2);
    QVERIFY(grid.countChunks() > 40);
    QCOMPARE(countOpenEdges(grid, &triangles), 0);
    QVERIFY(triangles > 9 * 2000);

    // A bump smaller than a coarse voxel on the plane between a fine chunk and the coarse one below it in x:
    // the fine chunk's own voxel there, which the coarse chunk's cell never sees. The fine chunk closes the
    // surface around it to that cell's centre.
    VoxelGrid bumped(glm::ivec3(96, 32, 96));
    bumped.fillBelow(10.5f, 1);
    QVERIFY(bumped.splitChunk(ChunkKey(0, 1, 0, 1)));
    const ChunkKey fineKey(1, 2, 0, 2);   // origin (32, 0, 32); its low x neighbour is level-0 chunk (0, 0, 1)
    Chunk* fineChunk = bumped.findChunk(fineKey);
    QVERIFY(fineChunk);
    QVERIFY(fineChunk->set(0, 29, 15, Voxel::ground(1)));
    bumped.voxelsChanged(fineKey, glm::ivec3(0, 29, 15), glm::ivec3(0, 29, 15));
    QVERIFY(bumped.getDirtyChunks().count(ChunkKey(0, 0, 0, 1)) > 0);
    QCOMPARE(countOpenEdges(bumped), 0);
    ChunkMesh fineMesh = meshChunk(bumped, fineKey);
    bool centred = false;
    for (const glm::vec3& p : fineMesh.positions) {
        centred = centred || near(p, glm::vec3(-1.0f, 29.0f, 15.0f), 1.0e-4f);
    }
    QVERIFY(centred);

    // The small brush's dig of smallBrushDigsFineDetailWithoutResamplingTheGrid: ninety chunks of three levels,
    // closed; then a wide brush over the fine ones and the coarse ones beside them, closed still.
    VoxelGrid dug(glm::ivec3(96, 96, 96));
    dug.fillBelow(40.5f, 1);
    Brush small;
    small.center = glm::vec3(40.0f, 40.5f, 40.0f);
    small.radius = 1.5f;
    QVERIFY(sculpt(dug, small, SculptMode::Dig) > 0);
    QCOMPARE(dug.countChunks(), 90);
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(countOpenEdges(dug, &triangles), 0);
    qDebug() << "meshed" << dug.countChunks() << "chunks of three levels," << triangles << "triangles, in" << timer.elapsed() << "ms";
    Brush wide;
    wide.center = glm::vec3(48.0f, 40.5f, 48.0f);
    wide.radius = 6.0f;
    QVERIFY(sculpt(dug, wide, SculptMode::Dig) > 0);
    QVERIFY(sculpt(dug, wide, SculptMode::Raise) > 0);
    QVERIFY(twoToOneRuleHolds(dug));
    QCOMPARE(countOpenEdges(dug), 0);
}

void TerrainTests::setChunkAndPlaceChunkReplaceWhatCoversTheKey() {
    VoxelGrid grid(glm::ivec3(32, 32, 32));
    grid.fillBelow(10.3f, 2);
    grid.clearDirtyChunks();

    // A finer chunk into a coarser one's place: the rest of the place is the old ground, resampled.
    grid.setChunk(ChunkKey(1, 1, 1, 1), Chunk(Voxel::ground(4)));
    QVERIFY(grid.hasChunk(ChunkKey(1, 1, 1, 1)));
    QVERIFY(grid.hasChunk(ChunkKey(1, 0, 0, 0)));
    QVERIFY(!grid.hasChunk(ChunkKey(0, 0, 0, 0)));
    QCOMPARE(grid.countChunks(), 8);
    QVERIFY(grid.get(glm::ivec3(20, 20, 20)) == Voxel::ground(4));
    QVERIFY(std::abs(surfaceHeight(grid, 5.0f, 5.0f, 5.0f, 15.0f) - 10.3f) < 0.05f);
    QVERIFY(std::abs(surfaceHeight(grid, 20.0f, 5.0f, 5.0f, 15.0f) - 10.3f) < 0.05f);   // child (1, 0, 0): the surface, resampled
    QCOMPARE((int)grid.getDirtyChunks().size(), 8);
    for (const ChunkKey& key : grid.getDirtyChunks()) {
        QVERIFY(grid.hasChunk(key));
    }

    // A coarser chunk over finer ones: they go.
    grid.clearDirtyChunks();
    grid.setChunk(ChunkKey(0, 0, 0, 0), Chunk(Voxel::air()));
    QCOMPARE(grid.countChunks(), 1);
    QVERIFY(grid.hasChunk(ChunkKey(0, 0, 0, 0)));
    QVERIFY(!grid.hasChunk(ChunkKey(1, 1, 1, 1)));
    QCOMPARE(grid.getDirtyChunks(), ChunkKeySet({ ChunkKey(0, 0, 0, 0) }));
    QVERIFY(grid.get(glm::ivec3(5, 5, 5)) == Voxel::air());

    // placeChunk fills the rest of a split place with the old chunk's first voxel instead: for sets that cover it.
    grid.fillBelow(10.3f, 2);
    grid.placeChunk(ChunkKey(2, 0, 0, 0), Chunk(Voxel::ground(6)));
    QCOMPARE(grid.countChunks(), 15);
    QVERIFY(grid.hasChunk(ChunkKey(2, 0, 0, 0)));
    QVERIFY(grid.getChunk(ChunkKey(2, 1, 0, 0)).isUniform());
    QVERIFY(grid.getChunk(ChunkKey(1, 1, 0, 0)).isUniform());
    QVERIFY(grid.getChunk(ChunkKey(1, 1, 0, 0)).getUniformValue() == grid.getChunk(ChunkKey(2, 1, 0, 0)).getUniformValue());
    for (int i = 1; i < 8; ++i) {
        grid.placeChunk(ChunkKey(2, 0, 0, 0).getParent().getChild(i), Chunk(Voxel::ground(6)));
        grid.placeChunk(ChunkKey(1, 0, 0, 0).getParent().getChild(i), Chunk(Voxel::air()));
    }
    QCOMPARE(grid.countChunks(), 15);
    QVERIFY(grid.get(glm::ivec3(10, 10, 10)) == Voxel::ground(6));
    QVERIFY(grid.get(glm::ivec3(20, 10, 10)) == Voxel::air());
    QVERIFY(grid.get(glm::ivec3(10, 20, 10)) == Voxel::air());

    // Chunks of the same ground compare equal, spelled out or not.
    Chunk dense(Voxel::air());
    dense.set(1, 1, 1, Voxel::ground());
    dense.set(1, 1, 1, Voxel::air());
    QVERIFY(!dense.isUniform());
    QVERIFY(dense == Chunk(Voxel::air()));
    QVERIFY(Chunk(Voxel::air()) == dense);
    QVERIFY(dense != Chunk(Voxel::ground()));
    dense.set(2, 2, 2, Voxel::ground(3));
    QVERIFY(dense != Chunk(Voxel::air()));
    Chunk other = dense;
    QVERIFY(other == dense);
    other.set(2, 2, 2, Voxel::ground(4));
    QVERIFY(other != dense);

    // A copy of a grid with levels is its own: changing one leaves the other.
    VoxelGrid copy = grid;
    copy.setChunk(ChunkKey(0, 0, 0, 0), Chunk(Voxel::air()));
    QCOMPARE(copy.countChunks(), 1);
    QCOMPARE(grid.countChunks(), 15);
    QVERIFY(grid.get(glm::ivec3(10, 10, 10)) == Voxel::ground(6));
}

void TerrainTests::fileRoundTripKeepsChunkLevels() {
    VoxelGrid grid(glm::ivec3(70, 40, 50));
    grid.fillHeightField(hills, 1);
    Brush brush;
    brush.center = glm::vec3(20.0f, 10.0f, 20.0f);
    brush.radius = 1.5f;
    sculpt(grid, brush, SculptMode::Dig);
    brush.center = glm::vec3(40.0f, 10.0f, 30.0f);
    brush.radius = 2.5f;
    brush.material = 3;
    sculpt(grid, brush, SculptMode::Fill);
    QCOMPARE(grid.getFinestLevel(), 2);
    QVERIFY(grid.countChunks() > 12);

    Bytes bytes = saveTerrain(grid);
    QCOMPARE((int)bytes[4], 3);   // the version: chunks with levels
    VoxelGrid loaded;
    PaintMap loadedPaint;
    std::string error;
    QVERIFY2(loadTerrain(bytes, loaded, &loadedPaint, nullptr, &error), error.c_str());
    QVERIFY(loadedPaint.isEmpty());   // none was written
    QCOMPARE(loaded.getVoxelCount(), grid.getVoxelCount());
    QCOMPARE(loaded.countChunks(), grid.countChunks());
    QCOMPARE(loaded.countDenseChunks(), grid.countDenseChunks());
    std::vector<std::pair<ChunkKey, const Chunk*>> saved, read;
    grid.forEachChunk([&](const ChunkKey& key, const Chunk& chunk) { saved.emplace_back(key, &chunk); });
    loaded.forEachChunk([&](const ChunkKey& key, const Chunk& chunk) { read.emplace_back(key, &chunk); });
    QCOMPARE(saved.size(), read.size());
    for (size_t i = 0; i < saved.size(); ++i) {
        QVERIFY(saved[i].first == read[i].first);
        QVERIFY(*saved[i].second == *read[i].second);
    }
    QCOMPARE((int)loaded.getDirtyChunks().size(), loaded.countChunks());
    QVERIFY(twoToOneRuleHolds(loaded));

    // With a painted map the map rides along as in version 2.
    PaintMap paint(glm::ivec2(70, 50), 2);
    PaintBrush paintBrush;
    paintBrush.center = glm::vec2(20.0f, 20.0f);
    paintBrush.radius = 5.0f;
    paintBrush.strength = 1.0f;
    paintBrush.layer = 3;
    QVERIFY(terrain::paint(paint, paintBrush, false) > 0);
    Bytes painted = saveTerrain(grid, &paint);
    QCOMPARE((int)painted[4], 3);
    QVERIFY(painted.size() > bytes.size());
    QVERIFY2(loadTerrain(painted, loaded, &loadedPaint, nullptr, &error), error.c_str());
    QCOMPARE(loaded.countChunks(), grid.countChunks());
    QVERIFY(loadedPaint.getWeights() == paint.getWeights());

    // A file of level-0 chunks is still version 1, and a cut file fails cleanly.
    VoxelGrid flat(glm::ivec3(70, 40, 50));
    flat.fillHeightField(hills, 1);
    QCOMPARE((int)saveTerrain(flat)[4], 1);
    Bytes cut(bytes.begin(), bytes.begin() + bytes.size() * 2 / 3);
    QVERIFY(!loadTerrain(cut, loaded, &loadedPaint, nullptr, &error));
    QVERIFY(!error.empty());
    QVERIFY(loaded.isEmpty());
    // a level the reader does not know is refused
    Bytes tooFine = bytes;
    tooFine[4 + 4 + 12 + 4 + 4 + 12] = (uint8_t)(MAX_CHUNK_LEVEL + 1);   // the first record's level byte
    QVERIFY(!loadTerrain(tooFine, loaded, &loadedPaint, nullptr, &error));
    QVERIFY(error.find("level") != std::string::npos);
}

void TerrainTests::resampleReadsTheFineChunks() {
    VoxelGrid source(glm::ivec3(64, 64, 64));
    source.fillBelow(40.5f, 1);
    Brush brush;
    brush.center = glm::vec3(32.0f, 40.5f, 32.0f);
    brush.radius = 1.5f;
    brush.material = 2;
    QVERIFY(sculpt(source, brush, SculptMode::Dig) > 0);
    QCOMPARE(source.getFinestLevel(), 2);

    // Twice the voxels, all at level 0: the fine hole is read from the fine chunks, not from the grid points between.
    const VoxelGrid fine = resampleGrid(source, glm::ivec3(128, 128, 128));
    QCOMPARE(fine.getFinestLevel(), 0);
    QCOMPARE(fine.countChunks(), 64);
    QVERIFY(std::abs(surfaceHeight(fine, 64.0f, 64.0f, 74.0f, 80.0f) - 78.0f) < 0.3f);
    QVERIFY(std::abs(surfaceHeight(fine, 66.0f, 64.0f, 74.0f, 80.0f) - 2.0f * (40.5f - std::sqrt(1.5f * 1.5f - 1.0f))) < 0.4f);
    QVERIFY(std::abs(surfaceHeight(fine, 70.0f, 64.0f, 74.0f, 84.0f) - 81.0f) < 0.2f);
    // Inside the sphere below the old surface the field is the sphere's: radius less the distance from the centre.
    // At these points, between the source's grid voxels, the fine chunks say so and the grid voxels alone would not.
    const glm::vec3 offsets[] = { glm::vec3(0.5f, -1.0f, 0.0f), glm::vec3(-0.5f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.5f),
                                  glm::vec3(0.5f, -0.5f, 0.5f), glm::vec3(1.0f, -0.5f, 0.0f), glm::vec3(0.5f, -1.0f, 0.5f) };
    for (const glm::vec3& offset : offsets) {
        const float expected = brush.radius - glm::length(offset);
        QVERIFY(std::abs(source.fieldAt(brush.center + offset).distance - expected) < 0.03f);
        QVERIFY(std::abs(fine.fieldAt(2.0f * (brush.center + offset)).distance - 2.0f * expected) < 0.08f);
    }
    // and half the voxels: the hole is too small to keep, but the ground around it is where it was
    const VoxelGrid coarse = resampleGrid(source, glm::ivec3(32, 32, 32));
    QCOMPARE(coarse.countChunks(), 1);
    QVERIFY(std::abs(surfaceHeight(coarse, 5.0f, 5.0f, 15.0f, 25.0f) - 20.25f) < 0.1f);
}

namespace {

// The meshes of a cut's nodes: the grid's own chunks' for the nodes drawn with those, the nodes' own for the rest.
KeyedMeshes meshCut(const VoxelGrid& grid, const LodCut& cut) {
    KeyedMeshes meshes;
    ChunkSamples samples;
    for (const LodCut::Node& node : cut.getNodes()) {
        if (node.isGridChunk) {
            meshes.emplace_back(node.key, meshChunk(grid, node.key));
        } else {
            sampleNode(grid, node.key, node.context, samples);
            meshes.emplace_back(node.key, meshChunk(samples));
        }
    }
    return meshes;
}

// Whether two nodes' boxes share any grid voxel, and whether they touch at all (faces, edges or corners).
bool boxesOverlap(const ChunkKey& a, const ChunkKey& b) {
    const glm::ivec3 aLow = a.getOrigin();
    const glm::ivec3 bLow = b.getOrigin();
    return glm::all(glm::lessThan(aLow, bLow + b.getSpan())) && glm::all(glm::lessThan(bLow, aLow + a.getSpan()));
}

bool boxesTouch(const ChunkKey& a, const ChunkKey& b) {
    const glm::ivec3 aLow = a.getOrigin();
    const glm::ivec3 bLow = b.getOrigin();
    return glm::all(glm::lessThanEqual(aLow, bLow + b.getSpan())) && glm::all(glm::lessThanEqual(bLow, aLow + a.getSpan()));
}

QString describe(const ChunkKey& key) {
    return QString("%1 (%2, %3, %4)").arg(key.level).arg(key.coords.x).arg(key.coords.y).arg(key.coords.z);
}

bool samplesEqual(const ChunkSamples& a, const ChunkSamples& b, QString& why) {
    for (int z = -1; z <= CHUNK_SIZE; ++z) {
        for (int y = -1; y <= CHUNK_SIZE; ++y) {
            for (int x = -1; x <= CHUNK_SIZE; ++x) {
                if (a.at(x, y, z) != b.at(x, y, z)) {
                    why = QString("sample (%1, %2, %3): %4/%5 against %6/%7").arg(x).arg(y).arg(z)
                        .arg(a.at(x, y, z).distance).arg(a.at(x, y, z).material).arg(b.at(x, y, z).distance).arg(b.at(x, y, z).material);
                    return false;
                }
            }
        }
    }
    for (int region = 0; region < 8; ++region) {
        if (a.getNeighbourLevel(region) != b.getNeighbourLevel(region)) {
            why = QString("neighbour level %1: %2 against %3").arg(region).arg(a.getNeighbourLevel(region)).arg(b.getNeighbourLevel(region));
            return false;
        }
    }
    if (a.getBlocks().size() != b.getBlocks().size()) {
        why = QString("%1 blocks against %2").arg(a.getBlocks().size()).arg(b.getBlocks().size());
        return false;
    }
    for (size_t i = 0; i < a.getBlocks().size(); ++i) {
        const SampleBlock& blockA = a.getBlocks()[i];
        const SampleBlock& blockB = b.getBlocks()[i];
        if (blockA.levelDelta != blockB.levelDelta || blockA.origin != blockB.origin || blockA.size != blockB.size) {
            why = QString("block %1 differs in its place").arg(i);
            return false;
        }
        for (size_t v = 0; v < blockA.voxels.size(); ++v) {
            if (blockA.voxels[v] != blockB.voxels[v]) {
                why = QString("block %1 voxel %2: %3 against %4").arg(i).arg(v).arg(blockA.voxels[v].distance).arg(blockB.voxels[v].distance);
                return false;
            }
        }
    }
    if (a.getLimit() != b.getLimit() || a.getFinerLimit() != b.getFinerLimit()) {
        why = "limits differ";
        return false;
    }
    return true;
}

glm::ivec3 fineOf(const glm::vec3& voxel) {
    return glm::ivec3(glm::floor(voxel * (float)FINEST_PER_GRID_VOXEL));
}

} // namespace

void TerrainTests::chunkKeysNameCoarserNodes() {
    const ChunkKey coarse(-2, 1, 0, 3);
    QCOMPARE(coarse.getSpan(), 128);
    QCOMPARE(coarse.getOrigin(), glm::ivec3(128, 0, 384));
    QCOMPARE(coarse.getVoxelSize(), 4.0f);
    QCOMPARE(coarse.getSlot(), glm::ivec3(4, 0, 12));
    QVERIFY(coarse.getChild(7) == ChunkKey(-1, 3, 1, 7));
    QVERIFY(coarse.getChild(7).getParent() == coarse);
    QVERIFY(ChunkKey(0, 5, 2, 3).getParent() == ChunkKey(-1, 2, 1, 1));
    QCOMPARE(ChunkKey(2, 1, 1, 1).getVoxelSize(), 0.25f);
    QVERIFY(coarse.intersects(glm::ivec3(255, 0, 400), glm::ivec3(300, 10, 410)));
    QVERIFY(!coarse.intersects(glm::ivec3(256, 0, 400), glm::ivec3(300, 10, 410)));

    // A coarser level's point is a grid voxel, read in that level's voxels; beyond the grid, the nearest in it.
    VoxelGrid grid(glm::ivec3(96, 64, 96));
    grid.fillBelow(20.5f, 3);
    const Voxel half = grid.sampleAt(-1, glm::ivec3(5, 10, 7));   // grid voxel (10, 20, 14), half a voxel under
    QCOMPARE((int)half.distance, (int)encodeDistance(-0.25f));
    QCOMPARE((int)half.material, 3);
    const Voxel& edge = grid.get(glm::ivec3(95, 8, 95));
    QVERIFY(grid.sampleAt(-2, glm::ivec3(100, 2, 100)) == Voxel(rescaleDistance(edge.getDistance(), 0.25f), edge.material));

    // How far a node's voxels reach before the grid ends, on a grid of odd sides.
    VoxelGrid odd(glm::ivec3(80, 40, 72));
    QCOMPARE(odd.limitOf(0, glm::ivec3(64, 32, 64), CHUNK_SIZE), glm::ivec3(16, 8, 8));
    QCOMPARE(odd.limitOf(1, glm::ivec3(64, 32, 64), 2 * CHUNK_SIZE), glm::ivec3(32, 16, 16));
    QCOMPARE(odd.limitOf(-1, glm::ivec3(0), CHUNK_SIZE), glm::ivec3(32, 20, 32));
    QCOMPARE(odd.limitOf(-2, glm::ivec3(0), CHUNK_SIZE), glm::ivec3(20, 10, 18));
    QCOMPARE(odd.limitOf(0, glm::ivec3(96, 0, 0), CHUNK_SIZE), glm::ivec3(0, 32, 32));
}

void TerrainTests::lodNodeSamplesMatchTheGridsChunks() {
    // Every chunk sampled as a node with the grid's own chunks around it gets the grid's own samples, to the bit:
    // hills with a small dig (chunks of levels 0 to 2), on a grid of whole chunks and on one of odd sides.
    for (const glm::ivec3& count : { glm::ivec3(96, 64, 96), glm::ivec3(80, 40, 72) }) {
        VoxelGrid grid(count);
        grid.fillHeightField(hills, 2);
        Brush small;
        small.center = glm::vec3(40.0f, hills(40, 40), 40.0f);
        small.radius = 1.5f;
        QVERIFY(sculpt(grid, small, SculptMode::Dig) > 0);
        QCOMPARE(grid.getFinestLevel(), 2);
        ChunkSamples expected;
        ChunkSamples actual;
        QString why;
        bool same = true;
        int compared = 0;
        grid.forEachChunk([&](const ChunkKey& key, const Chunk&) {
            if (!same) {
                return;
            }
            grid.sampleChunk(key, expected);
            sampleNode(grid, key, gridContext(grid, key), actual);
            same = samplesEqual(expected, actual, why);
            if (!same) {
                why = "chunk " + describe(key) + ": " + why;
            }
            ++compared;
        });
        QVERIFY2(same, qPrintable(why));
        QVERIFY(compared > 50);
    }
}

void TerrainTests::lodCutCoversTheGridOnceAndKeepsTheTwoToOneRule() {
    VoxelGrid grid(glm::ivec3(512, 64, 512));
    grid.fillHeightField(hills, 2);
    const glm::vec3 metres(0.5f);
    const glm::vec3 camera(256.0f, 30.0f, 256.0f);
    LodSettings settings;
    settings.detail = 1.0f;
    LodCut cut;
    cut.update(grid, camera, metres, settings);
    QCOMPARE(cut.getRootLevel(), -4);
    const std::vector<LodCut::Node>& nodes = cut.getNodes();

    // Each part of the grid in one node: the nodes' boxes within the grid add up to it, and none overlap.
    int64_t volume = 0;
    int coarsest = MAX_CHUNK_LEVEL;
    int finest = MIN_LOD_LEVEL;
    for (const LodCut::Node& node : nodes) {
        const glm::ivec3 low = node.key.getOrigin();
        const glm::ivec3 high = glm::min(low + node.key.getSpan(), grid.getVoxelCount());
        volume += (int64_t)(high.x - low.x) * (high.y - low.y) * (high.z - low.z);
        coarsest = std::min(coarsest, node.key.level);
        finest = std::max(finest, node.key.level);
    }
    QCOMPARE(volume, (int64_t)512 * 64 * 512);
    QCOMPARE(coarsest, -2);
    QCOMPARE(finest, 0);
    bool overlap = false;
    bool twoToOne = true;
    for (size_t i = 0; i < nodes.size(); ++i) {
        for (size_t j = i + 1; j < nodes.size(); ++j) {
            overlap = overlap || boxesOverlap(nodes[i].key, nodes[j].key);
            if (boxesTouch(nodes[i].key, nodes[j].key) && std::abs(nodes[i].key.level - nodes[j].key.level) > 1) {
                twoToOne = false;
            }
        }
    }
    QVERIFY(!overlap);
    QVERIFY(twoToOne);

    // Each node's context is the levels of the nodes at its probes; the nodes under the camera are the grid's own
    // chunks, drawn with their own meshes, those far away coarse.
    bool contextsAgree = true;
    for (const LodCut::Node& node : nodes) {
        for (int i = 0; i < 27; ++i) {
            if (i != NodeContext::CENTRE && cut.nodeCovering(contextProbe(grid, node.key, i)).level - node.key.level != node.context.at(i)) {
                contextsAgree = false;
            }
        }
    }
    QVERIFY(contextsAgree);
    const ChunkKey under = cut.nodeCovering(fineOf(glm::vec3(256.0f, 12.0f, 256.0f)));
    QCOMPARE(under.level, 0);
    bool underIsGridChunk = false;
    int gridChunks = 0;
    for (const LodCut::Node& node : nodes) {
        underIsGridChunk = underIsGridChunk || (node.key == under && node.isGridChunk);
        gridChunks += node.isGridChunk ? 1 : 0;
    }
    QVERIFY(underIsGridChunk);
    QVERIFY(gridChunks > 0 && gridChunks < (int)nodes.size());
    QCOMPARE(cut.nodeCovering(fineOf(glm::vec3(5.0f, 12.0f, 5.0f))).level, -2);

    // At the default detail the whole terrain is near: the grid's chunks within 128 voxels, a level coarser at the corners.
    cut.update(grid, camera, metres, LodSettings());
    QCOMPARE(cut.nodeCovering(fineOf(glm::vec3(256.0f + 100.0f, 12.0f, 256.0f))).level, 0);
    QCOMPARE(cut.nodeCovering(fineOf(glm::vec3(5.0f, 12.0f, 5.0f))).level, -1);

    // Without level of detail: the grid's own chunks, every one drawn with its own mesh.
    settings.detail = 0.0f;
    cut.update(grid, camera, metres, settings);
    QCOMPARE((int)cut.getNodes().size(), grid.countChunks());
    bool allGridChunks = true;
    for (const LodCut::Node& node : cut.getNodes()) {
        allGridChunks = allGridChunks && node.isGridChunk;
    }
    QVERIFY(allGridChunks);
}

void TerrainTests::lodCutKeepsItsSplitsWithinTheHysteresis() {
    // A row of chunks with the camera moving along it from beyond its low end. With a detail of 1 a level -1 node
    // (64 voxels of half a metre) is drawn as its children while the camera is within 32 m of it, and stays so
    // until it is 40 m away (a quarter more).
    VoxelGrid grid(glm::ivec3(256, 32, 32));
    grid.fillBelow(10.5f);
    const glm::vec3 metres(0.5f);
    LodSettings settings;
    settings.detail = 1.0f;
    LodCut cut;
    const ChunkKey node(-1, 3, 0, 0);   // grid voxels 192 to 255 along x
    auto drawnWhole = [&](float x) {
        cut.update(grid, glm::vec3(x, 16.0f, 16.0f), metres, settings);
        return cut.nodeCovering(fineOf(glm::vec3(200.0f, 10.0f, 10.0f))) == node;
    };
    QVERIFY(drawnWhole(100.0f));    // 46 m away
    QVERIFY(!drawnWhole(140.0f));   // 26 m: split
    QVERIFY(!drawnWhole(120.0f));   // 36 m: still split
    QVERIFY(drawnWhole(105.0f));    // 43.5 m: whole again
    QVERIFY(drawnWhole(120.0f));    // 36 m, coming from further: stays whole
}

void TerrainTests::lodMeshesMeetWithoutACrack() {
    // Hills under a camera at one corner: nodes from level -2 at the far corner to the grid's own chunks near the
    // camera (a low detail, for several levels on a small grid), closed but for the grid's sides. (Nearer the
    // middle, the 2:1 rule would split the far corner's node for the chunks a corner away from it.)
    VoxelGrid grid(glm::ivec3(256, 64, 256));
    grid.fillHeightField(hills, 2);
    const glm::vec3 metres(0.5f);
    LodSettings settings;
    settings.detail = 1.0f;
    LodCut cut;
    cut.update(grid, glm::vec3(10.0f, 20.0f, 10.0f), metres, settings);
    std::map<int, int> levels;
    int ownMeshes = 0;
    for (const LodCut::Node& node : cut.getNodes()) {
        ++levels[node.key.level];
        ownMeshes += node.isGridChunk ? 0 : 1;
    }
    QVERIFY(levels.begin()->first <= -2);
    QVERIFY(levels.count(0) > 0);
    QVERIFY(ownMeshes > 0);
    int triangles = 0;
    int full = 0;
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(countOpenEdges(grid, meshCut(grid, cut), &triangles, 8.0f), 0);
    qDebug() << "meshed" << cut.getNodes().size() << "nodes from level" << levels.begin()->first << "to" << levels.rbegin()->first
             << "," << ownMeshes << "of them not the grid's chunks," << triangles << "triangles, in" << timer.elapsed() << "ms";
    QCOMPARE(countOpenEdges(grid, &full), 0);
    qDebug() << "the grid's own chunks:" << grid.countChunks() << "chunks," << full << "triangles";
    QVERIFY(triangles < full / 2);

    // Fine chunks near the camera, drawn as they are, and far from it, where coarser nodes draw them.
    Brush small;
    small.radius = 1.5f;
    small.center = glm::vec3(30.0f, hills(30, 30), 30.0f);
    QVERIFY(sculpt(grid, small, SculptMode::Dig) > 0);
    small.center = glm::vec3(220.0f, hills(220, 220), 220.0f);
    QVERIFY(sculpt(grid, small, SculptMode::Dig) > 0);
    QCOMPARE(grid.getFinestLevel(), 2);
    cut.update(grid, glm::vec3(10.0f, 20.0f, 10.0f), metres, settings);
    QCOMPARE(cut.nodeCovering(fineOf(glm::vec3(30.0f, hills(30, 30), 30.0f))).level, 2);
    QVERIFY(cut.nodeCovering(fineOf(glm::vec3(220.0f, hills(220, 220), 220.0f))).level < 0);
    QCOMPARE(countOpenEdges(grid, meshCut(grid, cut), nullptr, 8.0f), 0);
    // and the camera over the far dig
    cut.update(grid, glm::vec3(225.0f, 15.0f, 225.0f), metres, settings);
    QCOMPARE(cut.nodeCovering(fineOf(glm::vec3(220.0f, hills(220, 220), 220.0f))).level, 2);
    QVERIFY(cut.nodeCovering(fineOf(glm::vec3(30.0f, hills(30, 30), 30.0f))).level < 0);
    QCOMPARE(countOpenEdges(grid, meshCut(grid, cut), nullptr, 8.0f), 0);

    // A grid of odd sides: no mesh runs on past the grid's end, by more than a voxel of its node.
    VoxelGrid odd(glm::ivec3(200, 40, 168));
    odd.fillHeightField(hills, 2);
    cut.update(odd, glm::vec3(10.0f, 20.0f, 10.0f), metres, settings);
    const KeyedMeshes meshes = meshCut(odd, cut);
    bool within = true;
    for (const auto& pair : meshes) {
        const glm::vec3 end = glm::vec3(odd.getVoxelCount()) + pair.first.getVoxelSize();
        for (const glm::vec3& p : pair.second.positions) {
            const glm::vec3 position = glm::vec3(pair.first.getOrigin()) + p * pair.first.getVoxelSize();
            within = within && glm::all(glm::lessThan(position, end));
        }
    }
    QVERIFY(within);
    QCOMPARE(countOpenEdges(odd, meshes, nullptr, 8.0f), 0);
}

void TerrainTests::levelOfDetailShowsANewCutOnceItsMeshesAreIn() {
    QThreadPool pool;
    pool.setMaxThreadCount(4);
    VoxelGrid grid(glm::ivec3(256, 64, 256));
    grid.fillHeightField(hills, 2);
    LevelOfDetail lod(&pool);
    LodSettings settings;
    settings.detail = 1.0f;
    lod.setSettings(settings);
    const glm::vec3 metres(0.5f);
    glm::vec3 camera(20.0f, 20.0f, 20.0f);
    auto gridChunkReady = [](const ChunkKey&) { return true; };   // the terrain's own chunks' meshes: there
    auto settle = [&] {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            lod.update(grid, camera, metres, gridChunkReady);
            if (lod.isSettled()) {
                return true;
            }
            QThread::msleep(1);
        }
        return false;
    };
    auto drawnKeys = [&] {
        std::vector<ChunkKey> keys;
        for (const LevelOfDetail::Drawn& drawn : lod.getDrawn()) {
            keys.push_back(drawn.key);
        }
        return keys;
    };
    auto cutKeys = [&] {
        std::vector<ChunkKey> keys;
        for (const LodCut::Node& node : lod.getCut().getNodes()) {
            keys.push_back(node.key);
        }
        return keys;
    };
    auto drawnAt = [&](const ChunkKey& key) -> const LevelOfDetail::Drawn* {
        for (const LevelOfDetail::Drawn& drawn : lod.getDrawn()) {
            if (drawn.key == key) {
                return &drawn;
            }
        }
        return nullptr;
    };

    // The first cut is drawn as its meshes come and settles on the cut wanted, each node with its samples' mesh.
    QElapsedTimer timer;
    timer.start();
    QVERIFY(settle());
    qDebug() << "the first cut," << lod.getDrawn().size() << "nodes," << lod.getMeshCount() << "meshes made here, settled in" << timer.elapsed() << "ms";
    QVERIFY(drawnKeys() == cutKeys());
    const KeyedMeshes expected = meshCut(grid, lod.getCut());
    bool meshesAgree = true;
    for (size_t i = 0; i < expected.size(); ++i) {
        const LevelOfDetail::Drawn& drawn = lod.getDrawn()[i];
        if (!drawn.isGridChunk) {
            const size_t triangles = drawn.mesh ? drawn.mesh->getTriangleCount() : 0;
            meshesAgree = meshesAgree && triangles == expected[i].second.getTriangleCount();
        }
    }
    QVERIFY(meshesAgree);
    const std::vector<ChunkKey> before = drawnKeys();

    // The camera at the far corner: the cut drawn stays until every node of the new one has its mesh.
    camera = glm::vec3(236.0f, 20.0f, 236.0f);
    lod.update(grid, camera, metres, gridChunkReady);
    QVERIFY(cutKeys() != before);
    QVERIFY(drawnKeys() == before);
    QVERIFY(settle());
    QVERIFY(drawnKeys() == cutKeys());

    // An edit under a coarse node far from the camera: its mesh is made again, and drawn.
    const glm::vec3 far(30.0f, hills(30, 30), 30.0f);
    const ChunkKey coarse = lod.getCut().nodeCovering(fineOf(far));
    QVERIFY(coarse.level < 0);
    QVERIFY(drawnAt(coarse) && drawnAt(coarse)->mesh);
    const uint64_t version = drawnAt(coarse)->version;
    const size_t triangles = drawnAt(coarse)->mesh->getTriangleCount();
    Brush wide;
    wide.center = far;
    wide.radius = 12.0f;
    QVERIFY(sculpt(grid, wide, SculptMode::Dig) > 0);
    QVERIFY(settle());
    QVERIFY(drawnAt(coarse) && drawnAt(coarse)->mesh);
    QVERIFY(drawnAt(coarse)->version != version);
    QVERIFY(drawnAt(coarse)->mesh->getTriangleCount() != triangles);

    // Without level of detail every node drawn is a grid chunk, drawn with its own mesh.
    settings.detail = 0.0f;
    lod.setSettings(settings);
    QVERIFY(settle());
    QCOMPARE((int)lod.getDrawn().size(), grid.countChunks());
    bool allGridChunks = true;
    for (const LevelOfDetail::Drawn& drawn : lod.getDrawn()) {
        allGridChunks = allGridChunks && drawn.isGridChunk;
    }
    QVERIFY(allGridChunks);
}

void TerrainTests::levelOfDetailSettlesAfterAnUndoBurst() {
    // The renderer's loop (RenderableTerrainEntityItem::updateMeshes and updateLevelOfDetail) through strokes that
    // split chunks near the camera and then undo them in a burst, as the editor's Undo does (whole level-0 places
    // put back with placeChunk): the cut shown must move to the cut wanted once the burst is over, every node near
    // the camera drawn as the grid's own chunk, and no mesh may stay for a chunk that is gone (the collision shape
    // is built from every mesh kept).
    QThreadPool pool;
    pool.setMaxThreadCount(4);
    VoxelGrid grid(glm::ivec3(256, 64, 256));
    grid.fillHeightField(hills, 2);
    const int chunksAtRest = grid.countChunks();

    // the terrain's own chunk meshes, as the renderer keeps them
    AsyncMesher mesher(&pool);
    std::map<ChunkKey, ChunkMesh> chunkMeshes;
    uint64_t structureVersion = grid.getStructureVersion();
    bool staleMeshes = false;
    auto updateMeshes = [&] {
        for (AsyncMesher::Result& result : mesher.collect()) {
            if (result.mesh.isEmpty()) {
                chunkMeshes.erase(result.key);
                continue;
            }
            chunkMeshes[result.key] = std::move(result.mesh);
        }
        if (grid.getStructureVersion() != structureVersion) {
            structureVersion = grid.getStructureVersion();
            staleMeshes = true;
        }
        if (staleMeshes) {
            staleMeshes = false;
            for (auto it = chunkMeshes.begin(); it != chunkMeshes.end();) {
                if (grid.hasChunk(it->first)) {
                    ++it;
                    continue;
                }
                bool pending = false;
                const glm::ivec3 origin = it->first.getOrigin();
                for (const ChunkKey& key : grid.chunksCovering(origin, origin + it->first.getSpan() - 1)) {
                    if (grid.getDirtyChunks().count(key) || mesher.isInFlight(key)) {
                        pending = true;
                        break;
                    }
                }
                if (pending) {
                    staleMeshes = true;
                    ++it;
                } else {
                    it = chunkMeshes.erase(it);
                }
            }
        }
        mesher.submit(grid);
    };

    LevelOfDetail lod(&pool);
    LodSettings settings;
    settings.detail = 4.0f;
    lod.setSettings(settings);
    const glm::vec3 metres(1.0f);
    const glm::vec3 camera(70.0f, 20.0f, 70.0f);
    auto gridChunkReady = [&](const ChunkKey& key) {
        return chunkMeshes.count(key) > 0 || (!grid.getDirtyChunks().count(key) && !mesher.isInFlight(key));
    };
    auto frame = [&] {
        updateMeshes();
        lod.update(grid, camera, metres, gridChunkReady);
    };
    auto settled = [&] { return lod.isSettled() && grid.getDirtyChunks().empty() && mesher.isIdle(); };
    auto settle = [&] {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            frame();
            if (settled()) {
                return true;
            }
            QThread::msleep(1);
        }
        return false;
    };
    auto frames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            frame();
            QThread::msleep(1);
        }
    };
    QVERIFY(settle());

    // A stroke records whole level-0 places, as EditorTerrain does, so that undoing it puts back whatever the
    // split made of them.
    using Chunks = std::map<ChunkKey, Chunk>;
    auto capture = [&](const Brush& brush) {
        Chunks before;
        glm::ivec3 low, high;
        if (!brushBounds(grid, brush, low, high)) {
            return before;
        }
        const int reach = std::max(grid.getMaxLevel() - 1, 0) * CHUNK_SIZE;
        const glm::ivec3 slotLow = VoxelGrid::chunkOf(low - reach);
        const glm::ivec3 slotHigh = VoxelGrid::chunkOf(high + reach);
        for (int z = slotLow.z; z <= slotHigh.z; ++z) {
            for (int y = slotLow.y; y <= slotHigh.y; ++y) {
                for (int x = slotLow.x; x <= slotHigh.x; ++x) {
                    const glm::ivec3 slot(x, y, z);
                    if (!grid.containsChunk(slot)) {
                        continue;
                    }
                    const glm::ivec3 origin = slot * CHUNK_SIZE;
                    for (const ChunkKey& key : grid.chunksCovering(origin, origin + CHUNK_SIZE - 1)) {
                        before.emplace(key, grid.getChunk(key));
                    }
                }
            }
        }
        return before;
    };
    std::vector<Chunks> undo;
    for (int i = 0; i < 12; ++i) {
        Brush brush;
        const int x = 56 + (i * 7) % 48;
        const int z = 56 + (i * 11) % 48;
        brush.center = glm::vec3((float)x, hills(x, z), (float)z);
        brush.radius = 3.0f + (float)(i % 3);
        undo.push_back(capture(brush));
        QVERIFY(sculpt(grid, brush, (i % 2) ? SculptMode::Dig : SculptMode::Raise) > 0);
        frames(3);
    }
    QVERIFY(grid.countChunks() > chunksAtRest);
    QVERIFY(settle());

    // the burst: every stroke undone, three frames apart, as keystrokes come
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
        for (const auto& pair : *it) {
            if (grid.containsChunk(pair.first)) {
                grid.placeChunk(pair.first, pair.second);
            }
        }
        frames(3);
    }
    QCOMPARE(grid.countChunks(), chunksAtRest);
    QVERIFY(settle());

    // what is drawn is the cut wanted, and near the camera the grid's own chunks
    const std::vector<LodCut::Node>& cut = lod.getCut().getNodes();
    QCOMPARE(lod.getDrawn().size(), cut.size());
    bool nearIsGrid = true;
    int nearNodes = 0;
    for (size_t i = 0; i < cut.size(); ++i) {
        const LevelOfDetail::Drawn& drawn = lod.getDrawn()[i];
        QVERIFY(drawn.key == cut[i].key);
        const float size = (float)cut[i].key.getSpan();
        if (cut[i].distance < settings.detail * size) {
            ++nearNodes;
            nearIsGrid = nearIsGrid && drawn.isGridChunk && grid.hasChunk(drawn.key);
        }
    }
    QVERIFY(nearNodes > 0);
    QVERIFY(nearIsGrid);
    // and every mesh kept is a chunk of the grid's
    int meshesOfGoneChunks = 0;
    for (const auto& pair : chunkMeshes) {
        if (!grid.hasChunk(pair.first)) {
            ++meshesOfGoneChunks;
        }
    }
    QCOMPARE(meshesOfGoneChunks, 0);
}

void TerrainTests::levelOfDetailDrawsTheGroundWholeAfterSmallRaises() {
    // Caity's holes of 2026-10-18: the sandbox's default terrain (32 x 16 x 32 m at 0.5 m, flat ground halfway up,
    // which lies on the boundary of the level-1 and level-2 chunks a small brush splits the places under it into),
    // raised by a 1 m brush in steps as the editor applies them (a shift of 0.2 grid voxels a step, a step every
    // other frame), the camera a few metres off, through the renderer's loop (RenderableTerrainEntityItem::
    // updateMeshes and updateLevelOfDetail). The ground drawn must be whole once the meshes have settled: every node
    // of the cut shown that the surface crosses has a mesh, and the meshes make a closed surface.
    QThreadPool pool;
    pool.setMaxThreadCount(4);
    VoxelGrid grid(glm::ivec3(64, 32, 64));
    grid.fillBelow(16.0f, 1);

    AsyncMesher mesher(&pool);
    std::map<ChunkKey, ChunkMesh> chunkMeshes;
    uint64_t structureVersion = grid.getStructureVersion();
    bool staleMeshes = false;
    auto updateMeshes = [&] {
        for (AsyncMesher::Result& result : mesher.collect()) {
            if (result.mesh.isEmpty()) {
                chunkMeshes.erase(result.key);
                continue;
            }
            chunkMeshes[result.key] = std::move(result.mesh);
        }
        if (grid.getStructureVersion() != structureVersion) {
            structureVersion = grid.getStructureVersion();
            staleMeshes = true;
        }
        if (staleMeshes) {
            staleMeshes = false;
            for (auto it = chunkMeshes.begin(); it != chunkMeshes.end();) {
                if (grid.hasChunk(it->first)) {
                    ++it;
                    continue;
                }
                bool pending = false;
                const glm::ivec3 origin = it->first.getOrigin();
                for (const ChunkKey& key : grid.chunksCovering(origin, origin + it->first.getSpan() - 1)) {
                    if (grid.getDirtyChunks().count(key) || mesher.isInFlight(key)) {
                        pending = true;
                        break;
                    }
                }
                if (pending) {
                    staleMeshes = true;
                    ++it;
                } else {
                    it = chunkMeshes.erase(it);
                }
            }
        }
        mesher.submit(grid);
    };

    LevelOfDetail lod(&pool);
    LodSettings settings;
    settings.detail = 4.0f;
    lod.setSettings(settings);
    const glm::vec3 metres(0.5f);
    const glm::vec3 camera(26.0f, 20.0f, 44.0f);   // 2 m over the ground, 6 m or so from the brush
    auto gridChunkReady = [&](const ChunkKey& key) {
        return chunkMeshes.count(key) > 0 || (!grid.getDirtyChunks().count(key) && !mesher.isInFlight(key));
    };
    auto frame = [&] {
        updateMeshes();
        lod.update(grid, camera, metres, gridChunkReady);
    };
    auto settled = [&] { return lod.isSettled() && grid.getDirtyChunks().empty() && mesher.isIdle(); };
    auto settle = [&] {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            frame();
            if (settled()) {
                return true;
            }
            QThread::msleep(1);
        }
        return false;
    };
    auto frames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            frame();
            QThread::msleep(1);
        }
    };
    QVERIFY(settle());

    // What is drawn, checked for holes: a node of the cut shown without a mesh where the surface crosses it, and
    // open edges between the meshes drawn.
    auto checkWhole = [&](const char* when) {
        KeyedMeshes drawn;
        int missing = 0;
        const std::vector<LodCut::Node>& nodes = lod.getCut().getNodes();
        const std::vector<LevelOfDetail::Drawn>& shown = lod.getDrawn();
        for (size_t i = 0; i < shown.size(); ++i) {
            const LevelOfDetail::Drawn& node = shown[i];
            if (node.isGridChunk) {
                auto found = chunkMeshes.find(node.key);
                if (found != chunkMeshes.end()) {
                    drawn.emplace_back(node.key, found->second);
                    continue;
                }
                const ChunkMesh own = meshChunk(grid, node.key);
                if (!own.isEmpty()) {
                    ++missing;
                    qDebug() << when << ": the grid chunk at level" << node.key.level << "at" << node.key.coords.x << node.key.coords.y
                             << node.key.coords.z << "is drawn with no mesh, though its own has" << own.getTriangleCount() << "triangles; dirty"
                             << (grid.getDirtyChunks().count(node.key) > 0) << "in flight" << mesher.isInFlight(node.key);
                }
                continue;
            }
            if (node.mesh) {
                drawn.emplace_back(node.key, *node.mesh);
                continue;
            }
            if (i < nodes.size() && nodes[i].key == node.key) {
                ChunkSamples samples;
                sampleNode(grid, node.key, nodes[i].context, samples);
                const ChunkMesh own = meshChunk(samples);
                if (!own.isEmpty()) {
                    ++missing;
                    qDebug() << when << ": the node at level" << node.key.level << "at" << node.key.coords.x << node.key.coords.y
                             << node.key.coords.z << "is drawn with no mesh, though its samples give" << own.getTriangleCount() << "triangles";
                }
            }
        }
        int triangles = 0;
        const int open = countOpenEdges(grid, drawn, &triangles);
        qDebug() << when << ":" << shown.size() << "nodes drawn," << drawn.size() << "with meshes," << triangles << "triangles," << open
                 << "open edges," << missing << "missing meshes";
        return missing == 0 && open == 0;
    };
    QVERIFY(checkWhole("flat"));

    // a drag of a 1 m brush (2 voxels at 0.5 m) along the ground, a step every other frame as the editor paces Raise
    // (RAISE_RATE 6 voxels a second, STEP_MIN 0.2), the centre picked on the ground as it rises
    Brush brush;
    brush.radius = 2.0f;
    brush.hardness = 0.5f;
    brush.strength = 0.2f;
    brush.material = 7;
    QCOMPARE(brushLevel(grid, brush), 1);
    int steps = 0;
    for (int pass = 0; pass < 4; ++pass) {
        for (int i = 0; i < 12; ++i) {
            const float x = 30.0f + (float)i * 0.5f;
            const float z = 33.0f + (float)(pass % 2) * 1.5f;
            brush.center = glm::vec3(x, surfaceHeight(grid, x, z, 8.0f, 31.0f), z);
            if (sculpt(grid, brush, SculptMode::Raise) > 0) {
                ++steps;
            }
            frames(2);
        }
    }
    QVERIFY(steps > 40);
    QVERIFY(grid.getFinestLevel() >= 1);
    QVERIFY(settle());
    QVERIFY(checkWhole("after the 1 m raises"));

    // and a 0.5 m brush (level 2) over the hill's side
    brush.radius = 1.0f;
    QCOMPARE(brushLevel(grid, brush), 2);
    for (int i = 0; i < 20; ++i) {
        const float x = 34.0f + (float)(i % 5) * 0.4f;
        const float z = 31.0f;
        brush.center = glm::vec3(x, surfaceHeight(grid, x, z, 8.0f, 31.0f), z);
        sculpt(grid, brush, SculptMode::Raise);
        frames(2);
    }
    QCOMPARE(grid.getFinestLevel(), 2);
    QVERIFY(settle());
    QVERIFY(checkWhole("after the 0.5 m raises"));
}

void TerrainTests::levelOfDetailDrawsATerrainFileWhole() {
    // A terrain file named by the environment (TERRAIN_FILE), loaded as the renderer loads one and drawn through its
    // loop from a few camera positions (TERRAIN_CAMERAS: "x,y,z;x,y,z" in grid voxels; the file's middle, high up,
    // when unset): the ground drawn must be whole at each. For files from the world that drew with holes.
    const QByteArray path = qgetenv("TERRAIN_FILE");
    if (path.isEmpty()) {
        QSKIP("set TERRAIN_FILE to a .drkt file to draw");
    }
    QFile file(QString::fromUtf8(path));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray data = file.readAll();
    VoxelGrid grid;
    PaintMap paint;
    LayerStack layers;
    std::string error;
    QVERIFY2(loadTerrain((const uint8_t*)data.constData(), (size_t)data.size(), grid, &paint, &layers, &error), error.c_str());
    std::map<int, int> chunksByLevel;
    int dense = 0;
    grid.forEachChunk([&](const ChunkKey& key, const Chunk& chunk) { ++chunksByLevel[key.level]; dense += chunk.isUniform() ? 0 : 1; });
    QString levelsText;
    for (const auto& pair : chunksByLevel) {
        levelsText += QString(" level %1: %2").arg(pair.first).arg(pair.second);
    }
    qDebug() << "grid" << grid.getVoxelCount().x << grid.getVoxelCount().y << grid.getVoxelCount().z << "voxels;" << qPrintable(levelsText)
             << "chunks," << dense << "dense; structure version" << grid.getStructureVersion() << "dirty" << grid.getDirtyChunks().size();
    QVERIFY(twoToOneRuleHolds(grid));
    // pictures of the paint map (a layer each) and of the top surface's materials and heights, beside the file
    {
        const QString dir = QFileInfo(file).absolutePath();
        auto writePGM = [&](const QString& name, int width, int height, const std::vector<uint8_t>& grey) {
            QFile out(dir + "/" + name);
            if (out.open(QIODevice::WriteOnly)) {
                out.write(QString("P5\n%1 %2\n255\n").arg(width).arg(height).toLatin1());
                out.write((const char*)grey.data(), (qint64)grey.size());
            }
        };
        if (!paint.isEmpty()) {
            const glm::ivec2 size = paint.getSize();
            for (int layer = 0; layer < PAINT_LAYERS; ++layer) {
                std::vector<uint8_t> grey((size_t)size.x * size.y);
                bool any = false;
                for (int y = 0; y < size.y; ++y) {
                    for (int x = 0; x < size.x; ++x) {
                        grey[(size_t)y * size.x + x] = paint.at(glm::ivec2(x, y))[layer];
                        any = any || grey[(size_t)y * size.x + x] != 0;
                    }
                }
                if (any) {
                    writePGM(QString("paint-%1.pgm").arg(layer), size.x, size.y, grey);
                }
            }
            qDebug() << "paint map" << size.x << "x" << size.y << "texels," << paint.getTexelsPerVoxel() << "a voxel";
        }
        const glm::ivec3 count = grid.getVoxelCount();
        std::vector<uint8_t> materials((size_t)count.x * count.z, 0), heights((size_t)count.x * count.z, 0);
        for (int z = 0; z < count.z; ++z) {
            for (int x = 0; x < count.x; ++x) {
                for (int y = count.y - 1; y >= 0; --y) {
                    const Voxel voxel = grid.get(glm::ivec3(x, y, z));
                    if (voxel.isGround()) {
                        materials[(size_t)z * count.x + x] = (uint8_t)(voxel.material * 32);
                        heights[(size_t)z * count.x + x] = (uint8_t)(y * 255 / count.y);
                        break;
                    }
                }
            }
        }
        writePGM("materials.pgm", count.x, count.z, materials);
        writePGM("heights.pgm", count.x, count.z, heights);
        std::map<int, int> materialCounts;
        grid.forEachChunk([&](const ChunkKey&, const Chunk& chunk) {
            if (chunk.isUniform()) {
                return;
            }
            for (const Voxel& voxel : chunk.getVoxels()) {
                if (voxel.isGround()) {
                    ++materialCounts[voxel.material];
                }
            }
        });
        QString materialsText;
        for (const auto& pair : materialCounts) {
            materialsText += QString(" %1: %2").arg(pair.first).arg(pair.second);
        }
        qDebug() << "ground voxels by material in dense chunks:" << qPrintable(materialsText);
    }
    int fullTriangles = 0;
    const int fullOpen = countOpenEdges(grid, &fullTriangles);
    qDebug() << "the grid's own chunks meshed:" << fullTriangles << "triangles," << fullOpen << "open edges";
    // TERRAIN_PROBE="x,y,z,radius" (grid voxels): the mesh around a spot, vertex by vertex, with the top surface
    // and the voxel materials there, for a patch that draws wrong
    const QList<QByteArray> probeParts = qgetenv("TERRAIN_PROBE").split(',');
    if (probeParts.size() == 4) {
        const glm::vec3 probe(probeParts[0].toFloat(), probeParts[1].toFloat(), probeParts[2].toFloat());
        const float radius = probeParts[3].toFloat();
        TopSurface top;
        top.rebuild(grid);
        for (int z = (int)std::floor(probe.z - radius); z <= (int)std::ceil(probe.z + radius); ++z) {
            QString row;
            for (int x = (int)std::floor(probe.x - radius); x <= (int)std::ceil(probe.x + radius); ++x) {
                int material = -1;
                for (int y = grid.getVoxelCount().y - 1; y >= 0; --y) {
                    const Voxel v = grid.get(glm::ivec3(x, y, z));
                    if (v.isGround()) {
                        material = v.material;
                        break;
                    }
                }
                // the paint map's coverage over the column: the strongest layer and its mean coverage over the texels
                QString painted = "-";
                if (!paint.isEmpty()) {
                    const int per = paint.getTexelsPerVoxel();
                    int sums[PAINT_LAYERS] = { 0 };
                    for (int j = 0; j < per; ++j) {
                        for (int i = 0; i < per; ++i) {
                            const glm::ivec2 texel(x * per + i, z * per + j);
                            if (paint.contains(texel)) {
                                for (int layer = 0; layer < PAINT_LAYERS; ++layer) {
                                    sums[layer] += paint.at(texel)[layer];
                                }
                            }
                        }
                    }
                    int best = 0;
                    for (int layer = 1; layer < PAINT_LAYERS; ++layer) {
                        if (sums[layer] > sums[best]) {
                            best = layer;
                        }
                    }
                    painted = QString("L%1:%2").arg(best).arg(sums[best] / (per * per));
                }
                row += QString(" %1/%2/%3").arg(top.heightAt(glm::ivec2(x, z)), 0, 'f', 1).arg(material).arg(painted);
            }
            qDebug() << "z" << z << "top/material/paint by x:" << qPrintable(row);
            // the surfaces under the top: a second ground met from the top down (the floor under a pillar's lip, a cave's)
            QString under;
            for (int x = (int)std::floor(probe.x - radius); x <= (int)std::ceil(probe.x + radius); ++x) {
                int state = 0;   // 0 air from the top, 1 in the first ground, 2 air below it
                float second = -1.0f;
                for (int y = grid.getVoxelCount().y - 1; y >= 0; --y) {
                    const bool ground = grid.get(glm::ivec3(x, y, z)).isGround();
                    if (state == 0 && ground) {
                        state = 1;
                    } else if (state == 1 && !ground) {
                        state = 2;
                    } else if (state == 2 && ground) {
                        second = (float)y + 0.5f;
                        break;
                    }
                }
                under += second < 0.0f ? " ." : QString(" %1").arg(second, 0, 'f', 1);
            }
            qDebug() << "z" << z << "second surface by x:" << qPrintable(under);
        }
        int flatBadNormals = 0;
        int flatTriangles = 0;
        grid.forEachChunk([&](const ChunkKey& key, const Chunk&) {
            const ChunkMesh mesh = meshChunk(grid, key);
            for (size_t t = 0; t < mesh.getTriangleCount(); ++t) {
                glm::vec3 p[3], n[3];
                for (int k = 0; k < 3; ++k) {
                    const uint32_t index = mesh.indices[3 * t + k];
                    p[k] = glm::vec3(key.getOrigin()) + mesh.positions[index] * key.getVoxelSize();
                    n[k] = mesh.normals[index];
                }
                const glm::vec3 centre = (p[0] + p[1] + p[2]) / 3.0f;
                if (glm::distance(centre, probe) > radius) {
                    continue;
                }
                const glm::vec3 face = glm::cross(p[1] - p[0], p[2] - p[0]);
                const glm::vec3 faceNormal = glm::length(face) > 1.0e-9f ? glm::normalize(face) : glm::vec3(0.0f);
                if (faceNormal.y > 0.9f) {
                    ++flatTriangles;
                    const float lowest = std::min(n[0].y, std::min(n[1].y, n[2].y));
                    if (lowest < 0.6f) {
                        ++flatBadNormals;
                        if (flatBadNormals <= 10) {
                            qDebug() << "flat triangle with a sideways vertex normal in chunk level" << key.level << "at" << key.coords.x << key.coords.y
                                     << key.coords.z << ": centre" << centre.x << centre.y << centre.z << "normals y" << n[0].y << n[1].y << n[2].y
                                     << "materials" << (int)mesh.materials[mesh.indices[3 * t]] << (int)mesh.materials[mesh.indices[3 * t + 1]]
                                     << (int)mesh.materials[mesh.indices[3 * t + 2]];
                        }
                    }
                }
            }
        });
        qDebug() << "around the probe:" << flatTriangles << "up-facing triangles," << flatBadNormals << "with a vertex normal under 0.6";
    }
    // every chunk's mesh: how its triangles face (the winding against the vertex normals, and how many face down:
    // a ceiling, an overhang, or an inside-out patch of ground)
    grid.forEachChunk([&](const ChunkKey& key, const Chunk& chunk) {
        const ChunkMesh mesh = meshChunk(grid, key);
        int against = 0;
        int down = 0;
        float lowY = 1.0e9f, highY = -1.0e9f;
        for (size_t t = 0; t < mesh.getTriangleCount(); ++t) {
            const glm::vec3& a = mesh.positions[mesh.indices[3 * t]];
            const glm::vec3& b = mesh.positions[mesh.indices[3 * t + 1]];
            const glm::vec3& c = mesh.positions[mesh.indices[3 * t + 2]];
            const glm::vec3 face = glm::cross(b - a, c - a);
            const glm::vec3 vertex = mesh.normals[mesh.indices[3 * t]] + mesh.normals[mesh.indices[3 * t + 1]] + mesh.normals[mesh.indices[3 * t + 2]];
            if (glm::dot(face, vertex) < 0.0f) {
                ++against;
            }
            if (glm::length(face) > 1.0e-9f && glm::normalize(face).y < -0.5f) {
                ++down;
            }
            lowY = std::min(lowY, std::min(a.y, std::min(b.y, c.y)));
            highY = std::max(highY, std::max(a.y, std::max(b.y, c.y)));
        }
        if (mesh.getTriangleCount() > 0) {
            qDebug() << "chunk level" << key.level << "at" << key.coords.x << key.coords.y << key.coords.z << (chunk.isUniform() ? "uniform" : "dense")
                     << ":" << mesh.getTriangleCount() << "triangles," << against << "wound against their normals," << down << "facing down; y from"
                     << key.getOrigin().y + lowY * key.getVoxelSize() << "to" << key.getOrigin().y + highY * key.getVoxelSize() << "grid voxels";
        }
    });

    std::vector<glm::vec3> cameras;
    const QByteArray camerasText = qgetenv("TERRAIN_CAMERAS");
    for (const QByteArray& one : camerasText.split(';')) {
        const QList<QByteArray> parts = one.trimmed().split(',');
        if (parts.size() == 3) {
            cameras.push_back(glm::vec3(parts[0].toFloat(), parts[1].toFloat(), parts[2].toFloat()));
        }
    }
    if (cameras.empty()) {
        cameras.push_back(glm::vec3(grid.getVoxelCount()) * glm::vec3(0.5f, 2.0f, 0.5f));
    }

    QThreadPool pool;
    pool.setMaxThreadCount(4);
    AsyncMesher mesher(&pool);
    std::map<ChunkKey, ChunkMesh> chunkMeshes;
    uint64_t structureVersion = grid.getStructureVersion();
    bool staleMeshes = false;
    auto updateMeshes = [&] {
        for (AsyncMesher::Result& result : mesher.collect()) {
            if (result.mesh.isEmpty()) {
                chunkMeshes.erase(result.key);
                continue;
            }
            chunkMeshes[result.key] = std::move(result.mesh);
        }
        if (grid.getStructureVersion() != structureVersion) {
            structureVersion = grid.getStructureVersion();
            staleMeshes = true;
        }
        if (staleMeshes) {
            staleMeshes = false;
            for (auto it = chunkMeshes.begin(); it != chunkMeshes.end();) {
                if (grid.hasChunk(it->first)) {
                    ++it;
                    continue;
                }
                bool pending = false;
                const glm::ivec3 origin = it->first.getOrigin();
                for (const ChunkKey& key : grid.chunksCovering(origin, origin + it->first.getSpan() - 1)) {
                    if (grid.getDirtyChunks().count(key) || mesher.isInFlight(key)) {
                        pending = true;
                        break;
                    }
                }
                if (pending) {
                    staleMeshes = true;
                    ++it;
                } else {
                    it = chunkMeshes.erase(it);
                }
            }
        }
        mesher.submit(grid);
    };
    LevelOfDetail lod(&pool);
    LodSettings settings;
    settings.detail = 4.0f;
    lod.setSettings(settings);
    const glm::vec3 metres(0.5f);
    glm::vec3 camera = cameras.front();
    auto gridChunkReady = [&](const ChunkKey& key) {
        return chunkMeshes.count(key) > 0 || (!grid.getDirtyChunks().count(key) && !mesher.isInFlight(key));
    };
    auto frame = [&] {
        updateMeshes();
        lod.update(grid, camera, metres, gridChunkReady);
    };
    auto settled = [&] { return lod.isSettled() && grid.getDirtyChunks().empty() && mesher.isIdle(); };
    auto settle = [&] {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            frame();
            if (settled()) {
                return true;
            }
            QThread::msleep(1);
        }
        return false;
    };
    auto checkWhole = [&](const QString& when) {
        KeyedMeshes drawn;
        int missing = 0;
        std::map<int, int> drawnByLevel;
        const std::vector<LodCut::Node>& nodes = lod.getCut().getNodes();
        const std::vector<LevelOfDetail::Drawn>& shown = lod.getDrawn();
        for (size_t i = 0; i < shown.size(); ++i) {
            const LevelOfDetail::Drawn& node = shown[i];
            ++drawnByLevel[node.key.level];
            if (node.isGridChunk) {
                auto found = chunkMeshes.find(node.key);
                if (found != chunkMeshes.end()) {
                    drawn.emplace_back(node.key, found->second);
                    continue;
                }
                const ChunkMesh own = meshChunk(grid, node.key);
                if (!own.isEmpty()) {
                    ++missing;
                    qDebug() << qPrintable(when) << ": the grid chunk at level" << node.key.level << "at" << node.key.coords.x << node.key.coords.y
                             << node.key.coords.z << "is drawn with no mesh, though its own has" << own.getTriangleCount() << "triangles; dirty"
                             << (grid.getDirtyChunks().count(node.key) > 0) << "in flight" << mesher.isInFlight(node.key);
                }
                continue;
            }
            if (node.mesh) {
                drawn.emplace_back(node.key, *node.mesh);
                continue;
            }
            if (i < nodes.size() && nodes[i].key == node.key) {
                ChunkSamples samples;
                sampleNode(grid, node.key, nodes[i].context, samples);
                const ChunkMesh own = meshChunk(samples);
                if (!own.isEmpty()) {
                    ++missing;
                    qDebug() << qPrintable(when) << ": the node at level" << node.key.level << "at" << node.key.coords.x << node.key.coords.y
                             << node.key.coords.z << "is drawn with no mesh, though its samples give" << own.getTriangleCount() << "triangles";
                }
            }
        }
        QString levelsText;
        for (const auto& pair : drawnByLevel) {
            levelsText += QString(" level %1: %2").arg(pair.first).arg(pair.second);
        }
        int triangles = 0;
        const int open = countOpenEdges(grid, drawn, &triangles, 1.0f);
        qDebug() << qPrintable(when) << ":" << shown.size() << "nodes drawn (" << qPrintable(levelsText) << ")," << drawn.size() << "with meshes,"
                 << triangles << "triangles," << open << "open edges," << missing << "missing meshes";
        return missing == 0 && open == 0;
    };
    bool whole = true;
    for (size_t i = 0; i < cameras.size(); ++i) {
        camera = cameras[i];
        QVERIFY(settle());
        whole = checkWhole(QString("camera %1 (%2, %3, %4)").arg(i).arg(camera.x).arg(camera.y).arg(camera.z)) && whole;
    }
    QVERIFY(whole);
}

void TerrainTests::closedFacesCapTheGround() {
    // With every face of the box closed the ground is watertight: a floor at the bottom, walls at the sides, a cap
    // at the top where a raise reaches it, every edge on two triangles, whatever the chunks' levels and the cut.
    VoxelGrid grid(glm::ivec3(64, 32, 64));
    grid.fillBelow(16.5f, 1);
    int triangles = 0;
    QVERIFY(countOpenEdges(grid, &triangles) == 0);
    const int openBefore = countOpenEdges(grid, [&] { KeyedMeshes m; grid.forEachChunk([&](const ChunkKey& key, const Chunk&) { m.emplace_back(key, meshChunk(grid, key)); }); return m; }(), nullptr, -1.0f);
    QVERIFY(openBefore > 0);   // open along the sides without caps
    grid.setCaps(CAP_ALL);
    QCOMPARE((int)grid.getCaps(), (int)CAP_ALL);
    auto allMeshes = [&] {
        KeyedMeshes meshes;
        grid.forEachChunk([&](const ChunkKey& key, const Chunk&) { meshes.emplace_back(key, meshChunk(grid, key)); });
        return meshes;
    };
    auto extent = [&](const KeyedMeshes& meshes, glm::vec3& low, glm::vec3& high) {
        low = glm::vec3(1.0e9f);
        high = glm::vec3(-1.0e9f);
        for (const auto& pair : meshes) {
            for (const glm::vec3& p : pair.second.positions) {
                const glm::vec3 position = glm::vec3(pair.first.getOrigin()) + p * pair.first.getVoxelSize();
                low = glm::min(low, position);
                high = glm::max(high, position);
            }
        }
    };
    KeyedMeshes meshes = allMeshes();
    std::vector<std::pair<glm::vec3, glm::vec3>> openEdges;
    const int open = countOpenEdges(grid, meshes, &triangles, -1.0f, &openEdges);
    QCOMPARE(open, 0);
    glm::vec3 low, high;
    extent(meshes, low, high);
    const float past = 0.5f / (float)FINEST_PER_GRID_VOXEL;   // the planes, just past the first and last voxels
    QVERIFY(std::abs(low.y + past) < 0.01f);    // the floor
    QVERIFY(std::abs(low.x + past) < 0.01f);    // the walls
    QVERIFY(std::abs(high.x - 63.0f - past) < 0.01f);
    QVERIFY(std::abs(high.z - 63.0f - past) < 0.01f);
    QVERIFY(high.y < 17.0f);                     // no cap at the top: the ground does not reach it

    // chunks split at the faces by a small dig in a corner and a raise at an edge: still closed
    Brush small;
    small.radius = 1.5f;
    small.center = glm::vec3(1.0f, 16.5f, 1.0f);
    QVERIFY(sculpt(grid, small, SculptMode::Dig) > 0);
    small.center = glm::vec3(62.0f, 16.5f, 30.0f);
    small.strength = 1.0f;
    for (int i = 0; i < 4; ++i) {
        sculpt(grid, small, SculptMode::Raise);
    }
    QCOMPARE(grid.getFinestLevel(), 2);
    meshes = allMeshes();
    openEdges.clear();
    const int openSplit = countOpenEdges(grid, meshes, &triangles, -1.0f, &openEdges);
    QCOMPARE(openSplit, 0);

    // ground up to the top of the box: capped there
    VoxelGrid full(glm::ivec3(64, 32, 64));
    full.fillBelow(40.0f, 1);
    full.setCaps(CAP_ALL);
    KeyedMeshes fullMeshes;
    full.forEachChunk([&](const ChunkKey& key, const Chunk&) { fullMeshes.emplace_back(key, meshChunk(full, key)); });
    QCOMPARE(countOpenEdges(full, fullMeshes, &triangles, -1.0f), 0);
    extent(fullMeshes, low, high);
    QVERIFY(std::abs(high.y - 31.0f - past) < 0.01f);
    QVERIFY(triangles > 0);
    // and only the top closed: the sides stay open
    full.setCaps(CAP_HIGH_Y);
    fullMeshes.clear();
    full.forEachChunk([&](const ChunkKey& key, const Chunk&) { fullMeshes.emplace_back(key, meshChunk(full, key)); });
    extent(fullMeshes, low, high);
    QVERIFY(std::abs(high.y - 31.0f - past) < 0.01f);
    QVERIFY(low.y > 0.0f);

    // the level of detail's nodes close the same way: a cut from a corner on hills, nodes of several levels
    VoxelGrid big(glm::ivec3(256, 64, 256));
    big.fillHeightField(hills, 2);
    big.setCaps(CAP_ALL);
    LodSettings settings;
    settings.detail = 1.0f;
    LodCut cut;
    cut.update(big, glm::vec3(10.0f, 20.0f, 10.0f), glm::vec3(0.5f), settings);
    openEdges.clear();
    const int openCut = countOpenEdges(big, meshCut(big, cut), &triangles, -1.0f, &openEdges);
    // Known limit (2026-10-18): where a closed high face meets a join of coarse nodes one polygon can be missing (six
    // edges on these hills, at x 252-255, z 127-130); the grid's own chunks are watertight. Every open edge must lie
    // within four voxels of a closed high face, and there must be only a few.
    int nearAHighFace = 0;
    for (const auto& edge : openEdges) {
        const glm::vec3 high = glm::vec3(big.getVoxelCount() - 1);
        bool near = false;
        for (int axis = 0; axis < 3; ++axis) {
            near = near || (edge.first[axis] >= high[axis] - 4.0f && edge.second[axis] >= high[axis] - 4.0f);
        }
        nearAHighFace += near ? 1 : 0;
    }
    qDebug() << "the cut on the closed box:" << triangles << "triangles," << openCut << "open edges, all near a closed high face:" << (nearAHighFace == openCut);
    QCOMPARE(nearAHighFace, openCut);
    QVERIFY(openCut <= 12);
    QVERIFY(triangles > 0);
}

void TerrainTests::topSurfaceFollowsRaisedPillars() {
    // Pillars raised with a 0.5 m brush, as Caity's test terrain of 2026-10-18, the top surface updated every few
    // steps as the renderer does: its heights must equal a fresh rebuild's, and every up-facing point of the ground
    // the sky sees must count as on top by the four columns around it (the notches cut out of her snow).
    VoxelGrid grid(glm::ivec3(64, 32, 64));
    grid.fillBelow(16.5f, 1);
    TopSurface top;
    top.update(grid);
    Brush brush;
    brush.radius = 1.0f;
    brush.hardness = 0.5f;
    brush.strength = 0.2f;
    int steps = 0;
    for (int pillar = 0; pillar < 6; ++pillar) {
        const float x = 20.0f + (float)(pillar % 3) * 5.0f + (float)pillar * 0.3f;
        const float z = 24.0f + (float)(pillar / 3) * 6.0f;
        for (int i = 0; i < 40; ++i) {
            brush.center = glm::vec3(x, surfaceHeight(grid, x, z, 8.0f, 31.0f), z);
            if (sculpt(grid, brush, SculptMode::Raise) > 0) {
                ++steps;
            }
            if (i % 2 == 1) {
                top.update(grid);
            }
        }
    }
    top.update(grid);
    QVERIFY(steps > 100);
    QCOMPARE(grid.getFinestLevel(), 2);

    TopSurface fresh;
    fresh.rebuild(grid);
    int staleColumns = 0;
    for (int z = 0; z < 64; ++z) {
        for (int x = 0; x < 64; ++x) {
            if (std::abs(top.heightAt(glm::ivec2(x, z)) - fresh.heightAt(glm::ivec2(x, z))) > 1.0e-4f) {
                if (staleColumns < 8) {
                    qDebug() << "column" << x << z << "top" << top.heightAt(glm::ivec2(x, z)) << "fresh" << fresh.heightAt(glm::ivec2(x, z));
                }
                ++staleColumns;
            }
        }
    }
    QCOMPARE(staleColumns, 0);

    // the ground the sky sees: up-facing mesh points at the top of their column are on top; those under a pillar's
    // lip or on a wall are not asked
    int upPoints = 0;
    int notOnTop = 0;
    grid.forEachChunk([&](const ChunkKey& key, const Chunk&) {
        const ChunkMesh mesh = meshChunk(grid, key);
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            if (mesh.normals[i].y < 0.8f) {
                continue;
            }
            const glm::vec3 p = glm::vec3(key.getOrigin()) + mesh.positions[i] * key.getVoxelSize();
            // the highest ground of its own column is the top there; a point within a voxel of it is the sky's
            const float ownTop = fresh.heightAt(glm::ivec2((int)std::floor(p.x + 0.5f), (int)std::floor(p.z + 0.5f)));
            if (p.y < ownTop - 1.0f) {
                continue;
            }
            ++upPoints;
            if (!top.isOnTop(p)) {
                if (notOnTop < 8) {
                    qDebug() << "up-facing point not on top at" << p.x << p.y << p.z << "own column's top" << ownTop;
                }
                ++notOnTop;
            }
        }
    });
    QVERIFY(upPoints > 1000);
    QCOMPARE(notOnTop, 0);
}

void TerrainTests::shelteredFloorsUnderALipAreTheSkysGround() {
    // A pillar whose top widens into a lip (as Raise builds them) shelters the ground at its foot: that ground is the
    // same ground as the open ground beside it and counts as the sky's (it takes the paint); a cave's floor further
    // in than SHELTER_REACH_COLUMNS from its mouth does not.
    VoxelGrid grid(glm::ivec3(64, 32, 64));
    grid.fillBelow(16.5f, 1);
    auto fill = [&](const glm::ivec3& low, const glm::ivec3& high, bool ground) {
        for (int z = low.z; z <= high.z; ++z) {
            for (int y = low.y; y <= high.y; ++y) {
                for (int x = low.x; x <= high.x; ++x) {
                    grid.set(glm::ivec3(x, y, z), ground ? Voxel::ground(2) : Voxel::air());
                }
            }
        }
    };
    // the pillar: a stem 2 voxels wide, a cap 8 wide, 2 voxels of air between the cap and the ground around the stem
    fill(glm::ivec3(30, 17, 30), glm::ivec3(31, 26, 31), true);
    fill(glm::ivec3(27, 24, 27), glm::ivec3(34, 26, 34), true);
    // the cave: a roof 3 voxels thick over a floor at the ground's height, open at its mouth on the low x side
    fill(glm::ivec3(40, 20, 40), glm::ivec3(60, 22, 60), true);
    TopSurface top;
    top.rebuild(grid);

    // under the lip beside the stem: a second ground that reaches open ground within three columns
    QVERIFY(top.heightAt(glm::ivec2(28, 31)) > 26.0f);
    QVERIFY(std::abs(top.floorAt(glm::ivec2(28, 31)) - 16.5f) < 0.6f);
    QVERIFY(top.isOnTop(glm::vec3(28.5f, 16.5f, 31.5f)));
    // the lip's underside, between the floor and the top, is neither
    QVERIFY(!top.isOnTop(glm::vec3(28.5f, 22.0f, 31.5f)));
    // open ground beside the pillar, and the pillar's top
    QVERIFY(top.isOnTop(glm::vec3(25.5f, 16.5f, 31.5f)));
    QVERIFY(top.isOnTop(glm::vec3(30.5f, 27.0f, 30.5f)));
    QCOMPARE(top.floorAt(glm::ivec2(25, 31)), NO_GROUND_HEIGHT);

    // the cave: its floor near the mouth is the sky's ground, deep inside it is not
    QVERIFY(top.heightAt(glm::ivec2(50, 50)) > 22.0f);
    QVERIFY(std::abs(top.floorAt(glm::ivec2(41, 50)) - 16.5f) < 0.6f);
    QVERIFY(top.isOnTop(glm::vec3(41.5f, 16.5f, 50.5f)));
    QCOMPARE(top.floorAt(glm::ivec2(50, 50)), NO_GROUND_HEIGHT);
    QVERIFY(!top.isOnTop(glm::vec3(50.5f, 16.5f, 50.5f)));

    // the tile carries both: two floats a texel
    std::vector<float> tile;
    top.fillTile(glm::ivec2(0, 0), tile);
    QCOMPARE((int)tile.size(), TopSurface::getTileSize() * TopSurface::getTileSize() * 2);
    const int i = (31 + PAINT_TILE_APRON) * TopSurface::getTileSize() + (28 + PAINT_TILE_APRON);
    QVERIFY(tile[i * 2] > 26.0f);
    QVERIFY(std::abs(tile[i * 2 + 1] - 16.5f) < 0.6f);

    // an edit follows through the incremental update: the cap dug away, the floor is open ground again
    fill(glm::ivec3(27, 24, 27), glm::ivec3(34, 26, 34), false);
    top.update(grid);
    QVERIFY(std::abs(top.heightAt(glm::ivec2(28, 31)) - 16.5f) < 0.6f);
    QCOMPARE(top.floorAt(glm::ivec2(28, 31)), NO_GROUND_HEIGHT);
    QVERIFY(top.isOnTop(glm::vec3(28.5f, 16.5f, 31.5f)));
}

void TerrainTests::writeCleanTestTerrain() {
    // Writes a terrain file for hand tests (TERRAIN_CLEAN_FILE=<path>): 32 x 16 x 32 m at 0.5 m, flat ground halfway
    // up, painted snow everywhere over rock voxels, with one of each thing the paint rules are about, at set places
    // from the box's centre (x to the right, z toward the viewer who looks along -z):
    //   a pillar with a lip (a mushroom) at (-8 m, -8 m); a terrace 3 m high with vertical walls at x 0..8 m, z -11..-6 m;
    //   a cave, roof 1.5 m thick over a floor at ground level, open on its west side, at x 0..10 m, z 2..12 m;
    //   a smooth hill 2 m high at (-8 m, +8 m). Where the paint applies the ground is snow; where it does not, rock.
    const QByteArray path = qgetenv("TERRAIN_CLEAN_FILE");
    if (path.isEmpty()) {
        QSKIP("set TERRAIN_CLEAN_FILE to a path to write the clean test terrain");
    }
    VoxelGrid grid(glm::ivec3(64, 32, 64));
    const float ground = 16.5f;
    grid.fillHeightField([&](int x, int z) {
        // the hill: a smooth bump 4 voxels (2 m) high around voxel (16, 48)
        const float dx = (float)x - 16.0f;
        const float dz = (float)z - 48.0f;
        const float r = std::sqrt(dx * dx + dz * dz);
        const float hill = r < 8.0f ? 4.0f * 0.5f * (1.0f + std::cos(r / 8.0f * 3.14159265f)) : 0.0f;
        return ground + hill;
    }, 2);
    auto fill = [&](const glm::ivec3& low, const glm::ivec3& high, bool isGround) {
        for (int z = low.z; z <= high.z; ++z) {
            for (int y = low.y; y <= high.y; ++y) {
                for (int x = low.x; x <= high.x; ++x) {
                    grid.set(glm::ivec3(x, y, z), isGround ? Voxel::ground(2) : Voxel::air());
                }
            }
        }
    };
    // the pillar with a lip: stem 2 voxels wide at (16, 16), 10 voxels tall; cap 8 wide, 3 thick, 2 voxels of air under its lip
    fill(glm::ivec3(15, 17, 15), glm::ivec3(16, 26, 16), true);
    fill(glm::ivec3(12, 24, 12), glm::ivec3(19, 26, 19), true);
    // the terrace: x 32..47, z 10..19, 6 voxels (3 m) up, vertical walls
    fill(glm::ivec3(32, 17, 10), glm::ivec3(47, 22, 19), true);
    // the cave: a roof 3 voxels thick at y 20..22 over x 32..51, z 36..55; its floor is the ground; open on the low x side
    fill(glm::ivec3(32, 20, 36), glm::ivec3(51, 22, 55), true);
    // snow painted over everything
    PaintMap paint(glm::ivec2(64, 64), 4);
    for (int j = 0; j < paint.getSize().y; ++j) {
        for (int i = 0; i < paint.getSize().x; ++i) {
            paint.at(glm::ivec2(i, j))[7] = 255;
        }
    }
    paint.markAllColumnsDirty();
    if (qEnvironmentVariableIsSet("TERRAIN_CLEAN_CLOSED")) {
        grid.setCaps(CAP_ALL);   // the same terrain with its box closed
    }
    const Bytes bytes = saveTerrain(grid, &paint, nullptr);
    QFile out(QString::fromUtf8(path));
    QVERIFY(out.open(QIODevice::WriteOnly));
    QCOMPARE((int)out.write((const char*)bytes.data(), (qint64)bytes.size()), (int)bytes.size());
    qDebug() << "wrote" << bytes.size() << "bytes to" << path;
}

void TerrainTests::fileRoundTripKeepsTheClosedFaces() {
    VoxelGrid grid(glm::ivec3(64, 32, 64));
    grid.fillBelow(16.5f, 1);
    // no closed face: the file stays at the older layout
    Bytes plain = saveTerrain(grid, nullptr, nullptr);
    QVERIFY(plain.size() > 8);
    QVERIFY(plain[4] < 6);
    grid.setCaps(CAP_HIGH_Y | CAP_LOW_X);
    Bytes bytes = saveTerrain(grid, nullptr, nullptr);
    QCOMPARE((int)bytes[4], 6);
    VoxelGrid back;
    PaintMap paint;
    LayerStack layers;
    std::string error;
    QVERIFY2(loadTerrain(bytes.data(), bytes.size(), back, &paint, &layers, &error), error.c_str());
    QCOMPARE((int)back.getCaps(), (int)(CAP_HIGH_Y | CAP_LOW_X));
    QVERIFY(layers.isEmpty());
    QCOMPARE(back.getVoxelCount(), grid.getVoxelCount());
    QVERIFY(back.get(glm::ivec3(10, 10, 10)).isGround());
    QVERIFY(!back.get(glm::ivec3(10, 20, 10)).isGround());
    // with layers and the paint map as well
    PaintMap map(glm::ivec2(64, 64), 2);
    PaintBrush brush;
    brush.center = glm::vec2(20.0f, 20.0f);
    brush.radius = 5.0f;
    brush.layer = 7;
    QVERIFY(terrain::paint(map, brush, false) > 0);
    LayerStack stack = LayerStack::defaults();
    bytes = saveTerrain(grid, &map, &stack);
    QCOMPARE((int)bytes[4], 6);
    VoxelGrid back2;
    PaintMap paint2;
    LayerStack layers2;
    QVERIFY2(loadTerrain(bytes.data(), bytes.size(), back2, &paint2, &layers2, &error), error.c_str());
    QCOMPARE((int)back2.getCaps(), (int)(CAP_HIGH_Y | CAP_LOW_X));
    QVERIFY(layers2 == stack);
    QVERIFY(paint2.isPainted());
}

void TerrainTests::levelOfDetailAsksAgainForAStalledNodeAndThenShowsWithoutIt() {
    // A node of the cut wanted whose mesh never comes (lost, say) holds the cut shown back: after the nudge time
    // its mesh is asked for again (a grid chunk is marked dirty for the terrain's own mesher), and after the force
    // time the cut wanted is shown without it. The stall says which node, for the renderer's log.
    QThreadPool pool;
    pool.setMaxThreadCount(4);
    VoxelGrid grid(glm::ivec3(256, 64, 256));
    grid.fillHeightField(hills, 2);
    grid.clearDirtyChunks();   // as if the terrain's mesher had meshed every chunk
    LevelOfDetail lod(&pool);
    LodSettings settings;
    settings.detail = 1.0f;
    settings.stallNudgeMs = 50.0f;
    settings.stallForceMs = 150.0f;
    lod.setSettings(settings);
    const glm::vec3 metres(0.5f);
    glm::vec3 camera(20.0f, 20.0f, 20.0f);
    ChunkKey refused;   // the grid chunk whose mesh never comes
    bool refusing = false;
    auto gridChunkReady = [&](const ChunkKey& key) { return !(refusing && key == refused); };
    auto update = [&] { lod.update(grid, camera, metres, gridChunkReady); };
    auto settle = [&] {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            update();
            if (lod.isSettled()) {
                return true;
            }
            QThread::msleep(1);
        }
        return false;
    };
    auto drawnKeys = [&] {
        std::vector<ChunkKey> keys;
        for (const LevelOfDetail::Drawn& drawn : lod.getDrawn()) {
            keys.push_back(drawn.key);
        }
        return keys;
    };
    auto cutKeys = [&] {
        std::vector<ChunkKey> keys;
        for (const LodCut::Node& node : lod.getCut().getNodes()) {
            keys.push_back(node.key);
        }
        return keys;
    };
    QVERIFY(settle());
    QVERIFY(!lod.getStall().active);
    const std::vector<ChunkKey> before = drawnKeys();

    // the camera at the far corner: the new cut has the grid's own chunks near it, and one of them never gets its mesh
    camera = glm::vec3(236.0f, 20.0f, 236.0f);
    update();
    for (const LodCut::Node& node : lod.getCut().getNodes()) {
        if (node.isGridChunk && node.key.level == 0) {
            refused = node.key;
            refusing = true;
            break;
        }
    }
    QVERIFY(refusing);
    QVERIFY(cutKeys() != before);
    // once every other node has its mesh, the refused chunk is what holds the cut back
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 20000 && !(lod.getStall().active && lod.getStall().key == refused)) {
        update();
        QThread::msleep(1);
    }
    QVERIFY(lod.getStall().active);
    QVERIFY(lod.getStall().key == refused);
    QVERIFY(lod.getStall().isGridChunk);
    QVERIFY(!lod.getStall().nudged);
    QVERIFY(drawnKeys() == before);
    // the nudge: the chunk is marked dirty for the terrain's mesher
    timer.start();
    while (timer.elapsed() < 5000 && !lod.getStall().nudged) {
        update();
        QThread::msleep(1);
    }
    QVERIFY(lod.getStall().nudged);
    QVERIFY(!lod.getStall().forced);
    QVERIFY(grid.getDirtyChunks().count(refused) > 0);
    QVERIFY(drawnKeys() == before);
    // the force: the cut wanted is shown without the chunk
    timer.start();
    while (timer.elapsed() < 5000 && !lod.getStall().forced) {
        update();
        QThread::msleep(1);
    }
    QVERIFY(lod.getStall().forced);
    QVERIFY(!lod.getStall().active);
    QCOMPARE((int)lod.getStall().events, 2);
    QVERIFY(drawnKeys() == cutKeys());
    // its mesh there at last, nothing stalls
    refusing = false;
    QVERIFY(settle());
    QVERIFY(!lod.getStall().active);
}

void TerrainTests::paintTilesOfCoarserNodesAverageTheMap() {
    PaintMap map(glm::ivec2(128, 128), 2);   // 256 texels a side
    // A level -1 node's tile: 36 of its voxels a side at two texels each, a texel the mean of 2 x 2 of the map's,
    // its texel (0, 0) at the map's (-8, -8).
    for (int z = 20; z < 22; ++z) {
        for (int x = 10; x < 12; ++x) {
            map.at(glm::ivec2(x, z))[3] = 200;
        }
    }
    map.at(glm::ivec2(12, 20))[3] = 100;
    map.at(glm::ivec2(12, 20))[6] = 40;
    std::vector<uint8_t> low, high;
    map.fillTile(-1, glm::ivec2(0, 0), low, high);
    QCOMPARE((int)low.size(), 72 * 72 * 4);
    QCOMPARE((int)low[((14 * 72) + 9) * 4 + 3], 200);    // map texels (10, 20) to (11, 21)
    QCOMPARE((int)low[((14 * 72) + 10) * 4 + 3], 25);    // (12, 20) to (13, 21): one of four at 100
    QCOMPARE((int)high[((14 * 72) + 10) * 4 + 2], 10);   // and one at 40 of layer 6
    QCOMPARE((int)low[((14 * 72) + 11) * 4 + 3], 0);
    // At level -3 a texel covers 8 x 8 of the map's and reads 4 x 4 of them, every other one: texel (5, 6) covers
    // the map's (8, 16) to (15, 23), and of the sixteen it reads (10, 20) holds 200 and (12, 20) 100.
    map.fillTile(-3, glm::ivec2(0, 0), low, high);
    QCOMPARE((int)low[((6 * 72) + 5) * 4 + 3], (300 + 8) / 16);
    QCOMPARE((int)low[((5 * 72) + 5) * 4 + 3], 0);
    // level 0 is the column's own tile
    std::vector<uint8_t> own0, own1;
    map.fillTile(0, glm::ivec2(1, 2), low, high);
    map.fillTile(glm::ivec2(1, 2), own0, own1);
    QVERIFY(low == own0 && high == own1);
}

void TerrainTests::raiseAcrossChunkLevelsKeepsTheGroundLevel() {
    // A wide Raise over ground whose chunks are at two levels (a small dig split one place) lifts the ground
    // evenly, with no wall along the chunks' join. A voxel at the byte's limit is not shifted by the brush, only
    // given a distance from its neighbours afterwards, so a chunk reading a neighbour at another level between
    // the passes sees it as far rather than a few of its own voxels off, which once grew a wall as tall as the
    // brush along the join, half a voxel a pass.
    VoxelGrid grid(glm::ivec3(128, 128, 128));
    grid.fillBelow(64.0f);
    Brush small;
    small.center = glm::vec3(48.0f, 64.0f, 56.0f);
    small.radius = 2.0f;
    QVERIFY(sculpt(grid, small, SculptMode::Dig) > 0);
    QCOMPARE(grid.getFinestLevel(), 1);
    QCOMPARE(grid.chunkCovering(glm::ivec3(48, 70, 63)).level, 1);
    QCOMPARE(grid.chunkCovering(glm::ivec3(48, 70, 64)).level, 0);
    auto topGround = [&](int x, int z) {
        for (int y = grid.getVoxelCount().y - 1; y >= 0; --y) {
            if (grid.get(glm::ivec3(x, y, z)).isGround()) {
                return y;
            }
        }
        return -1;
    };

    Brush raise;
    raise.radius = 20.0f;
    raise.strength = 0.5f;
    raise.hardness = 0.5f;
    for (int pass = 0; pass < 30; ++pass) {
        // the editor picks the ground under the pointer again at every step of a stroke
        raise.center = glm::vec3(48.5f, (float)topGround(48, 64) + 0.5f, 64.5f);
        QVERIFY(sculpt(grid, raise, SculptMode::Raise) > 0);
    }
    // half a voxel a pass under the brush's centre, the same on both sides of the join and across it
    const int atJoin = topGround(48, 64);
    QVERIFY2(atJoin >= 64 + 13 && atJoin <= 64 + 17, qPrintable(QString("the ground at the join rose to %1").arg(atJoin)));
    for (int z = 58; z <= 70; ++z) {
        const int top = topGround(48, z);
        QVERIFY2(std::abs(top - atJoin) <= 1, qPrintable(QString("z %1: top %2, at the join %3").arg(z).arg(top).arg(atJoin)));
    }
    // and the air above the ground is at the byte's limit again beyond the band, on both sides
    QCOMPARE((int)grid.get(glm::ivec3(48, atJoin + 5, 64)).distance, (int)DISTANCE_MAX);
    QCOMPARE((int)grid.get(glm::ivec3(48, atJoin + 5, 65)).distance, (int)DISTANCE_MAX);
    QCOMPARE((int)grid.get(glm::ivec3(48, atJoin + 5, 63)).distance, (int)DISTANCE_MAX);
}

void TerrainTests::raiseMendsAirLeftTooNearTheGround() {
    // A field the old brush left behind: air well above the ground at distances a few voxels short of the limit
    // (it read "far" as a few voxels when a neighbour at another level was shifted). The brush now mends such
    // air under it as it passes, so a Raise over it lifts the ground evenly instead of putting it up through the
    // whole column at once.
    VoxelGrid grid(glm::ivec3(96, 96, 96));
    grid.fillBelow(40.0f);
    for (int z = 30; z < 66; ++z) {
        for (int x = 30; x < 66; ++x) {
            for (int y = 44; y < 60; ++y) {
                Voxel damaged = grid.get(glm::ivec3(x, y, z));
                damaged.setDistance(x < 48 ? 3.5f : 3.0f + 0.03f * (float)(y - 44));
                grid.set(glm::ivec3(x, y, z), damaged);
            }
        }
    }
    auto topGround = [&](int x, int z) {
        for (int y = grid.getVoxelCount().y - 1; y >= 0; --y) {
            if (grid.get(glm::ivec3(x, y, z)).isGround()) {
                return y;
            }
        }
        return -1;
    };
    Brush raise;
    raise.radius = 14.0f;
    raise.strength = 0.5f;
    raise.hardness = 0.5f;
    for (int pass = 0; pass < 24; ++pass) {
        raise.center = glm::vec3(48.5f, (float)topGround(48, 48) + 0.5f, 48.5f);
        QVERIFY(sculpt(grid, raise, SculptMode::Raise) > 0);
    }
    const int atCentre = topGround(48, 48);
    QVERIFY2(atCentre >= 39 + 10 && atCentre <= 39 + 14, qPrintable(QString("the ground rose to %1").arg(atCentre)));
    for (int z = 44; z <= 52; ++z) {
        for (int x = 44; x <= 52; ++x) {
            const int top = topGround(x, z);
            QVERIFY2(std::abs(top - atCentre) <= 1, qPrintable(QString("(%1, %2): top %3, at the centre %4").arg(x).arg(z).arg(top).arg(atCentre)));
        }
    }
    // the damaged air under the brush is mended: at the limit again beyond the band
    QCOMPARE((int)grid.get(glm::ivec3(48, atCentre + 6, 48)).distance, (int)DISTANCE_MAX);
    QCOMPARE((int)grid.get(glm::ivec3(40, topGround(40, 48) + 6, 48)).distance, (int)DISTANCE_MAX);
    // and a true field under it is left as it is: a 45 degree slope's distances grow by 0.71 a voxel
    VoxelGrid slope(glm::ivec3(64, 64, 64));
    slope.fillHeightField([](int x, int) { return (float)x; }, 0);
    const float before = slope.get(glm::ivec3(20, 24, 32)).getDistance();
    Brush touch;
    touch.center = glm::vec3(20.0f, 20.0f, 32.0f);
    touch.radius = 6.0f;
    touch.strength = 0.1f;
    sculpt(slope, touch, SculptMode::Raise);
    QVERIFY2(std::abs(slope.get(glm::ivec3(20, 24, 32)).getDistance() - (before - 0.1f)) < 0.1f,
             qPrintable(QString("%1 -> %2").arg(before).arg(slope.get(glm::ivec3(20, 24, 32)).getDistance())));
}

void TerrainTests::smoothLevelsAHillUnderAWideBrush() {
    // Smooth blurs the field with a box a quarter of the brush's radius wide each way (a voxel at least), so a wide
    // brush at full strength takes a hill down in a few passes rather than polishing voxel-sized dents for ever,
    // while flat ground under it stays exactly where it is.
    VoxelGrid grid(glm::ivec3(96, 96, 96));
    grid.fillBelow(40.0f);
    auto topGround = [&](int x, int z) {
        for (int y = grid.getVoxelCount().y - 1; y >= 0; --y) {
            if (grid.get(glm::ivec3(x, y, z)).isGround()) {
                return y;
            }
        }
        return -1;
    };
    Brush raise;
    raise.radius = 10.0f;
    raise.strength = 1.0f;
    raise.hardness = 0.8f;
    for (int pass = 0; pass < 12; ++pass) {
        raise.center = glm::vec3(48.5f, (float)topGround(48, 48) + 0.5f, 48.5f);
        QVERIFY(sculpt(grid, raise, SculptMode::Raise) > 0);
    }
    const int hill = topGround(48, 48);
    QVERIFY2(hill >= 40 + 8, qPrintable(QString("the hill reached %1").arg(hill)));
    const int flatTop = topGround(22, 48);
    const float flatDistance = grid.get(glm::ivec3(22, flatTop + 1, 48)).getDistance();

    Brush smooth;
    smooth.radius = 24.0f;
    smooth.strength = 1.0f;
    smooth.hardness = 1.0f;
    for (int pass = 0; pass < 6; ++pass) {
        smooth.center = glm::vec3(48.5f, (float)topGround(48, 48) + 0.5f, 48.5f);
        QVERIFY(sculpt(grid, smooth, SculptMode::Smooth) > 0);
    }
    const int after = topGround(48, 48);
    QVERIFY2(after - 40 <= (hill - 40) / 2, qPrintable(QString("the hill went from %1 to %2").arg(hill).arg(after)));
    // the hill's volume spreads under the brush (a blur keeps the field's sum), so the ground near the brush's edge
    // comes up a little; outside the brush it is untouched
    for (int x = 20; x <= 23; ++x) {
        QCOMPARE(topGround(x, 48), flatTop);
        QCOMPARE(grid.get(glm::ivec3(x, flatTop + 1, 48)).getDistance(), flatDistance);
    }

    // a small brush keeps a one-voxel blur: a dent a voxel wide and deep is softened, the ground around it kept
    VoxelGrid flat(glm::ivec3(64, 64, 64));
    flat.fillBelow(32.0f);
    Brush small;
    small.center = glm::vec3(32.5f, 32.5f, 32.5f);
    small.radius = 2.0f;
    small.strength = 1.0f;
    const float surface = flat.get(glm::ivec3(32, 32, 32)).getDistance();
    QVERIFY(sculpt(flat, small, SculptMode::Smooth) >= 0);
    QCOMPARE(flat.get(glm::ivec3(32, 32, 32)).getDistance(), surface);
}

void TerrainTests::layersRoundTripThroughTheFile() {
    // The default stack is eight well-formed layers in slot order. A stack goes into the file (version 4) and
    // comes back equal, names, order and all; a file without layers leaves the stack empty, as does a stack that
    // is not well formed, which is not written.
    LayerStack stack = LayerStack::defaults();
    QVERIFY(stack.isValid());
    QCOMPARE((int)stack.layers.size(), 8);
    QCOMPARE(stack.freeSlot(), -1);
    stack.layers[2].name = "Granite \xc3\xa9";
    stack.layers[2].blend = LayerBlend::Overlay;
    stack.layers[2].opacity = 0.35f;
    stack.layers[2].offset = glm::vec2(1.5f, -2.25f);
    stack.layers[5].visible = false;
    stack.layers[5].materialURL = "https://example.com/mud.jpg";
    stack.layers[6].normalStrength = 0.0f;
    stack.layers[4].tint = glm::vec3(0.8f, 0.9f, 1.0f);
    std::swap(stack.layers[0], stack.layers[7]);    // snow at the bottom, grass on top
    stack.layers.erase(stack.layers.begin() + 3);   // seven layers, slot 3 free
    QVERIFY(stack.isValid());
    QCOMPARE(stack.freeSlot(), 3);
    QCOMPARE(stack.indexOf(7), 0);
    QVERIFY(stack.find(3) == nullptr);
    QCOMPARE(stack.order().size(), (size_t)7);

    VoxelGrid grid(glm::ivec3(64, 64, 64));
    grid.fillBelow(32.0f);
    PaintMap map(glm::ivec2(64, 64), 2);
    map.at(glm::ivec2(10, 10))[1] = 200;
    map.markAllColumnsDirty();
    const Bytes bytes = saveTerrain(grid, &map, &stack);
    VoxelGrid loaded;
    PaintMap loadedMap;
    LayerStack loadedStack;
    std::string error;
    QVERIFY2(loadTerrain(bytes, loaded, &loadedMap, &loadedStack, &error), error.c_str());
    QVERIFY(loadedStack == stack);
    QCOMPARE((int)loadedMap.at(glm::ivec2(10, 10))[1], 200);   // version 4 keeps coverages as they are

    const Bytes without = saveTerrain(grid, &map);
    QVERIFY2(loadTerrain(without, loaded, &loadedMap, &loadedStack, &error), error.c_str());
    QVERIFY(loadedStack.isEmpty());

    LayerStack twice = LayerStack::defaults();
    twice.layers[1].slot = 0;
    QVERIFY(!twice.isValid());
    const Bytes rejected = saveTerrain(grid, &map, &twice);
    QVERIFY2(loadTerrain(rejected, loaded, &loadedMap, &loadedStack, &error), error.c_str());
    QVERIFY(loadedStack.isEmpty());

    // cut short in the layers: refused
    Bytes cut(bytes.begin(), bytes.end() - 5);
    QVERIFY(!loadTerrain(cut, loaded, &loadedMap, &loadedStack, &error));
}

void TerrainTests::layerMapsTakeFileVersionFive() {
    // A stack whose layers have no maps, the normal map on and no Height blend writes version 4, as before; a map on
    // any layer, the normal map turned off, or the Height blend writes version 5, which carries all of it back; a
    // version 4 file reads with the normal map on and no maps.
    auto versionOf = [](const Bytes& bytes) {
        return (uint32_t)bytes[4] | ((uint32_t)bytes[5] << 8) | ((uint32_t)bytes[6] << 16) | ((uint32_t)bytes[7] << 24);
    };
    VoxelGrid grid(glm::ivec3(32, 32, 32));
    grid.fillBelow(16.0f);
    PaintMap map(glm::ivec2(32, 32), 1);

    LayerStack plain = LayerStack::defaults();
    QVERIFY(plain.layers[0].hasMaps());   // the defaults carry maps
    for (Layer& layer : plain.layers) {
        layer.normalURL.clear();
        layer.roughnessURL.clear();
        layer.occlusionURL.clear();
        layer.heightURL.clear();
    }
    QVERIFY(!plain.layers[0].hasMaps());
    const Bytes four = saveTerrain(grid, &map, &plain);
    QCOMPARE(versionOf(four), 4u);

    VoxelGrid loaded;
    PaintMap loadedMap;
    LayerStack loadedStack;
    std::string error;
    QVERIFY2(loadTerrain(four, loaded, &loadedMap, &loadedStack, &error), error.c_str());
    // a version 4 file's default materials come back with their maps (snow without an occlusion map), as if saved
    // with them; a layer with any other texture, or a map of its own, is left alone
    QVERIFY(loadedStack.layers[3].useNormal);
    QCOMPARE(loadedStack.layers[3].normalURL, LayerStack::defaults().layers[3].normalURL);
    QVERIFY(loadedStack.layers[7].occlusionURL.empty());
    QVERIFY(loadedStack == LayerStack::defaults());
    LayerStack custom = plain;
    custom.layers[1].materialURL = "https://example.com/dirt.jpg";
    custom.layers[2].heightURL = "https://example.com/rock-height.png";
    QVERIFY(custom.fillDefaultMaps());
    QVERIFY(custom.layers[1].normalURL.empty());
    QVERIFY(custom.layers[2].normalURL.empty());
    QVERIFY(custom.layers[2].heightURL == "https://example.com/rock-height.png");
    QVERIFY(!custom.layers[0].normalURL.empty());
    QVERIFY(!custom.fillDefaultMaps());   // nothing left to fill

    LayerStack withMaps = plain;
    withMaps.layers[2].normalURL = "https://example.com/rock-normal.png";
    withMaps.layers[2].useNormal = false;
    withMaps.layers[2].normalStrength = 1.5f;
    withMaps.layers[4].roughnessURL = "https://example.com/gravel-roughness.png";
    withMaps.layers[4].occlusionURL = "https://example.com/gravel-ao.png";
    withMaps.layers[4].heightURL = "https://example.com/gravel-height.png";
    withMaps.layers[4].blend = LayerBlend::Height;
    const Bytes five = saveTerrain(grid, &map, &withMaps);
    QCOMPARE(versionOf(five), 5u);
    QVERIFY2(loadTerrain(five, loaded, &loadedMap, &loadedStack, &error), error.c_str());
    QVERIFY(loadedStack == withMaps);
    QVERIFY(!loadedStack.layers[2].useNormal);
    QCOMPARE(loadedStack.layers[4].blend, LayerBlend::Height);

    // the toggle alone, and the blend alone, each need version 5; a version 5 stack with no map on any layer gets the
    // defaults' maps as a version 4 one does
    LayerStack toggled = plain;
    toggled.layers[0].useNormal = false;
    const Bytes toggledBytes = saveTerrain(grid, &map, &toggled);
    QCOMPARE(versionOf(toggledBytes), 5u);
    QVERIFY2(loadTerrain(toggledBytes, loaded, &loadedMap, &loadedStack, &error), error.c_str());
    QVERIFY(!loadedStack.layers[0].useNormal);
    QCOMPARE(loadedStack.layers[0].normalURL, LayerStack::defaults().layers[0].normalURL);
    LayerStack byHeight = plain;
    byHeight.layers[0].blend = LayerBlend::Height;
    QCOMPARE(versionOf(saveTerrain(grid, &map, &byHeight)), 5u);

    // the defaults themselves round-trip, maps and all
    const LayerStack defaults = LayerStack::defaults();
    const Bytes bytes = saveTerrain(grid, &map, &defaults);
    QCOMPARE(versionOf(bytes), 5u);
    QVERIFY2(loadTerrain(bytes, loaded, &loadedMap, &loadedStack, &error), error.c_str());
    QVERIFY(loadedStack == defaults);
    QVERIFY(loadedStack.layers[7].occlusionURL.empty());   // snow has none
    QVERIFY(!loadedStack.layers[6].occlusionURL.empty());

    // cut short in the maps: refused
    Bytes cut(five.begin(), five.end() - 3);
    QVERIFY(!loadTerrain(cut, loaded, &loadedMap, &loadedStack, &error));
}

void TerrainTests::sharesBecomeCoveragesThatDrawTheSame() {
    // A map from a file before version 4 holds shares of each texel, summing to 255 at most with the voxel material
    // taking the rest; converted to coverages drawn bottom to top in slot order, the stack composes the same colour.
    PaintMap map(glm::ivec2(4, 4), 1);
    uint8_t* shares = map.at(glm::ivec2(1, 1));
    shares[0] = 64;    // grass
    shares[1] = 128;   // dirt; the voxel material keeps 63
    map.at(glm::ivec2(2, 2))[3] = 255;
    std::vector<uint8_t> order;
    for (int slot = 0; slot < PAINT_LAYERS; ++slot) {
        order.push_back((uint8_t)slot);
    }
    map.sharesToCoverage(order);
    const uint8_t* coverage = map.at(glm::ivec2(1, 1));
    QCOMPARE((int)coverage[0], 129);   // 64 of the 127 drawn up to and including grass
    QCOMPARE((int)coverage[1], 128);   // 128 of all 255
    QCOMPARE((int)coverage[2], 0);
    const float v = 0.2f, g = 0.6f, d = 0.9f;   // a channel of the voxel material, grass and dirt
    const float composed = glm::mix(glm::mix(v, g, 129.0f / 255.0f), d, 128.0f / 255.0f);
    const float shared = (63.0f * v + 64.0f * g + 128.0f * d) / 255.0f;
    QVERIFY2(std::abs(composed - shared) < 0.01f, qPrintable(QString("%1 composed against %2 shared").arg(composed).arg(shared)));
    QCOMPARE((int)map.at(glm::ivec2(2, 2))[3], 255);
    QCOMPARE((int)map.at(glm::ivec2(0, 0))[0], 0);

    // a file before version 4 converts as it is read
    VoxelGrid grid(glm::ivec3(32, 32, 32));
    grid.fillBelow(16.0f);
    PaintMap old(glm::ivec2(32, 32), 1);
    uint8_t* texel = old.at(glm::ivec2(5, 5));
    texel[0] = 64;
    texel[1] = 128;
    old.markAllColumnsDirty();
    const Bytes bytes = saveTerrain(grid, &old);
    VoxelGrid loaded;
    PaintMap loadedMap;
    std::string error;
    QVERIFY2(loadTerrain(bytes, loaded, &loadedMap, nullptr, &error), error.c_str());
    QCOMPARE((int)loadedMap.at(glm::ivec2(5, 5))[0], 129);
    QCOMPARE((int)loadedMap.at(glm::ivec2(5, 5))[1], 128);
}

void TerrainTests::paintCoversOneLayerAlone() {
    // Painting a layer raises its own coverage and leaves the others as they are; erasing lowers its own alone.
    PaintMap map(glm::ivec2(32, 32), 2);
    for (int z = 0; z < 64; ++z) {
        for (int x = 0; x < 64; ++x) {
            map.at(glm::ivec2(x, z))[0] = 200;
        }
    }
    PaintBrush brush;
    brush.center = glm::vec2(16.0f, 16.0f);
    brush.radius = 4.0f;
    brush.strength = 1.0f;
    brush.hardness = 1.0f;
    brush.layer = 1;
    QVERIFY(paint(map, brush, false) > 0);
    const uint8_t* centre = map.at(glm::ivec2(32, 32));
    QCOMPARE((int)centre[0], 200);
    QCOMPARE((int)centre[1], 255);
    brush.strength = 0.5f;
    QVERIFY(paint(map, brush, true) > 0);
    QCOMPARE((int)centre[0], 200);
    QCOMPARE((int)centre[1], 128);   // half the way back toward none
    QCOMPARE((int)map.at(glm::ivec2(2, 2))[1], 0);   // outside the brush
}

void TerrainTests::brushMaskShapesTheBrushAndItsOutline() {
    // A square picture reaches the brush's corners, which a round brush does not; turned, it reaches further along
    // the axes instead; a picture with a hole leaves the middle alone; hardness pushes the greys.
    const int side = 16;
    BrushMask square(side, side, std::vector<float>((size_t)side * side, 1.0f));
    std::vector<float> ringValues((size_t)side * side, 1.0f);
    for (int y = 6; y < 10; ++y) {
        for (int x = 6; x < 10; ++x) {
            ringValues[(size_t)y * side + x] = 0.0f;
        }
    }
    BrushMask ring(side, side, ringValues);
    QVERIFY(!square.isEmpty());
    QCOMPARE(square.sample(0.0f, 0.0f), 1.0f);
    QCOMPARE(square.sample(1.5f, 0.0f), 0.0f);
    QCOMPARE(ring.sample(0.0f, 0.0f), 0.0f);
    QVERIFY(ring.sample(0.9f, 0.9f) > 0.99f);
    QCOMPARE(BrushMask(3, 3, std::vector<float>(4, 1.0f)).isEmpty(), true);   // the wrong number of values

    VoxelGrid grid(glm::ivec3(64, 64, 64));
    grid.fillBelow(40.5f, 1);
    Brush brush;
    brush.center = glm::vec3(32.0f, 40.5f, 32.0f);
    brush.radius = 6.0f;
    brush.strength = 1.0f;
    QVERIFY(sculpt(grid, brush, SculptMode::Raise) > 0);
    QVERIFY(grid.get(glm::ivec3(32, 41, 32)).isGround());    // under the middle
    QVERIFY(!grid.get(glm::ivec3(37, 41, 37)).isGround());   // the corner, 7.1 voxels out: beyond the round brush

    grid.fillBelow(40.5f, 1);
    brush.mask = &square;
    QVERIFY(sculpt(grid, brush, SculptMode::Raise) > 0);
    QVERIFY(grid.get(glm::ivec3(32, 41, 32)).isGround());
    QVERIFY(grid.get(glm::ivec3(37, 41, 37)).isGround());    // the square reaches the corner
    QVERIFY(!grid.get(glm::ivec3(39, 41, 32)).isGround());   // but not past its side
    QVERIFY(!grid.get(glm::ivec3(32, 44, 32)).isGround());   // the column's soft end: raised a voxel, not more

    grid.fillBelow(40.5f, 1);
    brush.angle = glm::radians(45.0f);
    QVERIFY(sculpt(grid, brush, SculptMode::Raise) > 0);
    QVERIFY(!grid.get(glm::ivec3(37, 41, 37)).isGround());   // the turned square's side has moved in from the corner
    QVERIFY(grid.get(glm::ivec3(39, 41, 32)).isGround());    // and its corner out along the axis

    grid.fillBelow(40.5f, 1);
    brush.angle = 0.0f;
    brush.mask = &ring;
    QVERIFY(sculpt(grid, brush, SculptMode::Raise) > 0);
    QVERIFY(!grid.get(glm::ivec3(32, 41, 32)).isGround());   // the hole
    QVERIFY(grid.get(glm::ivec3(36, 41, 32)).isGround());
    QVERIFY(grid.get(glm::ivec3(37, 41, 37)).isGround());

    // the paint brush: the same picture over the map, and hardness as a curve on its greys
    PaintMap map(glm::ivec2(64, 64), 1);
    PaintBrush paintBrush;
    paintBrush.center = glm::vec2(32.0f, 32.0f);
    paintBrush.radius = 6.0f;
    paintBrush.strength = 1.0f;
    paintBrush.layer = 2;
    QVERIFY(paint(map, paintBrush, false) > 0);
    QCOMPARE((int)map.at(glm::ivec2(32, 32))[2], 255);
    QCOMPARE((int)map.at(glm::ivec2(37, 37))[2], 0);         // the corner, outside the disc
    paintBrush.mask = &square;
    QVERIFY(paint(map, paintBrush, false) > 0);
    QCOMPARE((int)map.at(glm::ivec2(37, 37))[2], 255);
    QCOMPARE((int)map.at(glm::ivec2(39, 32))[2], 0);
    BrushMask grey(side, side, std::vector<float>((size_t)side * side, 0.5f));
    paintBrush.mask = &grey;
    paintBrush.layer = 3;
    paintBrush.center = glm::vec2(10.0f, 10.0f);
    QVERIFY(paint(map, paintBrush, false) > 0);
    QCOMPARE((int)map.at(glm::ivec2(10, 10))[3], 128);      // half hardness: the grey as it is
    paintBrush.hardness = 1.0f;
    paintBrush.center = glm::vec2(10.0f, 50.0f);
    QVERIFY(paint(map, paintBrush, false) > 0);
    QVERIFY(map.at(glm::ivec2(10, 50))[3] > 200);           // harder: toward white
    paintBrush.hardness = 0.0f;
    paintBrush.center = glm::vec2(50.0f, 10.0f);
    QVERIFY(paint(map, paintBrush, false) > 0);
    QVERIFY(map.at(glm::ivec2(50, 10))[3] < 30);            // softer: toward black

    // the outline: one closed curve around the square, two for the ring (its edge and its hole)
    const auto squareCurves = square.outline(0.5f, 16);
    QCOMPARE((int)squareCurves.size(), 1);
    QVERIFY(squareCurves[0].size() >= 4);
    for (const glm::vec2& point : squareCurves[0]) {
        QVERIFY(std::abs(point.x) <= 1.0f + 1.0e-4f && std::abs(point.y) <= 1.0f + 1.0e-4f);
        QVERIFY(std::abs(point.x) >= 0.85f || std::abs(point.y) >= 0.85f);   // along the square's edge
    }
    const auto ringCurves = ring.outline(0.5f, 16);
    QCOMPARE((int)ringCurves.size(), 2);
    QVERIFY(BrushMask().outline(0.5f, 16).empty());
}

void TerrainTests::topSurfaceIsTheGroundTheSkySees() {
    VoxelGrid grid(glm::ivec3(64, 64, 64));
    grid.fillBelow(40.5f, 1);
    std::vector<std::pair<glm::ivec3, glm::ivec3>> boxes;
    QVERIFY(!grid.takeChanges(boxes));   // the level of detail's log: a new grid has changed everywhere
    TopSurface top;
    top.update(grid);   // the first call finds every column
    QVERIFY(top.allColumnsDirty());
    top.clearAllColumnsDirty();
    QVERIFY(std::abs(top.heightAt(glm::ivec2(10, 10)) - 40.5f) < 0.3f);
    QVERIFY(std::abs(top.heightAt(glm::vec2(10.5f, 20.25f)) - 40.5f) < 0.3f);

    // a cave under the ground: a sphere dug well below it, a roof five voxels thick over it
    Brush brush;
    brush.center = glm::vec3(32.0f, 30.0f, 32.0f);
    brush.radius = 5.0f;
    QVERIFY(sculpt(grid, brush, SculptMode::Dig) > 0);
    top.update(grid);
    QVERIFY(!top.allColumnsDirty());
    QVERIFY(top.getDirtyColumns().count(glm::ivec2(1, 1)) > 0);
    top.clearDirtyColumns();
    QVERIFY(std::abs(top.heightAt(glm::ivec2(32, 32)) - 40.5f) < 0.3f);   // over the cave the ground is still the top
    QVERIFY(!top.isOnTop(glm::vec3(32.0f, 25.5f, 32.0f)));               // the cave's floor is under it
    QVERIFY(top.isOnTop(glm::vec3(32.0f, 40.5f, 32.0f)));                // the ground is on it
    QVERIFY(top.isOnTop(glm::vec3(32.0f, 39.8f, 32.0f)));                // within the tolerance
    QVERIFY(top.isOnTop(glm::vec3(32.0f, 45.0f, 32.0f)));                // above it

    // a hole through the roof: under it the cave's floor is what the sky sees now. The level of detail takes its own
    // change log first, and the top surface still gets its edits.
    brush.center = glm::vec3(32.0f, 38.0f, 32.0f);
    brush.radius = 4.0f;
    QVERIFY(sculpt(grid, brush, SculptMode::Dig) > 0);
    QVERIFY(grid.takeChanges(boxes));
    QVERIFY(!boxes.empty());
    top.update(grid);
    QVERIFY(top.heightAt(glm::ivec2(32, 32)) < 30.0f);
    QVERIFY(top.isOnTop(glm::vec3(32.0f, top.heightAt(glm::ivec2(32, 32)), 32.0f)));
    QVERIFY(std::abs(top.heightAt(glm::ivec2(10, 10)) - 40.5f) < 0.3f);   // far from it nothing moved
    QVERIFY(!top.getDirtyColumns().empty());

    // a tile: the column's heights, with the neighbours' apron around them and the grid's edge repeating past it
    const int size = TopSurface::getTileSize();
    std::vector<float> tile;
    top.fillTile(glm::ivec2(1, 1), tile);
    QCOMPARE((int)tile.size(), size * size * 2);   // the top and the sheltered floor, a texel
    QCOMPARE(tile[((size_t)PAINT_TILE_APRON * size + PAINT_TILE_APRON) * 2], top.heightAt(glm::ivec2(32, 32)));
    QCOMPARE(tile[0], top.heightAt(glm::ivec2(32 - PAINT_TILE_APRON, 32 - PAINT_TILE_APRON)));
    top.fillTile(glm::ivec2(0, 0), tile);
    QCOMPARE(tile[0], top.heightAt(glm::ivec2(0, 0)));   // beyond the grid: the edge column's

    // a column of air all the way down is under every surface
    VoxelGrid empty(glm::ivec3(32, 32, 32));
    TopSurface none;
    none.update(empty);
    QCOMPARE(none.heightAt(glm::ivec2(5, 5)), NO_GROUND_HEIGHT);
    QVERIFY(none.isOnTop(glm::vec3(5.0f, 0.0f, 5.0f)));

    // ground up to the grid's top: the height is at the top
    VoxelGrid full(glm::ivec3(32, 40, 32), Voxel::ground(2));
    TopSurface high;
    high.update(full);
    QVERIFY(high.heightAt(glm::ivec2(3, 3)) >= 39.0f);
}

void TerrainTests::paintVoxelsPaintsTheStrengthsShareOfTheVoxels() {
    // a voxel holds one material, so a step at part strength paints that share of the ground voxels under the brush
    // (each by chance), and steps with new seeds build the share up as the paint map's coverage builds
    Brush brush;
    brush.center = glm::vec3(32.0f, 40.5f, 32.0f);
    brush.radius = 8.0f;
    brush.hardness = 1.0f;   // the whole radius at its full effect
    brush.material = 3;

    VoxelGrid full(glm::ivec3(64, 64, 64));
    full.fillBelow(40.5f, 1);
    brush.strength = 1.0f;
    const int all = sculpt(full, brush, SculptMode::PaintVoxels);
    QVERIFY(all > 500);

    VoxelGrid half(glm::ivec3(64, 64, 64));
    half.fillBelow(40.5f, 1);
    brush.strength = 0.5f;
    brush.seed = 1;
    const int first = sculpt(half, brush, SculptMode::PaintVoxels);
    QVERIFY(std::abs((float)first / (float)all - 0.5f) < 0.06f);
    QCOMPARE(sculpt(half, brush, SculptMode::PaintVoxels), 0);   // the same seed paints the same voxels
    brush.seed = 2;
    const int second = sculpt(half, brush, SculptMode::PaintVoxels);
    QVERIFY(std::abs((float)(first + second) / (float)all - 0.75f) < 0.06f);

    // the soft edge: nearly every voxel near the middle, few near the rim
    VoxelGrid soft(glm::ivec3(64, 64, 64));
    soft.fillBelow(40.5f, 1);
    brush.strength = 1.0f;
    brush.hardness = 0.0f;
    brush.seed = 3;
    QVERIFY(sculpt(soft, brush, SculptMode::PaintVoxels) > 0);
    int middle = 0, middlePainted = 0, rim = 0, rimPainted = 0;
    for (int z = 22; z <= 42; ++z) {
        for (int y = 30; y <= 40; ++y) {
            for (int x = 22; x <= 42; ++x) {
                const float distance = glm::length(glm::vec3(x, y, z) - brush.center);
                const bool painted = soft.get(glm::ivec3(x, y, z)).material == 3;
                if (distance < 2.0f) {
                    ++middle;
                    middlePainted += painted ? 1 : 0;
                } else if (distance > 6.0f && distance < 8.0f) {
                    ++rim;
                    rimPainted += painted ? 1 : 0;
                }
            }
        }
    }
    QVERIFY(middle > 0 && rim > 0);
    QVERIFY((float)middlePainted / (float)middle > 0.8f);
    QVERIFY((float)rimPainted / (float)rim < 0.2f);
    QCOMPARE((int)soft.get(glm::ivec3(32, 45, 32)).material, 1);   // air keeps its material
}

void TerrainTests::snapshotKeepsReadingTheGridAsItWasWhenTaken() {
    // A snapshot of a grid (GridView) reads the grid as it was when it was taken, whatever the grid does afterwards:
    // a voxel written in place (the grid copies the chunk first, since the snapshot shares it), a chunk split, a
    // chunk put in its place, the box's faces closed, the grid reset; the grid itself reads the new state. A node
    // sampled from the snapshot gets the samples the grid gave before the edits, which is what a sampling job on
    // the pool relies on while the editing thread goes on.
    VoxelGrid grid(glm::ivec3(96, 64, 96));
    grid.fillHeightField(hills, 2);
    // three chunks the surface crosses (dense ones): one to write a voxel of, one to split, one to replace
    std::vector<ChunkKey> dense;
    grid.forEachChunk([&](const ChunkKey& key, const Chunk& chunk) {
        if (!chunk.isUniform()) {
            dense.push_back(key);
        }
    });
    QVERIFY(dense.size() >= 3);
    const ChunkKey edited = dense[0];
    const ChunkKey split = dense[1];
    const ChunkKey replaced = dense[2];
    const ChunkKey node = edited;
    const NodeContext context = gridContext(grid, node);
    ChunkSamples before;
    sampleNode(grid, node, context, before);
    const int chunks = grid.countChunks();
    const glm::ivec3 voxel = edited.getOrigin() + glm::ivec3(5, 7, 9);
    const Voxel was = grid.get(voxel);
    QVERIFY(was != Voxel::ground(5));

    const GridView snapshot = grid.snapshot();
    QCOMPARE(snapshot.getVoxelCount(), grid.getVoxelCount());
    QCOMPARE(snapshot.countChunks(), chunks);
    // the snapshot shares the chunk; writing to it through the grid gives the grid a chunk of its own
    const Chunk* shared = &snapshot.getChunk(edited);
    QVERIFY(&grid.getChunk(edited) == shared);
    Chunk* mine = grid.findChunk(edited);
    QVERIFY(mine != nullptr);
    QVERIFY(mine != shared);
    QVERIFY(grid.findChunk(edited) == mine);   // and keeps it: nothing else shares it now
    const glm::ivec3 local = VoxelGrid::localOf(voxel);
    QVERIFY(mine->set(local.x, local.y, local.z, Voxel::ground(5)));
    grid.voxelsChanged(edited, local, local);
    QVERIFY(grid.get(voxel) == Voxel::ground(5));
    QVERIFY(snapshot.get(voxel) == was);
    QVERIFY(&snapshot.getChunk(edited) == shared);

    QVERIFY(grid.splitChunk(split));
    QVERIFY(!grid.hasChunk(split) && grid.isSplit(split));
    QVERIFY(snapshot.hasChunk(split) && !snapshot.isSplit(split));
    QVERIFY(snapshot.chunkCovering(split.getOrigin()) == split);

    grid.setChunk(replaced, Chunk(Voxel::air()));
    QVERIFY(grid.getChunk(replaced).isUniform());
    QVERIFY(!snapshot.getChunk(replaced).isUniform());

    grid.setCaps(CAP_ALL);
    QCOMPARE((int)grid.getCaps(), (int)CAP_ALL);
    QCOMPARE((int)snapshot.getCaps(), (int)CAP_NONE);

    ChunkSamples after;
    sampleNode(snapshot, node, context, after);
    QString why;
    QVERIFY2(samplesEqual(before, after, why), qPrintable(why));
    ChunkSamples changed;
    sampleNode(grid, node, context, changed);
    QVERIFY(!samplesEqual(before, changed, why));

    grid.reset(glm::ivec3(32));
    QCOMPARE(grid.countChunks(), 1);
    QCOMPARE(snapshot.countChunks(), chunks);
    QVERIFY(snapshot.get(voxel) == was);

    // Readers on a pool sample from one snapshot while this thread sculpts the grid: every one of them reads the
    // grid as it was when the snapshot was taken.
    VoxelGrid live(glm::ivec3(128, 64, 128));
    live.fillHeightField(hills, 2);
    std::vector<ChunkKey> keys;   // the dense chunks: the hills cross one chunk in each of the 16 columns
    live.forEachChunk([&](const ChunkKey& key, const Chunk& chunk) {
        if (!chunk.isUniform()) {
            keys.push_back(key);
        }
    });
    QVERIFY(keys.size() >= 12);
    std::vector<ChunkSamples> expected(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        sampleNode(live, keys[i], gridContext(live, keys[i]), expected[i]);
    }
    auto taken = std::make_shared<const GridView>(live.snapshot());
    QThreadPool pool;
    pool.setMaxThreadCount(4);
    std::atomic<int> mismatches { 0 };
    std::atomic<int> done { 0 };
    const int rounds = 8;
    for (int round = 0; round < rounds; ++round) {
        pool.start([&, taken] {
            ChunkSamples samples;
            QString reason;
            for (size_t i = 0; i < keys.size(); ++i) {
                sampleNode(*taken, keys[i], gridContext(*taken, keys[i]), samples);
                if (!samplesEqual(expected[i], samples, reason)) {
                    ++mismatches;
                }
            }
            ++done;
        });
    }
    taken.reset();   // the jobs hold it
    Brush brush;
    brush.radius = 6.0f;
    for (int stroke = 0; done < rounds && stroke < 400; ++stroke) {
        brush.center = glm::vec3(20.0f + (stroke % 50), hills(20 + stroke % 50, 30), 30.0f);
        sculpt(live, brush, stroke % 2 ? SculptMode::Dig : SculptMode::Raise);
        QThread::msleep(1);
    }
    pool.waitForDone();
    QCOMPARE((int)mismatches, 0);
    QCOMPARE((int)done, rounds);
}
