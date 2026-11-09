//
//  RigPreview.cpp
//  tools/auto-rig/src
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RigPreview.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

#include <QImage>
#include <QPainter>

#include <GLMHelpers.h>

namespace {

const int PANEL = 512;

// The animation's absolute rotation for each of its joints at one frame, by name, built as AnimClip.cpp builds them.
std::map<QString, glm::quat> animationRotations(const HFMModel& animation, int frame) {
    std::map<QString, glm::quat> byName;
    if (animation.animationFrames.isEmpty()) {
        return byName;
    }
    frame = std::max(0, std::min(frame, (int)animation.animationFrames.size() - 1));
    const HFMAnimationFrame& pose = animation.animationFrames[frame];
    std::vector<glm::quat> absolute(animation.joints.size());
    for (size_t i = 0; i < animation.joints.size(); i++) {
        const HFMJoint& joint = animation.joints[i];
        const glm::quat frameRotation = (int)i < pose.rotations.size() ? pose.rotations[(int)i] : glm::quat();
        const glm::quat relative = joint.preRotation * frameRotation * joint.postRotation;
        absolute[i] = joint.parentIndex >= 0 ? absolute[joint.parentIndex] * relative : relative;
        byName[joint.name.section(':', -1)] = absolute[i];
    }
    return byName;
}

// The body posed: each joint takes the animation's absolute rotation where the animation has the joint, keeps its
// rest rotation relative to its parent where it does not; the hips stay where they are. Dual quaternion skinning, as
// the client uses for avatars.
std::vector<glm::vec3> posedBody(const autorig::Body& body, const autorig::Result& rig,
                                 const std::map<QString, glm::quat>& rotations, std::vector<glm::vec3>& jointPositions) {
    const int count = (int)rig.joints.size();
    std::vector<glm::quat> turned(count);
    jointPositions.assign(count, glm::vec3(0.0f));
    std::vector<glm::quat> real(count), dual(count);
    for (int j = 0; j < count; j++) {
        const autorig::RiggedJoint& joint = rig.joints[j];
        const int parent = joint.parent;
        auto found = rotations.find(joint.name);
        if (found != rotations.end()) {
            turned[j] = found->second;
        } else {
            turned[j] = parent >= 0 ? turned[parent] * (glm::inverse(rig.joints[parent].rotation) * joint.rotation)
                                    : joint.rotation;
        }
        if (parent < 0) {
            jointPositions[j] = joint.position;
        } else {
            const glm::vec3 offset = glm::inverse(rig.joints[parent].rotation) * (joint.position - rig.joints[parent].position);
            jointPositions[j] = jointPositions[parent] + turned[parent] * offset;
        }
        // the joint's move from rest to pose: turn r about the origin, then shift t
        const glm::quat r = glm::normalize(turned[j] * glm::inverse(joint.rotation));
        const glm::vec3 t = jointPositions[j] - r * joint.position;
        real[j] = r;
        dual[j] = 0.5f * (glm::quat(0.0f, t.x, t.y, t.z) * r);
    }
    std::vector<glm::vec3> vertices(body.vertices.size());
    for (size_t v = 0; v < body.vertices.size(); v++) {
        const autorig::VertexWeights& weights = rig.weights[v];
        glm::quat blendReal(0.0f, 0.0f, 0.0f, 0.0f), blendDual(0.0f, 0.0f, 0.0f, 0.0f);
        const glm::quat& first = real[weights.joints[0]];
        for (int k = 0; k < 4; k++) {
            if (weights.weights[k] <= 0.0f) {
                continue;
            }
            const float sign = glm::dot(real[weights.joints[k]], first) < 0.0f ? -1.0f : 1.0f;
            blendReal = blendReal + (sign * weights.weights[k]) * real[weights.joints[k]];
            blendDual = blendDual + (sign * weights.weights[k]) * dual[weights.joints[k]];
        }
        const float length = glm::length(blendReal);
        if (length <= 0.0f) {
            vertices[v] = body.vertices[v];
            continue;
        }
        blendReal = blendReal * (1.0f / length);
        blendDual = blendDual * (1.0f / length);
        const glm::quat shift = blendDual * glm::conjugate(blendReal);
        vertices[v] = blendReal * body.vertices[v] + 2.0f * glm::vec3(shift.x, shift.y, shift.z);
    }
    return vertices;
}

// Draws the mesh shaded, and the skeleton over it, into one panel. side: seen from the character's left.
void drawPanel(QImage& image, int left, const std::vector<glm::vec3>& vertices, const std::vector<glm::uvec3>& triangles,
               const autorig::Result& rig, const std::vector<glm::vec3>& joints, bool side, const glm::vec3& center,
               float scale, const std::vector<float>* heat = nullptr) {
    auto project = [&](const glm::vec3& p, float& depth) {
        const float across = side ? -(p.z - center.z) : (p.x - center.x);
        depth = side ? p.x : p.z;
        return glm::vec2(left + PANEL * 0.5f + across * scale, PANEL * 0.5f - (p.y - center.y) * scale);
    };
    std::vector<float> zbuffer((size_t)PANEL * PANEL, -std::numeric_limits<float>::max());
    const glm::vec3 toViewer = side ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 light = glm::normalize(toViewer + glm::vec3(0.0f, 0.6f, 0.0f) + (side ? glm::vec3(0.0f, 0.0f, 0.4f)
                                                                                          : glm::vec3(0.4f, 0.0f, 0.0f)));
    for (const glm::uvec3& t : triangles) {
        float da, db, dc;
        const glm::vec2 a = project(vertices[t.x], da), b = project(vertices[t.y], db), c = project(vertices[t.z], dc);
        const glm::vec3 normal = glm::cross(vertices[t.y] - vertices[t.x], vertices[t.z] - vertices[t.x]);
        if (glm::length(normal) <= 0.0f) {
            continue;
        }
        const float shade = std::min(1.0f, 0.3f + 0.7f * std::fabs(glm::dot(glm::normalize(normal), light)));
        QRgb color = qRgb((int)(255.0f * shade), (int)(255.0f * shade), (int)(255.0f * shade));
        if (heat) {
            // blue (no weight) through green to red (all of it)
            const float h = glm::clamp(((*heat)[t.x] + (*heat)[t.y] + (*heat)[t.z]) / 3.0f, 0.0f, 1.0f);
            const glm::vec3 tint = h < 0.5f ? glm::mix(glm::vec3(0.15f, 0.25f, 1.0f), glm::vec3(0.1f, 0.9f, 0.2f), h * 2.0f)
                                            : glm::mix(glm::vec3(0.1f, 0.9f, 0.2f), glm::vec3(1.0f, 0.15f, 0.1f), h * 2.0f - 1.0f);
            color = qRgb((int)(255.0f * shade * tint.r), (int)(255.0f * shade * tint.g), (int)(255.0f * shade * tint.b));
        }
        const int x0 = std::max(left, (int)std::floor(std::min(a.x, std::min(b.x, c.x))));
        const int x1 = std::min(left + PANEL - 1, (int)std::ceil(std::max(a.x, std::max(b.x, c.x))));
        const int y0 = std::max(0, (int)std::floor(std::min(a.y, std::min(b.y, c.y))));
        const int y1 = std::min(PANEL - 1, (int)std::ceil(std::max(a.y, std::max(b.y, c.y))));
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1.0e-6f) {
            continue;
        }
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                const glm::vec2 p(x + 0.5f, y + 0.5f);
                const float w0 = ((b.x - p.x) * (c.y - p.y) - (b.y - p.y) * (c.x - p.x)) / area;
                const float w1 = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                    continue;
                }
                const float depth = w0 * da + w1 * db + w2 * dc;
                float& stored = zbuffer[(size_t)y * PANEL + (x - left)];
                if (depth > stored) {
                    stored = depth;
                    image.setPixel(x, y, color);
                }
            }
        }
    }
    QPainter painter(&image);
    painter.setClipRect(left, 0, PANEL, PANEL);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(220, 40, 40), 1.5));
    for (size_t j = 0; j < rig.joints.size(); j++) {
        const int parent = rig.joints[j].parent;
        if (parent >= 0) {
            float unused;
            const glm::vec2 a = project(joints[parent], unused), b = project(joints[j], unused);
            painter.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
        }
    }
    painter.setBrush(QColor(255, 200, 0));
    painter.setPen(Qt::NoPen);
    for (const glm::vec3& joint : joints) {
        float unused;
        const glm::vec2 p = project(joint, unused);
        painter.drawEllipse(QPointF(p.x, p.y), 2.0, 2.0);
    }
}

}

