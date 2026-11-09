//
//  TerrainTests.h
//  tests/terrain/src
//
//  The terrain library: chunks, the voxel grid and its levels of chunks, surface-nets meshing (on this thread and on
//  a pool), brushes, the paint map, resampling, the terrain file and the level of detail by distance.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_TerrainTests_h
#define hifi_TerrainTests_h

#include <QtTest/QtTest>

class TerrainTests : public QObject {
    Q_OBJECT
private slots:
    void chunkStaysUniformUntilAVoxelDiffers();
    void gridFillsBelowAHeightAndDirtiesNeighboursAcrossChunkFaces();
    void flatGroundMeshesToOneLevelPlane();
    void neighbouringChunksMeetWithoutASeam();
    void trianglesFaceTheAirLikeTheirNormals();
    void brushesDigFillAndRaise();
    void fileRoundTripKeepsEveryVoxel();
    void asyncMesherMatchesTheMesherAndRedoesChunksEditedMeanwhile();
    void paintMapPaintsErasesTilesAndRoundTripsThroughTheFile();
    void resampledGridKeepsTheSurfaceAndItsMaterials();
    void resampledPaintMapKeepsItsPaint();
    void chunkKeysNameTheirGridVoxels();
    void splitChunkKeepsTheGroundAndWhatTheNeighboursSample();
    void refineKeepsTheTwoToOneRule();
    void smallBrushDigsFineDetailWithoutResamplingTheGrid();
    void raiseAcrossChunkLevelsKeepsTheGroundLevel();
    void raiseMendsAirLeftTooNearTheGround();
    void chunksOfDifferentLevelsMeetWithoutACrack();
    void setChunkAndPlaceChunkReplaceWhatCoversTheKey();
    void fileRoundTripKeepsChunkLevels();
    void resampleReadsTheFineChunks();
    void chunkKeysNameCoarserNodes();
    void lodNodeSamplesMatchTheGridsChunks();
    void lodCutCoversTheGridOnceAndKeepsTheTwoToOneRule();
    void lodCutKeepsItsSplitsWithinTheHysteresis();
    void lodMeshesMeetWithoutACrack();
    void levelOfDetailShowsANewCutOnceItsMeshesAreIn();
    void levelOfDetailSettlesAfterAnUndoBurst();
    void levelOfDetailDrawsTheGroundWholeAfterSmallRaises();
    void levelOfDetailDrawsATerrainFileWhole();
    void closedFacesCapTheGround();
    void topSurfaceFollowsRaisedPillars();
    void shelteredFloorsUnderALipAreTheSkysGround();
    void writeCleanTestTerrain();
    void fileRoundTripKeepsTheClosedFaces();
    void levelOfDetailAsksAgainForAStalledNodeAndThenShowsWithoutIt();
    void paintTilesOfCoarserNodesAverageTheMap();
    void smoothLevelsAHillUnderAWideBrush();
    void layersRoundTripThroughTheFile();
    void layerMapsTakeFileVersionFive();
    void sharesBecomeCoveragesThatDrawTheSame();
    void paintCoversOneLayerAlone();
    void brushMaskShapesTheBrushAndItsOutline();
    void topSurfaceIsTheGroundTheSkySees();
    void paintVoxelsPaintsTheStrengthsShareOfTheVoxels();
    void snapshotKeepsReadingTheGridAsItWasWhenTaken();
};

#endif // hifi_TerrainTests_h
