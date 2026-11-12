//
//  AutoRig.cpp
//  libraries/auto-rigging/src
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AutoRig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_map>

#include <GLMHelpers.h>
#include <hfm/HFM.h>

#include "SkinWeights.h"
#include <StandardSkeleton.h>
#include "VoxelBody.h"

using namespace autorig;

void Body::scale(float factor) {
    for (glm::vec3& v : vertices) {
        v *= factor;
    }
    adjustment = glm::scale(glm::mat4(), glm::vec3(factor)) * adjustment;
    measure();
}

void Body::turnAround() {
    for (glm::vec3& v : vertices) {
        v = glm::vec3(-v.x, v.y, -v.z);
    }
    adjustment = glm::mat4_cast(glm::angleAxis(PI, glm::vec3(0.0f, 1.0f, 0.0f))) * adjustment;
    measure();
}

void Body::measure() {
    minimum = glm::vec3(std::numeric_limits<float>::max());
    maximum = glm::vec3(-std::numeric_limits<float>::max());
    for (const glm::uvec3& t : triangles) {
        for (int k = 0; k < 3; k++) {
            minimum = glm::min(minimum, vertices[t[k]]);
            maximum = glm::max(maximum, vertices[t[k]]);
        }
    }
}

Body autorig::gatherBody(const hfm::Model& model) {
    Body body;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> pieceOf;  // (mesh, placing node) -> its piece
    for (uint32_t shapeIndex = 0; shapeIndex < (uint32_t)model.shapes.size(); shapeIndex++) {
        const hfm::Shape& shape = model.shapes[shapeIndex];
        if (shape.mesh >= model.meshes.size()) {
            continue;
        }
        const hfm::Mesh& mesh = model.meshes[shape.mesh];
        if (shape.meshPart >= mesh.parts.size()) {
            continue;
        }
        const glm::mat4 transform = shape.joint < model.joints.size() ? model.joints[shape.joint].globalTransform
                                                                     : model.offset;
        auto key = std::make_pair(shape.mesh, shape.joint);
        auto found = pieceOf.find(key);
        uint32_t piece;
        if (found == pieceOf.end()) {
            piece = (uint32_t)body.pieces.size();
            pieceOf[key] = piece;
            Body::Piece added;
            added.mesh = shape.mesh;
            added.joint = shape.joint;
            added.firstVertex = (uint32_t)body.vertices.size();
            added.vertexCount = (uint32_t)mesh.vertices.size();
            added.transform = transform;
            body.pieces.push_back(added);
            for (const glm::vec3& vertex : mesh.vertices) {
                body.vertices.push_back(glm::vec3(transform * glm::vec4(vertex, 1.0f)));
            }
        } else {
            piece = found->second;
        }
        body.pieces[piece].shapes.push_back(shapeIndex);
        const uint32_t base = body.pieces[piece].firstVertex;
        const hfm::MeshPart& part = mesh.parts[shape.meshPart];
        const int vertexCount = mesh.vertices.size();
        for (const QVector<int>* indices : { &part.triangleIndices, &part.quadTrianglesIndices }) {
            for (int i = 0; i + 2 < indices->size(); i += 3) {
                int a = indices->at(i), b = indices->at(i + 1), c = indices->at(i + 2);
                if (a >= 0 && b >= 0 && c >= 0 && a < vertexCount && b < vertexCount && c < vertexCount) {
                    body.triangles.push_back(glm::uvec3(base + a, base + b, base + c));
                }
            }
        }
    }
    body.measure();
    return body;
}