bool renderPreview(const autorig::Body& body, const autorig::Result& rig, const HFMModel* animation, int frame,
                   const QString& pngPath, const QString& focusJoint, float zoom, const std::vector<float>* heat,
                   const std::vector<float>* keyHeat, bool sideHeat) {
    if (!rig.ok || rig.weights.size() != body.vertices.size()) {
        return false;
    }
    std::vector<glm::vec3> restJoints;
    for (const autorig::RiggedJoint& joint : rig.joints) {
        restJoints.push_back(joint.position);
    }
    std::vector<glm::vec3> posedJoints;
    std::vector<glm::vec3> posed;
    if (animation) {
        posed = posedBody(body, rig, animationRotations(*animation, frame), posedJoints);
    }
    const int panels = animation ? 4 : 2;
    QImage image(PANEL * panels, PANEL, QImage::Format_RGB32);
    image.fill(qRgb(40, 44, 52));
    glm::vec3 center = (body.minimum + body.maximum) * 0.5f;
    glm::vec3 posedCenter = center;
    for (size_t j = 0; j < rig.joints.size(); j++) {
        if (rig.joints[j].name == focusJoint) {
            center = restJoints[j];
            posedCenter = animation ? posedJoints[j] : center;
        }
    }
    const glm::vec3 span = body.maximum - body.minimum;
    const float scale = std::max(zoom, 0.1f) * 0.9f * PANEL / std::max(span.x, std::max(span.y, span.z));
    drawPanel(image, 0, body.vertices, body.triangles, rig, restJoints, sideHeat, center, scale, heat);
    if (keyHeat) {
        drawPanel(image, PANEL, body.vertices, body.triangles, rig, restJoints, sideHeat, center, scale, keyHeat);
    } else {
        drawPanel(image, PANEL, body.vertices, body.triangles, rig, restJoints, true, center, scale, heat);
    }
    if (animation) {
        drawPanel(image, 2 * PANEL, posed, body.triangles, rig, posedJoints, false, posedCenter, scale);
        drawPanel(image, 3 * PANEL, posed, body.triangles, rig, posedJoints, true, posedCenter, scale);
    }
    return image.save(pngPath);
}
