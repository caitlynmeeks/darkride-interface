//
//  BlendshapeConstants.cpp
//
//
//  Created by Clement on 1/23/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "BlendshapeConstants.h"

#include <QHash>
#include <QRegularExpression>
#include <QVector>

const char* BLENDSHAPE_NAMES[] = {
    "EyeBlink_L",
    "EyeBlink_R",
    "EyeSquint_L",
    "EyeSquint_R",
    "EyeDown_L",
    "EyeDown_R",
    "EyeIn_L",
    "EyeIn_R",
    "EyeOpen_L",
    "EyeOpen_R",
    "EyeOut_L",
    "EyeOut_R",
    "EyeUp_L",
    "EyeUp_R",
    "BrowsD_L",
    "BrowsD_R",
    "BrowsU_C",
    "BrowsU_L",
    "BrowsU_R",
    "JawFwd",
    "JawLeft",
    "JawOpen",
    "JawRight",
    "MouthLeft",
    "MouthRight",
    "MouthFrown_L",
    "MouthFrown_R",
    "MouthSmile_L",
    "MouthSmile_R",
    "MouthDimple_L",
    "MouthDimple_R",
    "LipsStretch_L",
    "LipsStretch_R",
    "LipsUpperClose",
    "LipsLowerClose",
    "LipsFunnel",
    "LipsPucker",
    "Puff",
    "CheekSquint_L",
    "CheekSquint_R",
    "MouthClose",
    "MouthUpperUp_L",
    "MouthUpperUp_R",
    "MouthLowerDown_L",
    "MouthLowerDown_R",
    "MouthPress_L",
    "MouthPress_R",
    "MouthShrugLower",
    "MouthShrugUpper",
    "NoseSneer_L",
    "NoseSneer_R",
    "TongueOut",
    "UserBlendshape0",
    "UserBlendshape1",
    "UserBlendshape2",
    "UserBlendshape3",
    "UserBlendshape4",
    "UserBlendshape5",
    "UserBlendshape6",
    "UserBlendshape7",
    "UserBlendshape8",
    "UserBlendshape9",
    ""
};

const QMap<QString, int> BLENDSHAPE_LOOKUP_MAP = [] {
    QMap<QString, int> toReturn;
    for (int i = 0; i < (int)Blendshapes::BlendshapeCount; i++) {
        toReturn[BLENDSHAPE_NAMES[i]] = i;
    }
    return toReturn;
}();