namespace {

// A centerline from a far end of the body (a fingertip, a toe, the top of the head) back to the core of the torso.
struct Centerline {
    std::vector<glm::vec3> points;  // the far end first
    std::vector<float> along;       // distance from the far end, metres
};

Centerline makeCenterline(const VoxelBody& voxels, const std::vector<size_t>& cells) {
    Centerline line;
    std::vector<glm::vec3> raw;
    for (size_t cell : cells) {
        raw.push_back(voxels.center(cell));
    }
    // smooth the voxel staircase
    const int reach = 2;
    const int count = (int)raw.size();
    for (int k = 0; k < count; k++) {
        glm::vec3 sum(0.0f);
        int n = 0;
        for (int j = std::max(0, k - reach); j <= std::min(count - 1, k + reach); j++) {
            sum += raw[j];
            n++;
        }
        line.points.push_back(k == 0 ? raw[0] : sum / (float)n);
    }
    float run = 0.0f;
    for (int k = 0; k < count; k++) {
        if (k > 0) {
            run += glm::distance(line.points[k], line.points[k - 1]);
        }
        line.along.push_back(run);
    }
    return line;
}

int indexAlong(const Centerline& line, float distance) {
    for (int k = 0; k < (int)line.along.size(); k++) {
        if (line.along[k] >= distance) {
            return k;
        }
    }
    return (int)line.along.size() - 1;
}

glm::vec3 pointAlong(const Centerline& line, float distance) {
    int k = indexAlong(line, distance);
    if (k == 0) {
        return line.points[0];
    }
    float span = line.along[k] - line.along[k - 1];
    float t = span > 0.0f ? glm::clamp((distance - line.along[k - 1]) / span, 0.0f, 1.0f) : 0.0f;
    return glm::mix(line.points[k - 1], line.points[k], t);
}

// Walking from the far end, the first point at or above a height.
int indexAbove(const Centerline& line, float y, int from) {
    for (int k = from; k < (int)line.points.size(); k++) {
        if (line.points[k].y >= y) {
            return k;
        }
    }
    return (int)line.points.size() - 1;
}

// The point whose height is nearest a height, from a starting point on.
int indexNearestHeight(const Centerline& line, float y, int from) {
    int best = from;
    for (int k = from; k < (int)line.points.size(); k++) {
        if (std::fabs(line.points[k].y - y) < std::fabs(line.points[best].y - y)) {
            best = k;
        }
    }
    return best;
}

float median(std::vector<float> values) {
    if (values.empty()) {
        return 0.0f;
    }
    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    return values[values.size() / 2];
}

// How deep below the surface a point lies (metres), 0 outside the body.
float depthAt(const VoxelBody& voxels, const glm::vec3& point) {
    long k = voxels.find(point);
    return k >= 0 && voxels.inside(k) ? voxels.depth(k) : 0.0f;
}

struct Arm {
    glm::vec3 arm, foreArm, hand, tip;
};

struct Leg {
    glm::vec3 upLeg, leg, foot, toeBase, toeEnd;
};

}

