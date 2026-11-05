//
//  BlendshapeSourceTests.cpp
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "BlendshapeSourceTests.h"

#include <BlendshapeConstants.h>

QTEST_MAIN(BlendshapeSourceTests)

namespace {

BlendshapeSource find(Blendshapes blendshape, const QStringList& names) {
    return findBlendshapeSource((int)blendshape, names);
}

}

// The engine's own name, exactly, comes first
void BlendshapeSourceTests::testExactName() {
    const BlendshapeSource source = find(Blendshapes::EyeBlink_L, { "eyeBlinkLeft", "EyeBlink_L" });
    QCOMPARE(source.name, QString("EyeBlink_L"));
    QCOMPARE(source.weight, 1.0f);
    QCOMPARE(source.proceduralGain, 1.0f);
}

// Names are compared regardless of case: ARKit's in capitals, and the engine's own in lower case where it has no other
void BlendshapeSourceTests::testCase() {
    QCOMPARE(find(Blendshapes::EyeBlink_R, { "EYEBLINKRIGHT" }).name, QString("EYEBLINKRIGHT"));
    QCOMPARE(find(Blendshapes::UserBlendshape0, { "userblendshape0" }).name, QString("userblendshape0"));
}

// and by what follows the last dot, as some exporters name their targets after the shape key block
void BlendshapeSourceTests::testAfterDot() {
    QCOMPARE(find(Blendshapes::BrowsU_C, { "blendShape1.browInnerUp", "blendShape1.cheekPuff" }).name,
             QString("blendShape1.browInnerUp"));
    QCOMPARE(find(Blendshapes::Puff, { "blendShape1.browInnerUp", "blendShape1.cheekPuff" }).name,
             QString("blendShape1.cheekPuff"));
}

// and without Rocketbox's numbered AK_ prefix
void BlendshapeSourceTests::testRocketboxPrefix() {
    QCOMPARE(find(Blendshapes::EyeBlink_L, { "AK_09_EyeBlinkLeft", "AK_10_EyeBlinkRight" }).name, QString("AK_09_EyeBlinkLeft"));
    QCOMPARE(find(Blendshapes::NoseSneer_R, { "AK_51_NoseSneerRight" }).name, QString("AK_51_NoseSneerRight"));
}

// For the shapes voice drives, Ready Player Me's own targets before ARKit's, and ARKit's before the engine's name in
// another case (jawOpen is ARKit's whole open jaw, not the engine's JawOpen)
void BlendshapeSourceTests::testPreferenceOrder() {
    QCOMPARE(find(Blendshapes::JawOpen, { "jawOpen", "mouthOpen" }).name, QString("mouthOpen"));
    QCOMPARE(find(Blendshapes::JawOpen, { "jawOpen" }).name, QString("jawOpen"));
    QCOMPARE(find(Blendshapes::JawOpen, { "jawOpen", "JawOpen" }).name, QString("JawOpen"));
    QCOMPARE(find(Blendshapes::LipsFunnel, { "mouthFunnel", "viseme_CH" }).name, QString("viseme_CH"));
    QCOMPARE(find(Blendshapes::LipsUpperClose, { "mouthRollUpper", "viseme_PP" }).name, QString("viseme_PP"));
    QCOMPARE(find(Blendshapes::MouthSmile_L, { "mouthSmileLeft", "mouthSmile" }).name, QString("mouthSmile"));
}

// One to one, at weight 1 (F5): the voice's tuning is a gain for the procedural face, not a weight, so face tracking
// drives the target at its full range. Ready Player Me's mouthSmile, both sides in one, is half on each.
void BlendshapeSourceTests::testOneToOne() {
    BlendshapeSource source = find(Blendshapes::JawOpen, { "jawOpen" });
    QCOMPARE(source.weight, 1.0f);
    QCOMPARE(source.proceduralGain, 0.5f);
    source = find(Blendshapes::MouthSmile_R, { "mouthSmileRight" });
    QCOMPARE(source.weight, 1.0f);
    QCOMPARE(source.proceduralGain, 0.7f);
    source = find(Blendshapes::LipsFunnel, { "mouthFunnel" });
    QCOMPARE(source.weight, 1.0f);
    QCOMPARE(source.proceduralGain, 0.6f);
    source = find(Blendshapes::MouthSmile_L, { "mouthSmile" });
    QCOMPARE(source.weight, 0.5f);
    QCOMPARE(source.proceduralGain, 1.0f);
    source = find(Blendshapes::JawOpen, { "mouthOpen" });
    QCOMPARE(source.weight, 1.0f);
    QCOMPARE(source.proceduralGain, 1.0f);
}

// No target for it, or nothing to choose from
void BlendshapeSourceTests::testNone() {
    QVERIFY(find(Blendshapes::TongueOut, { "jawOpen", "Body" }).name.isEmpty());
    QVERIFY(find(Blendshapes::JawOpen, {}).name.isEmpty());
    QVERIFY(findBlendshapeSource(-1, { "jawOpen" }).name.isEmpty());
    QVERIFY(findBlendshapeSource((int)Blendshapes::BlendshapeCount, { "jawOpen" }).name.isEmpty());
}