namespace {

struct Candidate {
    const char* name;
    float weight;
};

// The other names a model may give each blendshape, in order of preference.
//
// The four shapes that voice drives come first from Ready Player Me's own targets, as Overte maps them. Head.cpp's
// voice animation was tuned for much smaller shapes than ARKit's: on a Ready Player Me head, jawOpen moves the jaw
// about four times as far as mouthOpen, and plain talking looked like shouting. When only the ARKit shape is
// there, it's scaled to about the same movement (measured on a Ready Player Me avatar: jawOpen 4.65 cm against
// mouthOpen 1.11; mouthSmileLeft 2.2 cm a side against mouthSmile 1.57 for both; mouthRollUpper 1.04 against
// viseme_PP 0.65; mouthFunnel 1.43 against viseme_CH 0.89). jawOpen alone is at twice that, as on Rocketbox's ARKit
// face the mouth barely moved at a quarter. Every other shape is ARKit's, at full weight.
const QHash<int, QVector<Candidate>> ALTERNATIVE_NAMES = [] {
    QHash<int, QVector<Candidate>> names;
    auto add = [&names](Blendshapes blendshape, QVector<Candidate> candidates) {
        names[(int)blendshape] = candidates;
    };
    add(Blendshapes::EyeBlink_L, { { "eyeBlinkLeft", 1.0f } });
    add(Blendshapes::EyeBlink_R, { { "eyeBlinkRight", 1.0f } });
    add(Blendshapes::EyeSquint_L, { { "eyeSquintLeft", 1.0f } });
    add(Blendshapes::EyeSquint_R, { { "eyeSquintRight", 1.0f } });
    add(Blendshapes::EyeDown_L, { { "eyeLookDownLeft", 1.0f } });
    add(Blendshapes::EyeDown_R, { { "eyeLookDownRight", 1.0f } });
    add(Blendshapes::EyeIn_L, { { "eyeLookInLeft", 1.0f } });
    add(Blendshapes::EyeIn_R, { { "eyeLookInRight", 1.0f } });
    add(Blendshapes::EyeOpen_L, { { "eyeWideLeft", 1.0f } });
    add(Blendshapes::EyeOpen_R, { { "eyeWideRight", 1.0f } });
    add(Blendshapes::EyeOut_L, { { "eyeLookOutLeft", 1.0f } });
    add(Blendshapes::EyeOut_R, { { "eyeLookOutRight", 1.0f } });
    add(Blendshapes::EyeUp_L, { { "eyeLookUpLeft", 1.0f } });
    add(Blendshapes::EyeUp_R, { { "eyeLookUpRight", 1.0f } });
    add(Blendshapes::BrowsD_L, { { "browDownLeft", 1.0f } });
    add(Blendshapes::BrowsD_R, { { "browDownRight", 1.0f } });
    add(Blendshapes::BrowsU_C, { { "browInnerUp", 1.0f } });
    add(Blendshapes::BrowsU_L, { { "browOuterUpLeft", 1.0f } });
    add(Blendshapes::BrowsU_R, { { "browOuterUpRight", 1.0f } });
    add(Blendshapes::JawFwd, { { "jawForward", 1.0f } });
    add(Blendshapes::JawLeft, { { "jawLeft", 1.0f } });
    add(Blendshapes::JawOpen, { { "mouthOpen", 1.0f }, { "jawOpen", 0.5f } });
    add(Blendshapes::JawRight, { { "jawRight", 1.0f } });
    add(Blendshapes::MouthLeft, { { "mouthLeft", 1.0f } });
    add(Blendshapes::MouthRight, { { "mouthRight", 1.0f } });
    add(Blendshapes::MouthFrown_L, { { "mouthFrownLeft", 1.0f } });
    add(Blendshapes::MouthFrown_R, { { "mouthFrownRight", 1.0f } });
    add(Blendshapes::MouthSmile_L, { { "mouthSmile", 0.5f }, { "mouthSmileLeft", 0.7f } });
    add(Blendshapes::MouthSmile_R, { { "mouthSmile", 0.5f }, { "mouthSmileRight", 0.7f } });
    add(Blendshapes::MouthDimple_L, { { "mouthDimpleLeft", 1.0f } });
    add(Blendshapes::MouthDimple_R, { { "mouthDimpleRight", 1.0f } });
    add(Blendshapes::LipsStretch_L, { { "mouthStretchLeft", 1.0f } });
    add(Blendshapes::LipsStretch_R, { { "mouthStretchRight", 1.0f } });
    add(Blendshapes::LipsUpperClose, { { "viseme_PP", 1.0f }, { "mouthRollUpper", 0.6f } });
    add(Blendshapes::LipsLowerClose, { { "mouthRollLower", 1.0f } });
    add(Blendshapes::LipsFunnel, { { "viseme_CH", 1.0f }, { "mouthFunnel", 0.6f } });
    add(Blendshapes::LipsPucker, { { "mouthPucker", 1.0f } });
    add(Blendshapes::Puff, { { "cheekPuff", 1.0f } });
    add(Blendshapes::CheekSquint_L, { { "cheekSquintLeft", 1.0f } });
    add(Blendshapes::CheekSquint_R, { { "cheekSquintRight", 1.0f } });
    add(Blendshapes::MouthClose, { { "mouthClose", 1.0f } });
    add(Blendshapes::MouthUpperUp_L, { { "mouthUpperUpLeft", 1.0f } });
    add(Blendshapes::MouthUpperUp_R, { { "mouthUpperUpRight", 1.0f } });
    add(Blendshapes::MouthLowerDown_L, { { "mouthLowerDownLeft", 1.0f } });
    add(Blendshapes::MouthLowerDown_R, { { "mouthLowerDownRight", 1.0f } });
    add(Blendshapes::MouthPress_L, { { "mouthPressLeft", 1.0f } });
    add(Blendshapes::MouthPress_R, { { "mouthPressRight", 1.0f } });
    add(Blendshapes::MouthShrugLower, { { "mouthShrugLower", 1.0f } });
    add(Blendshapes::MouthShrugUpper, { { "mouthShrugUpper", 1.0f } });
    add(Blendshapes::NoseSneer_L, { { "noseSneerLeft", 1.0f } });
    add(Blendshapes::NoseSneer_R, { { "noseSneerRight", 1.0f } });
    add(Blendshapes::TongueOut, { { "tongueOut", 1.0f } });
    return names;
}();

} // namespace

BlendshapeSource findBlendshapeSource(int index, const QStringList& modelNames) {
    if (index < 0 || index >= (int)Blendshapes::BlendshapeCount || modelNames.isEmpty()) {
        return {};
    }

    // the model's names by their lower case, by the lower case of what follows their last '.', and without the
    // numbered prefix Microsoft's Rocketbox avatars give their ARKit names (AK_25_JawOpen)
    static const QRegularExpression ROCKETBOX_PREFIX("^ak_[0-9]+_");
    QHash<QString, QString> byLowerCase;
    for (const QString& name : modelNames) {
        QString lower = name.toLower();
        int dot = lower.lastIndexOf('.');
        QString unprefixed = lower;
        unprefixed.remove(ROCKETBOX_PREFIX);
        for (const QString& key : { lower, dot >= 0 ? lower.mid(dot + 1) : QString(), unprefixed }) {
            if (!key.isEmpty() && !byLowerCase.contains(key)) {
                byLowerCase.insert(key, name);
            }
        }
    }

    // its own name, exactly
    if (modelNames.contains(BLENDSHAPE_NAMES[index])) {
        return { BLENDSHAPE_NAMES[index], 1.0f };
    }

    // then the other names; before its own name in another case, which ARKit's can be (jawOpen, scaled, for JawOpen)
    for (const Candidate& candidate : ALTERNATIVE_NAMES.value(index)) {
        auto found = byLowerCase.find(QString(candidate.name).toLower());
        if (found != byLowerCase.end()) {
            return { found.value(), candidate.weight };
        }
    }

    auto own = byLowerCase.find(QString(BLENDSHAPE_NAMES[index]).toLower());
    if (own != byLowerCase.end()) {
        return { own.value(), 1.0f };
    }
    return {};
}