// Places the standard skeleton; false (with the reason in result.error) when the body can't be read.
static bool placeJoints(const Body& body, const VoxelBody& voxels, const Options& options, Result& result) {
    const float floorY = body.minimum.y;
    const float H = body.maximum.y - body.minimum.y;

    // the core: the deepest voxel near the middle of the body
    glm::vec3 centroid(0.0f);
    size_t n = 0;
    for (size_t i = 0; i < voxels.count(); i++) {
        if (voxels.inside(i)) {
            centroid += voxels.center(i);
            n++;
        }
    }
    centroid /= (float)n;
    long core = voxels.nearestInside(centroid, (int)(0.15f * H / voxels.voxelSize()));
    if (core < 0) {
        result.error = "no body near the middle of the model";
        return false;
    }
    {
        const glm::vec3 start = voxels.center(core);
        const int radius = (int)(0.08f * H / voxels.voxelSize());
        for (int dz = -radius; dz <= radius; dz++) {
            for (int dy = -radius; dy <= radius; dy++) {
                for (int dx = -radius; dx <= radius; dx++) {
                    long k = voxels.find(start + glm::vec3(dx, dy, dz) * voxels.voxelSize());
                    if (k >= 0 && voxels.inside(k) && voxels.depth(k) > voxels.depth(core)) {
                        core = k;
                    }
                }
            }
        }
    }
    const glm::vec3 corePoint = voxels.center(core);
    const VoxelBody::Paths plain = voxels.shortestPaths({ (size_t)core }, false);

    // the far ends: the top of the head, the fingertips (most to each side), the toes (furthest along the ground)
    long headTop = -1, leftTip = -1, rightTip = -1, leftToe = -1, rightToe = -1;
    for (size_t i = 0; i < voxels.count(); i++) {
        if (!voxels.inside(i) || !std::isfinite(plain.distance[i])) {
            continue;
        }
        const glm::vec3 p = voxels.center(i);
        if (headTop < 0 || p.y > voxels.center(headTop).y) {
            headTop = (long)i;
        }
        if (p.y > floorY + 0.4f * H) {
            if (leftTip < 0 || p.x > voxels.center(leftTip).x) {
                leftTip = (long)i;
            }
            if (rightTip < 0 || p.x < voxels.center(rightTip).x) {
                rightTip = (long)i;
            }
        }
        if (p.y < floorY + 0.06f * H) {
            long& toe = p.x > corePoint.x ? leftToe : rightToe;
            if (toe < 0 || plain.distance[i] > plain.distance[toe]) {
                toe = (long)i;
            }
        }
    }
    if (headTop < 0 || leftTip < 0 || rightTip < 0 || leftToe < 0 || rightToe < 0) {
        result.error = "could not find the head, both hands and both feet";
        return false;
    }

    const VoxelBody::Paths medial = voxels.shortestPaths({ (size_t)core }, true);
    const Centerline headLine = makeCenterline(voxels, voxels.trace(medial, headTop));
    const Centerline leftArmLine = makeCenterline(voxels, voxels.trace(medial, leftTip));
    const Centerline rightArmLine = makeCenterline(voxels, voxels.trace(medial, rightTip));
    const Centerline leftLegLine = makeCenterline(voxels, voxels.trace(medial, leftToe));
    const Centerline rightLegLine = makeCenterline(voxels, voxels.trace(medial, rightToe));

    // the middle of the body, side to side: the middle of the T or the A the arms make
    const float midX = (body.minimum.x + body.maximum.x) * 0.5f;

    auto placeArm = [&](const Centerline& line, const char* side) {
        Arm arm;
        const int count = (int)line.points.size();
        std::vector<float> sagittal(count), below(count), radii;
        for (int k = 0; k < count; k++) {
            sagittal[k] = voxels.thickness(line.points[k], glm::vec3(0.0f, 0.0f, 1.0f));
            below[k] = voxels.reach(line.points[k], glm::vec3(0.0f, -1.0f, 0.0f));
            if (line.along[k] > 0.12f * H && line.along[k] < 0.30f * H) {
                radii.push_back(depthAt(voxels, line.points[k]));
            }
        }
        auto smoothed = [&](const std::vector<float>& values, int k) {
            float sum = 0.0f;
            int n = 0;
            for (int j = std::max(0, k - 2); j <= std::min(count - 1, k + 2); j++) {
                sum += values[j];
                n++;
            }
            return sum / (float)n;
        };
        const float radius = median(radii);
        // the armpit: past the hand and forearm, the first point with the side of the torso below it
        int armpit = -1;
        for (int k = 0; k < count; k++) {
            if (line.along[k] > 0.15f * H && below[k] > std::max(3.0f * radius, 0.08f * H)) {
                armpit = k;
                break;
            }
        }
        if (armpit < 0) {
            armpit = indexAlong(line, 0.40f * H);
            result.notes << QString("%1 arm: no armpit found, placed by proportion").arg(side);
        }
        // the shoulder joint: in the arm a little out from the side of the torso
        arm.arm = pointAlong(line, line.along[armpit] - 0.3f * radius);
        // the wrist: the narrow point, front to back, just past the widest part of the palm
        int palm = -1;
        for (int k = 0; k < count; k++) {
            if (line.along[k] >= 0.03f * H && line.along[k] <= 0.10f * H &&
                (palm < 0 || smoothed(sagittal, k) > smoothed(sagittal, palm))) {
                palm = k;
            }
        }
        // either a real dip (the forearm widens again after it, as on Mixamo's characters) or, where the arm only
        // tapers (Chloe's), the point where the palm's width starts to fall
        int wrist = -1;
        if (palm >= 0) {
            int lowest = palm;
            for (int k = palm; k < count && line.along[k] <= line.along[palm] + 0.06f * H; k++) {
                if (smoothed(sagittal, k) < smoothed(sagittal, lowest)) {
                    lowest = k;
                }
            }
            bool dip = false;
            for (int k = lowest; k < count && line.along[k] <= line.along[lowest] + 0.04f * H; k++) {
                if (smoothed(sagittal, k) >= smoothed(sagittal, lowest) + 0.35f * voxels.voxelSize()) {
                    dip = true;
                    break;
                }
            }
            if (dip) {
                wrist = lowest;
            } else {
                const float palmWidth = smoothed(sagittal, palm);
                const float threshold = palmWidth - 0.25f * (palmWidth - smoothed(sagittal, lowest));
                for (int k = palm; k <= lowest; k++) {
                    if (smoothed(sagittal, k) < threshold) {
                        wrist = k;
                        break;
                    }
                }
            }
        }
        if (wrist < 0) {
            wrist = indexAlong(line, 0.10f * H);
            result.notes << QString("%1 arm: no wrist found, placed by proportion").arg(side);
        }
        arm.hand = line.points[wrist];
        arm.tip = line.points[0];
        // the elbow halfway from the shoulder to the wrist, as in Mixamo's skeleton
        arm.foreArm = glm::mix(arm.arm, arm.hand, 0.5f);
        result.notes << QString("%1 arm: radius %2 cm, torso side %3 cm from the fingertip, palm %4 cm, wrist %5 cm")
                            .arg(side).arg(radius * 100.0f, 0, 'f', 1).arg(line.along[armpit] * 100.0f, 0, 'f', 1)
                            .arg(palm >= 0 ? line.along[palm] * 100.0f : -1.0f, 0, 'f', 1)
                            .arg(line.along[wrist] * 100.0f, 0, 'f', 1);
        if (options.profiles) {
            for (int k = 0; k < count && line.along[k] < 0.5f * H; k++) {
                const glm::vec3& p = line.points[k];
                result.profiles << QString("%1 %2 at (%3 %4 %5) deep %6 front-to-back %7 below %8")
                                       .arg(side).arg(line.along[k] * 100.0f, 6, 'f', 1).arg(p.x * 100.0f, 0, 'f', 1)
                                       .arg(p.y * 100.0f, 0, 'f', 1).arg(p.z * 100.0f, 0, 'f', 1)
                                       .arg(depthAt(voxels, p) * 100.0f, 0, 'f', 1).arg(sagittal[k] * 100.0f, 0, 'f', 1)
                                       .arg(below[k] * 100.0f, 0, 'f', 1);
            }
        }
        return arm;
    };

    // Mixamo's rig and the game rigs measured so far put the hip joints at 53.6% of the height, 4.5% to each side
    const float hipY = floorY + 0.536f * H;
    auto placeLeg = [&](const Centerline& line, float sideSign) {
        Leg leg;
        const glm::vec3 tip = line.points[0];
        const int ankle = indexAbove(line, floorY + 0.040f * H, 0);
        leg.foot = line.points[ankle];
        leg.toeBase = glm::vec3(glm::mix(leg.foot.x, tip.x, 0.55f), floorY + 0.006f * H,
                                glm::mix(leg.foot.z, tip.z, 0.55f));
        leg.toeEnd = glm::vec3(tip.x, floorY + 0.004f * H, tip.z);
        const int thigh = indexNearestHeight(line, hipY - 0.08f * H, ankle);
        leg.upLeg = glm::vec3(midX + sideSign * 0.045f * H, hipY, line.points[thigh].z);
        leg.leg = line.points[indexNearestHeight(line, (hipY + leg.foot.y) * 0.5f, ankle)];
        return leg;
    };

    const Arm left = placeArm(leftArmLine, "left");
    const Arm right = placeArm(rightArmLine, "right");
    const Leg leftLeg = placeLeg(leftLegLine, 1.0f);
    const Leg rightLeg = placeLeg(rightLegLine, -1.0f);

    // the neck: the narrowest side to side on the head's centerline, between the shoulders and the head
    float neckY = floorY + 0.86f * H;
    {
        float narrowest = std::numeric_limits<float>::max();
        for (int k = 0; k < (int)headLine.points.size(); k++) {
            const glm::vec3& p = headLine.points[k];
            if (p.y < floorY + 0.76f * H || p.y > floorY + 0.93f * H) {
                continue;
            }
            float width = voxels.thickness(p, glm::vec3(1.0f, 0.0f, 0.0f));
            if (width < narrowest) {
                narrowest = width;
                neckY = p.y;
            }
        }
        result.notes << QString("neck: narrowest %1 cm across at %2 cm")
                            .arg(narrowest * 100.0f, 0, 'f', 1).arg((neckY - floorY) * 100.0f, 0, 'f', 1);
    }
    auto onHeadLine = [&](float y) {
        glm::vec3 p = headLine.points[indexNearestHeight(headLine, y, 0)];
        return glm::vec3(midX, y, p.z);
    };

    std::map<QString, glm::vec3> placed;
    placed["Hips"] = glm::vec3(midX, hipY + 0.04f * H, (leftLeg.upLeg.z + rightLeg.upLeg.z) * 0.5f + 0.009f * H);
    placed["Neck"] = onHeadLine(neckY - 0.03f * H);
    placed["Head"] = onHeadLine(neckY + 0.02f * H);
    // the top of the head in Mixamo's direction from the head joint, so the head keeps Mixamo's axes
    const glm::vec3 top = voxels.center(headTop);
    {
        const StandardSkeleton& standard = StandardSkeleton::get();
        const glm::vec3 templateHead = standard.joint(standard.indexOf("Head")).position;
        const glm::vec3 templateTop = standard.joint(standard.indexOf("HeadTop_End")).position;
        const float scale = (top.y - placed["Head"].y) / (templateTop.y - templateHead.y);
        placed["HeadTop_End"] = placed["Head"] + (templateTop - templateHead) * scale;
        placed["HeadTop_End"].x = midX;
    }
    // the spine between the hips and the neck, at Mixamo's proportions, following the torso's centerline
    const float spineSpan = placed["Neck"].y - placed["Hips"].y;
    const char* spineNames[] = { "Spine", "Spine1", "Spine2" };
    const float spineFractions[] = { 0.222f, 0.435f, 0.637f };
    for (int i = 0; i < 3; i++) {
        float y = placed["Hips"].y + spineFractions[i] * spineSpan;
        glm::vec3 p = onHeadLine(y);
        if (y < corePoint.y) {
            p.z = glm::mix(placed["Hips"].z, corePoint.z, (y - placed["Hips"].y) / std::max(corePoint.y - placed["Hips"].y, 0.001f));
        }
        placed[spineNames[i]] = p;
    }
    for (int s = 0; s < 2; s++) {
        const Arm& arm = s == 0 ? left : right;
        const Leg& leg = s == 0 ? leftLeg : rightLeg;
        const QString side = s == 0 ? "Left" : "Right";
        placed[side + "UpLeg"] = leg.upLeg;
        placed[side + "Leg"] = leg.leg;
        placed[side + "Foot"] = leg.foot;
        placed[side + "ToeBase"] = leg.toeBase;
        placed[side + "Toe_End"] = leg.toeEnd;
        placed[side + "Arm"] = arm.arm;
        placed[side + "ForeArm"] = arm.foreArm;
        placed[side + "Hand"] = arm.hand;
        const glm::vec3 spine2 = placed["Spine2"];
        placed[side + "Shoulder"] = glm::vec3(midX + 0.30f * (arm.arm.x - midX), arm.arm.y + 0.003f * H,
                                              (arm.arm.z + spine2.z) * 0.5f);
    }

    // the eyes: behind the front of the face, a little over a third of the way from the head joint to the top
    {
        const float eyeY = placed["Head"].y + 0.37f * (placed["HeadTop_End"].y - placed["Head"].y);
        float faceZ = -std::numeric_limits<float>::max();
        for (size_t i = 0; i < voxels.count(); i++) {
            if (!voxels.inside(i)) {
                continue;
            }
            const glm::vec3 p = voxels.center(i);
            if (std::fabs(p.y - eyeY) < voxels.voxelSize() && std::fabs(p.x - midX) < 0.03f * H) {
                faceZ = std::max(faceZ, p.z);
            }
        }
        if (faceZ < -1.0e30f) {
            faceZ = placed["Head"].z + 0.05f * H;
        }
        placed["LeftEye"] = glm::vec3(midX + 0.018f * H, eyeY, faceZ - 0.022f * H);
        placed["RightEye"] = glm::vec3(midX - 0.018f * H, eyeY, faceZ - 0.022f * H);
    }

    // the fingers: Mixamo's hand, scaled to this hand's length and turned along it
    const StandardSkeleton& standard = StandardSkeleton::get();
    auto templatePosition = [&](const QString& name) {
        return standard.joint(standard.indexOf(name)).position * 0.01f;
    };
    for (int s = 0; s < 2; s++) {
        const Arm& arm = s == 0 ? left : right;
        const QString side = s == 0 ? "Left" : "Right";
        const glm::vec3 templateHand = templatePosition(side + "Hand");
        const glm::vec3 templateTip = templatePosition(side + "HandMiddle4");
        const float scale = glm::distance(arm.tip, arm.hand) / glm::distance(templateTip, templateHand);
        const glm::quat turn = rotationBetween(glm::normalize(templateTip - templateHand),
                                               glm::normalize(arm.tip - arm.hand));
        for (const char* finger : { "Thumb", "Index", "Middle", "Ring", "Pinky" }) {
            for (int k = 1; k <= 4; k++) {
                const QString name = side + "Hand" + finger + QString::number(k);
                placed[name] = arm.hand + turn * ((templatePosition(name) - templateHand) * scale);
            }
        }
    }

    // the joints in the standard skeleton's order, each with Mixamo's axes turned to fit its bone
    std::vector<glm::quat> turns(standard.size());
    for (int i = 0; i < standard.size(); i++) {
        const TemplateJoint& joint = standard.joint(i);
        RiggedJoint rigged;
        rigged.name = joint.name;
        rigged.parent = standard.parentIndex(i);
        rigged.position = placed.count(rigged.name) ? placed[rigged.name] : joint.position * 0.01f;
        result.joints.push_back(rigged);
    }
    for (int i = 0; i < standard.size(); i++) {
        const int child = standard.boneChild(i);
        const int parent = standard.parentIndex(i);
        glm::quat turn;
        if (child >= 0) {
            const glm::vec3 templateBone = standard.joint(child).position - standard.joint(i).position;
            const glm::vec3 bone = result.joints[child].position - result.joints[i].position;
            if (glm::length(templateBone) > 0.0f && glm::length(bone) > 0.0f) {
                turn = rotationBetween(glm::normalize(templateBone), glm::normalize(bone));
            }
        } else if (parent >= 0) {
            turn = turns[parent];
        }
        turns[i] = turn;
        result.joints[i].rotation = glm::normalize(turn * standard.joint(i).rotation);
    }
    return true;
}

