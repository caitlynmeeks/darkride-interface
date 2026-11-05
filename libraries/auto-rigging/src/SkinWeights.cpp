//
//  SkinWeights.cpp
//  libraries/auto-rigging/src
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "SkinWeights.h"

#include <StandardSkeleton.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <unordered_map>

#include <QElapsedTimer>

using namespace autorig;

static float distanceToSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float length2 = glm::dot(ab, ab);
    const float t = length2 > 0.0f ? glm::clamp(glm::dot(p - a, ab) / length2, 0.0f, 1.0f) : 0.0f;
    return glm::distance(p, a + ab * t);
}

void autorig::computeSkinWeights(const Body& body, const VoxelBody& voxels, const Options& options, Result& result) {
    QElapsedTimer timer;
    timer.start();
    const std::vector<RiggedJoint>& joints = result.joints;
    const int jointCount = (int)joints.size();
    const float size = voxels.voxelSize();
    const float height = body.maximum.y - body.minimum.y;

    // the joints that carry weight: every joint with a bone to a child, except the eyes, which move only eyeballs
    std::vector<std::vector<int>> children(jointCount);
    for (int j = 0; j < jointCount; j++) {
        if (joints[j].parent >= 0) {
            children[joints[j].parent].push_back(j);
        }
    }
    std::vector<int> influences;
    for (int j = 0; j < jointCount; j++) {
        if (!children[j].empty() && joints[j].name != "LeftEye" && joints[j].name != "RightEye") {
            influences.push_back(j);
        }
    }
    const int influenceCount = (int)influences.size();

    // weld: one point per distinct position, so vertices split along UV seams, and pieces that touch, move together
    std::vector<uint32_t> weldOf(body.vertices.size());
    std::vector<glm::vec3> welded;
    {
        std::unordered_map<uint64_t, uint32_t> seen;
        const float grain = 1.0e-4f;  // a tenth of a millimetre
        for (size_t v = 0; v < body.vertices.size(); v++) {
            const glm::vec3& p = body.vertices[v];
            const uint64_t key = ((uint64_t)std::llround(p.x / grain) & 0x1FFFFF) |
                                 (((uint64_t)std::llround(p.y / grain) & 0x1FFFFF) << 21) |
                                 (((uint64_t)std::llround(p.z / grain) & 0x1FFFFF) << 42);
            auto found = seen.find(key);
            if (found == seen.end()) {
                seen[key] = (uint32_t)welded.size();
                weldOf[v] = (uint32_t)welded.size();
                welded.push_back(p);
            } else {
                weldOf[v] = found->second;
            }
        }
    }
    const size_t weldCount = welded.size();
    std::vector<long> cellOf(weldCount, -1);
    for (size_t w = 0; w < weldCount; w++) {
        long k = voxels.find(welded[w]);
        if (k < 0 || !voxels.inside(k)) {
            k = voxels.nearestInside(welded[w], 2);
        }
        cellOf[w] = k;
    }

    // the distance through the body from each influence's bones to every point: one search per influence, in parallel
    std::vector<float> distance((size_t)influenceCount * weldCount, std::numeric_limits<float>::infinity());
    std::vector<float> radius(influenceCount, size);  // how thick the body is around each influence's bones
    std::atomic<int> next(0);
    auto work = [&]() {
        while (true) {
            const int i = next++;
            if (i >= influenceCount) {
                break;
            }
            const int j = influences[i];
            std::vector<size_t> seeds;
            for (int c : children[j]) {
                const glm::vec3 a = joints[j].position;
                const glm::vec3 b = joints[c].position;
                const int steps = std::max(1, (int)std::ceil(glm::distance(a, b) / (0.5f * size)));
                for (int s = 0; s <= steps; s++) {
                    long k = voxels.find(glm::mix(a, b, (float)s / steps));
                    if (k >= 0 && voxels.inside(k)) {
                        seeds.push_back((size_t)k);
                    }
                }
            }
            if (seeds.empty()) {
                long k = voxels.nearestInside(joints[j].position, 4);
                if (k >= 0) {
                    seeds.push_back((size_t)k);
                }
            }
            std::sort(seeds.begin(), seeds.end());
            seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
            std::vector<float> depths;
            for (size_t seed : seeds) {
                depths.push_back(voxels.depth(seed));
            }
            if (!depths.empty()) {
                std::nth_element(depths.begin(), depths.begin() + depths.size() / 2, depths.end());
                radius[i] = std::max(depths[depths.size() / 2], size);
            }
            const VoxelBody::Paths paths = voxels.shortestPaths(seeds, false, 0.5f * height);
            float* row = &distance[(size_t)i * weldCount];
            for (size_t w = 0; w < weldCount; w++) {
                if (cellOf[w] >= 0) {
                    row[w] = paths.distance[(size_t)cellOf[w]];
                }
            }
        }
    };
    const unsigned threadCount = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < threadCount; t++) {
        threads.emplace_back(work);
    }
    for (std::thread& t : threads) {
        t.join();
    }

    std::vector<float> weight(weldCount * (size_t)influenceCount, 0.0f);
    size_t unreached = 0;
    // a piece the body's volume does not reach: all of it to the nearest bone in a straight line
    auto nearestBone = [&](size_t w) {
        int best = 0;
        float bestDistance = std::numeric_limits<float>::max();
        for (int i = 0; i < influenceCount; i++) {
            const int j = influences[i];
            for (int c : children[j]) {
                const float d = distanceToSegment(welded[w], joints[j].position, joints[c].position);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = i;
                }
            }
        }
        return best;
    };
    if (options.weighting == 0) {
        // each influence's weight falls off with its distance in voxels: (1 / ((1 - a) d + a d^2))^2
        const float alpha = glm::clamp(options.falloff, 0.0f, 1.0f);
        for (size_t w = 0; w < weldCount; w++) {
            float* out = &weight[w * influenceCount];
            float sum = 0.0f;
            for (int i = 0; i < influenceCount; i++) {
                float d = distance[(size_t)i * weldCount + w];
                if (!std::isfinite(d)) {
                    continue;
                }
                d = std::max(d / size, 0.5f);
                const float f = 1.0f / ((1.0f - alpha) * d + alpha * d * d);
                out[i] = f * f;
                sum += out[i];
            }
            if (sum <= 0.0f) {
                unreached++;
                out[nearestBone(w)] = 1.0f;
                sum = 1.0f;
            }
            for (int i = 0; i < influenceCount; i++) {
                out[i] /= sum;
            }
        }

        // smoothing over the welded mesh: each pass moves every point halfway to the average of its neighbours
        std::vector<std::vector<uint32_t>> neighbours(weldCount);
        for (const glm::uvec3& t : body.triangles) {
            const uint32_t corner[3] = { weldOf[t.x], weldOf[t.y], weldOf[t.z] };
            for (int k = 0; k < 3; k++) {
                const uint32_t a = corner[k];
                const uint32_t b = corner[(k + 1) % 3];
                if (a != b) {
                    neighbours[a].push_back(b);
                    neighbours[b].push_back(a);
                }
            }
        }
        for (std::vector<uint32_t>& near : neighbours) {
            std::sort(near.begin(), near.end());
            near.erase(std::unique(near.begin(), near.end()), near.end());
        }
        std::vector<float> smoothed(weight.size());
        for (int pass = 0; pass < options.smoothing; pass++) {
            for (size_t w = 0; w < weldCount; w++) {
                const std::vector<uint32_t>& near = neighbours[w];
                const float* own = &weight[w * influenceCount];
                float* out = &smoothed[w * influenceCount];
                if (near.empty()) {
                    std::copy(own, own + influenceCount, out);
                    continue;
                }
                for (int i = 0; i < influenceCount; i++) {
                    float sum = 0.0f;
                    for (uint32_t n : near) {
                        sum += weight[(size_t)n * influenceCount + i];
                    }
                    out[i] = 0.5f * own[i] + 0.5f * sum / (float)near.size();
                }
            }
            weight.swap(smoothed);
        }

    } else {
        // joint planes (the default): each point mostly goes to the bone nearest in units of that bone's own
        // thickness, so the arm cannot claim the chest just because the spine lies deep inside it; then between a
        // bone and its parent the weight changes over a short band across the joint's plane, as hand-painted rigs
        // do, and only where the point is near that bone at all
        std::vector<int> parentOf(influenceCount, -1);
        std::vector<glm::vec3> direction(influenceCount, glm::vec3(0.0f, 1.0f, 0.0f));
        std::vector<glm::vec3> face(influenceCount, glm::vec3(0.0f));     // the head's: toward the face
        std::vector<glm::vec3> leaning(influenceCount, glm::vec3(0.0f));  // the head's plane in front of it
        const StandardSkeleton& standard = StandardSkeleton::get();
        for (int i = 0; i < influenceCount; i++) {
            const int j = influences[i];
            for (int a = joints[j].parent; a >= 0 && parentOf[i] < 0; a = joints[a].parent) {
                auto found = std::find(influences.begin(), influences.end(), a);
                if (found != influences.end()) {
                    parentOf[i] = (int)(found - influences.begin());
                }
            }
            const int standardIndex = standard.indexOf(joints[j].name);
            const int child = standardIndex >= 0 ? standard.boneChild(standardIndex) : -1;
            const int bone = child >= 0 && child < jointCount ? child : children[j].front();
            const glm::vec3 along = joints[bone].position - joints[j].position;
            if (glm::length(along) > 0.0f) {
                direction[i] = glm::normalize(along);
            }
            // the head hands over to the neck along the jaw line, as hand-painted rigs do: in front of the head joint
            // its plane leans 30 degrees down toward the face, so the chin and lower lip, below the head joint, are
            // the head's and the throat stays the neck's; behind, it stays level, where the neck meets the skull. A
            // level plane all round gave the chin to the neck, and a talking animation's nod opened the mouth (Chloe,
            // 10-13).
            if (joints[j].name == "Head") {
                const glm::vec3 up = direction[i];
                const glm::vec3 forward = glm::vec3(0.0f, 0.0f, 1.0f) - up * up.z;
                if (glm::length(forward) > 0.0f) {
                    const float lean = glm::radians(30.0f);
                    face[i] = glm::normalize(forward);
                    leaning[i] = glm::normalize(std::cos(lean) * up + std::sin(lean) * face[i]);
                }
            }
        }
        const float sharpness = std::max(options.sharpness, 0.1f);
        std::vector<float> near(influenceCount);
        for (size_t w = 0; w < weldCount; w++) {
            float* out = &weight[w * influenceCount];
            float nearest = std::numeric_limits<float>::max();
            for (int i = 0; i < influenceCount; i++) {
                const float d = distance[(size_t)i * weldCount + w];
                near[i] = std::isfinite(d) ? d / radius[i] : std::numeric_limits<float>::max();
                nearest = std::min(nearest, near[i]);
            }
            if (nearest == std::numeric_limits<float>::max()) {
                unreached++;
                out[nearestBone(w)] = 1.0f;
                continue;
            }
            float sum = 0.0f;
            for (int i = 0; i < influenceCount; i++) {
                const float behind = near[i] - nearest;
                if (behind < 4.0f / sharpness) {
                    out[i] = std::exp(-sharpness * behind);
                    sum += out[i];
                }
            }
            for (int i = 0; i < influenceCount; i++) {
                out[i] /= sum;
            }
            // across each joint, parents before children, the pair's weight changes over the joint's plane
            for (int i = 0; i < influenceCount; i++) {
                const int parent = parentOf[i];
                if (parent < 0 || near[i] - nearest > 1.0f) {
                    continue;
                }
                const float pair = out[parent] + out[i];
                if (pair <= 0.0f) {
                    continue;
                }
                // the hip joint hands over across a wider band: hand-painted rigs share the buttock between hips and leg;
                // the head across a narrow one, a crisp jaw line
                const QString& name = joints[influences[i]].name;
                const float width = name.endsWith("UpLeg") ? 2.0f * options.blend
                                    : name == "Head"      ? 0.4f * options.blend
                                                          : options.blend;
                const float halfBand = std::max(width * radius[i], size);
                const glm::vec3 offset = welded[w] - joints[influences[i]].position;
                const float along = glm::dot(offset, glm::dot(offset, face[i]) > 0.0f ? leaning[i] : direction[i]);
                const float t = glm::clamp((along + halfBand) / (2.0f * halfBand), 0.0f, 1.0f);
                const float f = t * t * (3.0f - 2.0f * t);
                out[i] = pair * f;
                out[parent] = pair * (1.0f - f);
            }
        }
    }

    // the four largest influences of each point, to every vertex at that point
    std::vector<VertexWeights> weldWeights(weldCount);
    for (size_t w = 0; w < weldCount; w++) {
        const float* row = &weight[w * influenceCount];
        int top[4] = { -1, -1, -1, -1 };
        for (int i = 0; i < influenceCount; i++) {
            if (row[i] <= 0.0f) {
                continue;
            }
            for (int k = 0; k < 4; k++) {
                if (top[k] < 0 || row[i] > row[top[k]]) {
                    for (int m = 3; m > k; m--) {
                        top[m] = top[m - 1];
                    }
                    top[k] = i;
                    break;
                }
            }
        }
        float sum = 0.0f;
        for (int k = 0; k < 4; k++) {
            if (top[k] >= 0) {
                sum += row[top[k]];
            }
        }
        for (int k = 0; k < 4 && sum > 0.0f; k++) {
            if (top[k] >= 0) {
                weldWeights[w].joints[k] = (uint16_t)influences[top[k]];
                weldWeights[w].weights[k] = row[top[k]] / sum;
            }
        }
    }
    result.weights.resize(body.vertices.size());
    for (size_t v = 0; v < body.vertices.size(); v++) {
        result.weights[v] = weldWeights[weldOf[v]];
    }
    result.notes << QString("skin: %1 joints carry weight over %2 points (%3 vertices), %4 out of the body's reach; "
                            "%5 threads, %6 s")
                        .arg(influenceCount).arg(weldCount).arg(body.vertices.size()).arg(unreached)
                        .arg(threadCount).arg(timer.elapsed() / 1000.0, 0, 'f', 1);
}
