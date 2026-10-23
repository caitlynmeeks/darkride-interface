//
//  BlendshapeConstants.h
//
//
//  Created by Clement on 1/23/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_BlendshapeConstants_h
#define hifi_BlendshapeConstants_h

#include <QMap>
#include <QString>
#include <QStringList>

#include <glm/glm.hpp>

// The names of the supported blendshapes, terminated with an empty string.
extern const char* BLENDSHAPE_NAMES[];
extern const QMap<QString, int> BLENDSHAPE_LOOKUP_MAP;

// A model's own morph target, and the weight it's applied at, for one of the blendshapes above
struct BlendshapeSource {
    QString name;  // as the model names it; empty if the model has none for the blendshape
    float weight { 1.0f };
    // Or, as a VRM's expressions give it, the target's number among those of the model's mesh of this name, for when
    // the loader gets the targets without their names (three-vrm-girl keeps them only in its primitives' extras)
    QString meshName;
    int index { -1 };
};

// Which of a model's morph targets (modelNames) to use for blendshape `index` when the FST maps nothing for it:
// one named exactly like the blendshape; else, for the four that voice drives (JawOpen, MouthSmile_L/_R,
// LipsUpperClose, LipsFunnel), Ready Player Me's targets; else Apple's ARKit names, which Ready Player Me, VRoid,
// Avaturn, MetaPerson, MakeHuman and iPhone face capture use. Names are compared regardless of case, and also by
// what follows the last '.' (blendShape1.jawOpen) or Rocketbox's AK_nn_ (AK_25_JawOpen).
BlendshapeSource findBlendshapeSource(int index, const QStringList& modelNames);

enum class Blendshapes : int {
    EyeBlink_L = 0,
    EyeBlink_R,
    EyeSquint_L,
    EyeSquint_R,
    EyeDown_L,
    EyeDown_R,
    EyeIn_L,
    EyeIn_R,
    EyeOpen_L,
    EyeOpen_R,
    EyeOut_L,
    EyeOut_R,
    EyeUp_L,
    EyeUp_R,
    BrowsD_L,
    BrowsD_R,
    BrowsU_C,
    BrowsU_L,
    BrowsU_R,
    JawFwd,
    JawLeft,
    JawOpen,
    JawRight,
    MouthLeft,
    MouthRight,
    MouthFrown_L,
    MouthFrown_R,
    MouthSmile_L,
    MouthSmile_R,
    MouthDimple_L,
    MouthDimple_R,
    LipsStretch_L,
    LipsStretch_R,
    LipsUpperClose,
    LipsLowerClose,
    LipsFunnel,
    LipsPucker,
    Puff,
    CheekSquint_L,
    CheekSquint_R,
    MouthClose,
    MouthUpperUp_L,
    MouthUpperUp_R,
    MouthLowerDown_L,
    MouthLowerDown_R,
    MouthPress_L,
    MouthPress_R,
    MouthShrugLower,
    MouthShrugUpper,
    NoseSneer_L,
    NoseSneer_R,
    TongueOut,
    UserBlendshape0,
    UserBlendshape1,
    UserBlendshape2,
    UserBlendshape3,
    UserBlendshape4,
    UserBlendshape5,
    UserBlendshape6,
    UserBlendshape7,
    UserBlendshape8,
    UserBlendshape9,
    BlendshapeCount
};

enum class LegacyBlendshpaes : int {
    JawChew,  // not in ARKit
    LipsUpperUp, // split in ARKit
    LipsLowerDown, // split in ARKit
    ChinLowerRaise, // not in ARKit
    ChinUpperRaise, // not in ARKit
    Sneer, // split in ARKit
    LegacyBlendshapeCount
};

// Original blendshapes were per Faceshift.

// NEW in ARKit
// * MouthClose
// * MouthUpperUp_L
// * MouthUpperUp_R
// * MouthLowerDown_L
// * MouthLowerDown_R
// * MouthPress_L
// * MouthPress_R
// * MouthShrugLower
// * MouthShrugUpper
// * NoseSneer_L
// * NoseSneer_R
// * TongueOut

// Legacy shapes
// * JawChew (not in ARKit)
// * LipsUpperUp (split in ARKit)
// * LipsLowerDown (split in ARKit)
// * Sneer (split in ARKit)
// * ChinLowerRaise (not in ARKit)
// * ChinUpperRaise (not in ARKit)
// * LipsUpperOpen (not in ARKit)
// * LipsLowerOpen (not in ARKit)

struct BlendshapeOffsetPacked {
    glm::uvec4 packedPosNorTan;
};

struct BlendshapeOffsetUnpacked {
    glm::vec3 positionOffset;
    glm::vec3 normalOffset;
    glm::vec3 tangentOffset;
};

using BlendshapeOffset = BlendshapeOffsetPacked;

#endif // hifi_BlendshapeConstants_h