// --- the eyelids -------------------------------------------------------------------------------------------------

// How much the file's own rig ties each vertex of the body to one joint and what hangs below it.
static std::vector<float> fileWeights(const Body& body, const hfm::Model& model, int joint) {
    std::vector<bool> under(model.joints.size(), false);
    for (int j = 0; j < (int)model.joints.size(); j++) {
        for (int k = j, steps = 0; k >= 0 && k < (int)model.joints.size() && steps < 256; k = model.joints[k].parentIndex, steps++) {
            if (k == joint) {
                under[j] = true;
                break;
            }
        }
    }
    std::vector<float> weights(body.vertices.size(), 0.0f);
    for (const Body::Piece& piece : body.pieces) {
        if (piece.shapes.empty() || piece.mesh >= model.meshes.size()) {
            continue;
        }
        const hfm::Shape& shape = model.shapes[piece.shapes[0]];
        if (shape.skinDeformer >= model.skinDeformers.size()) {
            continue;
        }
        const hfm::SkinDeformer& deformer = model.skinDeformers[shape.skinDeformer];
        const hfm::Mesh& mesh = model.meshes[piece.mesh];
        const size_t per = mesh.clusterWeightsPerVertex;
        if (per == 0 || mesh.clusterIndices.size() < piece.vertexCount * per ||
            mesh.clusterWeights.size() < piece.vertexCount * per) {
            continue;
        }
        for (uint32_t v = 0; v < piece.vertexCount; v++) {
            float weight = 0.0f;
            for (size_t k = 0; k < per; k++) {
                const uint16_t cluster = mesh.clusterIndices[v * per + k];
                if (cluster < deformer.clusters.size()) {
                    const uint32_t bound = deformer.clusters[cluster].jointIndex;
                    if (bound < under.size() && under[bound]) {
                        weight += (float)mesh.clusterWeights[v * per + k] / (float)UINT16_MAX;
                    }
                }
            }
            weights[piece.firstVertex + v] = std::min(weight, 1.0f);
        }
    }
    return weights;
}

QString autorig::openEyelids(Body& body, const hfm::Model& model) {
    auto find = [&](const QString& name) {
        for (int j = 0; j < (int)model.joints.size(); j++) {
            if (model.joints[j].name.section(':', -1).compare(name, Qt::CaseInsensitive) == 0) {
                return j;
            }
        }
        return -1;
    };
    // a turn about the left-right axis through the eye's centre; a positive angle lifts what is in front of it
    auto turned = [](const glm::vec3& p, const glm::vec3& centre, float weight, float angle) {
        const glm::vec3 d = p - centre;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        return p + weight * (centre + glm::vec3(d.x, c * d.y + s * d.z, -s * d.y + c * d.z) - p);
    };
    QStringList notes;
    for (const QString& side : { QString("Left"), QString("Right") }) {
        const int eye = find(side + "Eye");
        if (eye < 0) {
            continue;
        }
        const glm::vec3 centre = extractTranslation(model.joints[eye].globalTransform);
        // the eyeball's radius: its vertices follow the eye bone alone
        const std::vector<float> eyeball = fileWeights(body, model, eye);
        std::vector<float> distances;
        for (uint32_t v = 0; v < body.vertices.size(); v++) {
            if (eyeball[v] > 0.9f) {
                distances.push_back(glm::distance(body.vertices[v], centre));
            }
        }
        if (distances.size() < 12) {
            continue;
        }
        std::nth_element(distances.begin(), distances.begin() + distances.size() / 2, distances.end());
        const float radius = distances[distances.size() / 2];
        // each lid's edge in front of the eye's middle, in eyeball radii above the centre, and where it should be
        const struct { const char* bone; float target; bool upper; } lids[] = { { "UpEye", 0.38f, true },
                                                                                  { "DownEye", -0.42f, false } };
        for (const auto& lid : lids) {
            const int bone = find(side + lid.bone);
            if (bone < 0) {
                continue;
            }
            const std::vector<float> moved = fileWeights(body, model, bone);
            float edge = lid.upper ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max();
            for (uint32_t v = 0; v < body.vertices.size(); v++) {
                const glm::vec3& p = body.vertices[v];
                if (moved[v] >= 0.5f && std::fabs(p.x - centre.x) < 0.3f * radius && p.z > centre.z + 0.3f * radius) {
                    const float height = (p.y - centre.y) / radius;
                    edge = lid.upper ? std::min(edge, height) : std::max(edge, height);
                }
            }
            if (std::fabs(edge) > 0.95f || (lid.upper ? edge >= lid.target : edge <= lid.target)) {
                continue;
            }
            const float angle = std::asin(lid.target) - std::asin(edge);
            int count = 0;
            for (uint32_t v = 0; v < body.vertices.size(); v++) {
                if (moved[v] > 0.0f) {
                    body.vertices[v] = turned(body.vertices[v], centre, moved[v], angle);
                    count++;
                }
            }
            notes << QString("%1 %2 lid opened %3 degrees (%4 vertices; its edge %5 eyeball radii from the centre, "
                             "now %6)")
                         .arg(side.toLower(), lid.upper ? "upper" : "lower").arg(std::fabs(glm::degrees(angle)), 0, 'f', 1)
                         .arg(count).arg(edge, 0, 'f', 2).arg(lid.target, 0, 'f', 2);
        }
    }
    if (notes.isEmpty()) {
        return QString();
    }
    body.measure();
    return "eyelids: " + notes.join("; ");
}

// --- the eyeballs ------------------------------------------------------------------------------------------------

// An eyeball is a small closed piece of the head that a sphere fits, one each side of the face. Its centre is where
// the eye turns: the eye joint goes there, and its vertices follow that joint alone, so the eyes turn with gaze
// instead of riding the head. A layer of the same eye (an iris or a cornea shell) goes with it.
struct Eyeball {
    glm::vec3 center { 0.0f };
    float radius { 0.0f };
    std::vector<uint32_t> vertices;  // every Body vertex of the eye, seam duplicates included
};

// Least squares sphere: |p|^2 = 2 p.c + k, linear in the centre c and k = r^2 - |c|^2, solved about the points'
// mean for precision. spread: the root mean square of each point's distance from the surface, over the radius.
static bool fitSphere(const std::vector<glm::vec3>& points, glm::vec3& center, float& radius, float& spread) {
    if (points.size() < 8) {
        return false;
    }
    glm::dvec3 mean(0.0);
    for (const glm::vec3& p : points) {
        mean += glm::dvec3(p);
    }
    mean /= (double)points.size();
    glm::dmat4 normal(0.0);
    glm::dvec4 right(0.0);
    for (const glm::vec3& point : points) {
        const glm::dvec3 p = glm::dvec3(point) - mean;
        const glm::dvec4 row(2.0 * p.x, 2.0 * p.y, 2.0 * p.z, 1.0);
        normal += glm::outerProduct(row, row);
        right += row * glm::dot(p, p);
    }
    if (std::fabs(glm::determinant(normal)) < 1.0e-30) {
        return false;
    }
    const glm::dvec4 x = glm::inverse(normal) * right;
    const glm::dvec3 c(x.x, x.y, x.z);
    const double r2 = x.w + glm::dot(c, c);
    if (r2 <= 0.0) {
        return false;
    }
    const double r = std::sqrt(r2);
    double squares = 0.0;
    for (const glm::vec3& point : points) {
        const double d = glm::length(glm::dvec3(point) - mean - c) - r;
        squares += d * d;
    }
    center = glm::vec3(c + mean);
    radius = (float)r;
    spread = (float)(std::sqrt(squares / (double)points.size()) / r);
    return true;
}

static int jointIndex(const Result& result, const QString& name) {
    for (int i = 0; i < (int)result.joints.size(); i++) {
        if (result.joints[i].name == name) {
            return i;
        }
    }
    return -1;
}

// Finds the eyeballs, moves the eye joints onto their centres, and returns them (left, right), or none.
static std::vector<Eyeball> findEyeballs(const Body& body, Result& result) {
    const int neck = jointIndex(result, "Neck");
    const int head = jointIndex(result, "Head");
    const int top = jointIndex(result, "HeadTop_End");
    const int leftEye = jointIndex(result, "LeftEye");
    const int rightEye = jointIndex(result, "RightEye");
    if (neck < 0 || head < 0 || top < 0 || leftEye < 0 || rightEye < 0) {
        return {};
    }
    const float H = body.maximum.y - body.minimum.y;
    // searched from the neck up, in front of it: a head joint placed too high (Joyce's, 10-13) still finds the eyes
    const glm::vec3 neckJoint = result.joints[neck].position;
    const float midX = result.joints[head].position.x;

    // the pieces of the mesh: vertices joined by triangles, and by sharing a place (a seam splits them)
    const size_t count = body.vertices.size();
    std::vector<uint32_t> parent(count);
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](uint32_t v) {
        while (parent[v] != v) {
            parent[v] = parent[parent[v]];
            v = parent[v];
        }
        return v;
    };
    auto join = [&](uint32_t a, uint32_t b) {
        a = root(a);
        b = root(b);
        if (a != b) {
            parent[b] = a;
        }
    };
    for (const glm::uvec3& t : body.triangles) {
        join(t.x, t.y);
        join(t.x, t.z);
    }
    const float weld = 0.0001f;
    std::unordered_map<uint64_t, uint32_t> places;
    for (uint32_t v = 0; v < count; v++) {
        const glm::ivec3 q = glm::ivec3(glm::round(body.vertices[v] / weld));
        const uint64_t key = ((uint64_t)(uint32_t)(q.x & 0x1FFFFF) << 42) | ((uint64_t)(uint32_t)(q.y & 0x1FFFFF) << 21) |
                             (uint64_t)(uint32_t)(q.z & 0x1FFFFF);
        auto found = places.emplace(key, v);
        if (!found.second) {
            join(found.first->second, v);
        }
    }
    std::map<uint32_t, std::vector<uint32_t>> pieces;
    for (uint32_t v = 0; v < count; v++) {
        pieces[root(v)].push_back(v);
    }

    // the small round pieces inside the head
    const float headTop = result.joints[top].position.y;
    std::vector<Eyeball> spheres;
    for (const auto& piece : pieces) {
        const std::vector<uint32_t>& vertices = piece.second;
        if (vertices.size() < 12) {
            continue;
        }
        glm::vec3 low(std::numeric_limits<float>::max());
        glm::vec3 high(-std::numeric_limits<float>::max());
        std::vector<glm::vec3> points;
        points.reserve(vertices.size());
        for (uint32_t v : vertices) {
            low = glm::min(low, body.vertices[v]);
            high = glm::max(high, body.vertices[v]);
            points.push_back(body.vertices[v]);
        }
        const glm::vec3 size = high - low;
        if (low.y < neckJoint.y || high.y > headTop || low.z < neckJoint.z || std::fabs(low.x - midX) > 0.07f * H ||
            std::fabs(high.x - midX) > 0.07f * H || std::max({ size.x, size.y, size.z }) > 0.03f * H) {
            continue;
        }
        Eyeball eye;
        float spread = 1.0f;
        if (fitSphere(points, eye.center, eye.radius, spread) && spread < 0.12f && eye.radius > 0.004f * H &&
            eye.radius < 0.012f * H) {
            eye.vertices = vertices;
            spheres.push_back(eye);
        }
    }

    // the pair: one each side, mirror images of each other, an eye's width or so apart
    int bestLeft = -1;
    int bestRight = -1;
    float best = 0.012f * H;
    for (int l = 0; l < (int)spheres.size(); l++) {
        for (int r = 0; r < (int)spheres.size(); r++) {
            const Eyeball& a = spheres[l];
            const Eyeball& b = spheres[r];
            const float apart = a.center.x - b.center.x;
            if (a.center.x <= midX || b.center.x >= midX || apart < 0.02f * H || apart > 0.05f * H) {
                continue;
            }
            const float mismatch = std::fabs((a.center.x - midX) - (midX - b.center.x)) +
                                   std::fabs(a.center.y - b.center.y) + std::fabs(a.center.z - b.center.z) +
                                   2.0f * std::fabs(a.radius - b.radius);
            if (mismatch < best) {
                best = mismatch;
                bestLeft = l;
                bestRight = r;
            }
        }
    }
    if (bestLeft < 0) {
        result.notes << QString("eyes: no eyeball pair among %1 small round pieces of the head; placed by proportion, "
                                "following the head").arg(spheres.size());
        return {};
    }

    // each eye takes its layers too: other round pieces about the same centre, wholly inside the eye
    std::vector<Eyeball> eyes;
    for (int chosen : { bestLeft, bestRight }) {
        Eyeball eye = spheres[chosen];
        for (int other = 0; other < (int)spheres.size(); other++) {
            if (other == bestLeft || other == bestRight ||
                glm::distance(spheres[other].center, eye.center) > 0.5f * eye.radius) {
                continue;
            }
            bool inside = true;
            for (uint32_t v : spheres[other].vertices) {
                inside = inside && glm::distance(body.vertices[v], eye.center) < 1.15f * eye.radius;
            }
            if (inside) {
                eye.vertices.insert(eye.vertices.end(), spheres[other].vertices.begin(), spheres[other].vertices.end());
            }
        }
        eyes.push_back(eye);
    }
    result.joints[leftEye].position = eyes[0].center;
    result.joints[rightEye].position = eyes[1].center;
    result.notes << QString("eyes: eyeballs of radius %1 and %2 cm, %3 cm apart; %4 and %5 vertices follow the eye "
                            "joints")
                        .arg(eyes[0].radius * 100.0f, 0, 'f', 2).arg(eyes[1].radius * 100.0f, 0, 'f', 2)
                        .arg((eyes[0].center.x - eyes[1].center.x) * 100.0f, 0, 'f', 2)
                        .arg(eyes[0].vertices.size()).arg(eyes[1].vertices.size());
    return eyes;
}

// After the skin weights: an eyeball's vertices follow its eye joint alone.
static void bindEyeballs(const std::vector<Eyeball>& eyes, Result& result) {
    const int joints[2] = { jointIndex(result, "LeftEye"), jointIndex(result, "RightEye") };
    for (size_t e = 0; e < eyes.size() && e < 2; e++) {
        for (uint32_t v : eyes[e].vertices) {
            if (v < result.weights.size()) {
                VertexWeights only;
                only.joints[0] = (uint16_t)joints[e];
                only.weights[0] = 1.0f;
                result.weights[v] = only;
            }
        }
    }
}

Result autorig::rig(const Body& body, const Options& options) {
    Result result;
    const float height = body.maximum.y - body.minimum.y;
    if (body.triangles.empty() || height <= 0.0f) {
        result.error = "the model has no triangles";
        return result;
    }
    VoxelBody voxels(body, height / options.voxelsPerHeight);
    const glm::ivec3 dims = voxels.dimensions();
    result.notes << QString("height %1 m, %2 triangles; voxels %3 cm, grid %4 x %5 x %6, %7 inside")
                        .arg(height, 0, 'f', 3).arg(body.triangles.size()).arg(voxels.voxelSize() * 100.0f, 0, 'f', 2)
                        .arg(dims.x).arg(dims.y).arg(dims.z).arg(voxels.insideCount());
    if (voxels.insideCount() == 0) {
        result.error = "no inside found: the mesh does not enclose a body";
        return result;
    }
    if (!placeJoints(body, voxels, options, result)) {
        return result;
    }
    const std::vector<Eyeball> eyes = findEyeballs(body, result);
    if (options.skin) {
        computeSkinWeights(body, voxels, options, result);
        bindEyeballs(eyes, result);
    }
    result.ok = true;
    return result;
}
