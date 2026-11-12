//
//  VrScreenLayout.h
//  interface/src/ui
//
//  Where the VR screen's pieces go: an arc of flat panels around the head, each facing it, and a grab bar under the
//  middle one. Arithmetic only, in the headset's play space (metres, the sensor frame), so it can be tested without one.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#pragma once

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace vrscreen {

const float DISTANCE = 1.2f;                     // from the head, a little over arm's length
const float GAP_DEGREES = 1.5f;                  // between panels
const float DEFAULT_PITCH_DEGREES = -8.0f;       // a little below the eye line, tilted toward you
const float PARKED_PITCH_DEGREES = 35.0f;        // let go above this, the screen is parked overhead
const float MIN_PITCH_DEGREES = -60.0f;          // low like a dashboard
const float MAX_PITCH_DEGREES = 75.0f;
const glm::vec2 BAR_SIZE { 0.24f, 0.035f };
// The bar's capsule (ScreenGrabBar.qml) is this much of the bar's height, centred on it; mary's 2 cm (11-02) is between
// the middle panel's bottom edge and the capsule's top, so the bar's own top (the laser's target) is a little nearer
const float CAPSULE_HEIGHT = 0.47f;
const float CAPSULE_GAP = 0.02f;
const float BAR_GAP = CAPSULE_GAP - BAR_SIZE.y * (1.0f - CAPSULE_HEIGHT) * 0.5f;

// How far the screen is: pushed away or pulled in with the hand on a drag (Caity, 11-02: 0.7x-1.5x of DISTANCE). The
// panels keep their size in metres and their pixels at any distance, so nearer looks bigger (and a little softer)
const float MIN_DISTANCE = 0.7f * DISTANCE;
const float MAX_DISTANCE = 1.5f * DISTANCE;
// the screen moves this much farther for each metre the hand moves away from the head: a whole arm's reach (about 0.3 m
// from pulled in to stretched out) covers the range
const float DEPTH_GAIN = 3.0f;

// A layout pixel's size by angle sets the type's size. The tablet's is 0.058 degrees (0.525 mm a pixel at about 0.515 m).
const float DEFAULT_DEGREES_PER_PIXEL = 0.058f;

// A panel's surface has more pixels than its layout, up to what the headset can show: its pixels per degree (Quest 3: 25,
// rendering's number; Part 2 asks the runtime). Panels scales the page by surface width / layout width.
const float DEFAULT_PIXELS_PER_DEGREE = 25.0f;

inline float textureScale(float degreesPerPixel, float pixelsPerDegree = DEFAULT_PIXELS_PER_DEGREE) {
    return std::fmax(1.0f, degreesPerPixel * pixelsPerDegree);
}

struct Pose {
    glm::vec3 position { 0.0f };
    glm::quat rotation { 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec2 size { 0.0f };   // metres
};

struct SlotPixels {
    glm::vec2 left { 0.0f };
    glm::vec2 middle { 0.0f };
    glm::vec2 right { 0.0f };   // zero while it's folded away (the radio off)
};

struct Arrangement {
    Pose left, middle, right, bar;
};

// a flat panel of this many degrees across, facing the head at this distance
inline float metresForDegrees(float degrees, float distance = DISTANCE) {
    return 2.0f * distance * std::tan(glm::radians(degrees) * 0.5f);
}

// the angle a flat width takes facing the head at this distance
inline float degreesForMetres(float metres, float distance) {
    return glm::degrees(2.0f * std::atan(metres * 0.5f / distance));
}

// The arc around a head at the origin, looking along -Z and level, at a distance from MIN_DISTANCE to MAX_DISTANCE. A
// panel's size in metres comes from its pixels at DISTANCE, whatever the distance (the same surface, nearer looks
// bigger), and the arc closes up around it, so the panels and their gaps stay side by side. The middle panel stays
// straight ahead whether or not the right one is folded, so nothing moves when the radio turns on or off. The tops line
// up.
inline Arrangement arrange(const SlotPixels& pixels, float degreesPerPixel, float distance = DISTANCE) {
    const glm::vec3 UP { 0.0f, 1.0f, 0.0f };
    distance = glm::clamp(distance, MIN_DISTANCE, MAX_DISTANCE);
    auto size = [&](const glm::vec2& px) {
        return glm::vec2(metresForDegrees(px.x * degreesPerPixel), metresForDegrees(px.y * degreesPerPixel));
    };
    auto panel = [&](const glm::vec2& px, float yawDegrees, float top) {
        Pose pose;
        pose.size = size(px);
        pose.rotation = glm::angleAxis(glm::radians(yawDegrees), UP);
        pose.position = pose.rotation * glm::vec3(0.0f, top - pose.size.y * 0.5f, -distance);
        return pose;
    };
    const float gapDegrees = degreesForMetres(metresForDegrees(GAP_DEGREES), distance);
    const float middleDegrees = degreesForMetres(size(pixels.middle).x, distance);
    const float top = size(pixels.middle).y * 0.5f;

    Arrangement arrangement;
    arrangement.middle = panel(pixels.middle, 0.0f, top);
    const float leftDegrees = degreesForMetres(size(pixels.left).x, distance);
    arrangement.left = panel(pixels.left, (middleDegrees + leftDegrees) * 0.5f + gapDegrees, top);
    const float rightDegrees = degreesForMetres(size(pixels.right).x, distance);
    arrangement.right = panel(pixels.right, -((middleDegrees + rightDegrees) * 0.5f + gapDegrees), top);
    arrangement.bar.size = BAR_SIZE;
    arrangement.bar.position = arrangement.middle.position -
        glm::vec3(0.0f, arrangement.middle.size.y * 0.5f + BAR_GAP + BAR_SIZE.y * 0.5f, 0.0f);
    return arrangement;
}

// A piece of the arc, turned with the whole screen around the head: positive yaw is to the left, positive pitch is up.
// Turning around the head keeps every panel facing it, so a parked screen tilts down at you with no extra work.
inline Pose placed(const Pose& piece, const glm::vec3& head, float yawDegrees, float pitchDegrees) {
    const glm::quat turn = glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(glm::radians(pitchDegrees), glm::vec3(1.0f, 0.0f, 0.0f));
    Pose pose;
    pose.position = head + turn * piece.position;
    pose.rotation = turn * piece.rotation;
    pose.size = piece.size;
    return pose;
}

// a direction's yaw (positive to the left of -Z) and pitch (positive up), degrees
inline float yawOf(const glm::vec3& direction) {
    return glm::degrees(std::atan2(-direction.x, -direction.z));
}
inline float pitchOf(const glm::vec3& direction) {
    return glm::degrees(std::asin(glm::clamp(glm::normalize(direction).y, -1.0f, 1.0f)));
}

// the way the screen's middle faces out from the head, turned as placed() turns it
inline glm::vec3 directionOf(float yawDegrees, float pitchDegrees) {
    return glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(glm::radians(pitchDegrees), glm::vec3(1.0f, 0.0f, 0.0f)) * glm::vec3(0.0f, 0.0f, -1.0f);
}

// The distance on a drag: the hand's push, how much farther from the head it is than at the start (metres, negative
// for a pull), moves the screen DEPTH_GAIN times as far, within its bounds
inline float pushedDistance(float startDistance, float push) {
    return glm::clamp(startDistance + DEPTH_GAIN * push, MIN_DISTANCE, MAX_DISTANCE);
}

// A ray (a hand's laser) on a flat piece, with a margin all round (metres): where the laser meets it, or past it
inline bool rayHits(const Pose& piece, const glm::vec3& origin, const glm::vec3& direction, float margin = 0.0f) {
    const glm::vec3 normal = piece.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    const float facing = glm::dot(direction, normal);
    if (std::fabs(facing) < 1.0e-4f) {
        return false;
    }
    const float along = glm::dot(piece.position - origin, normal) / facing;
    if (along <= 0.0f) {
        return false;
    }
    const glm::vec3 local = glm::inverse(piece.rotation) * (origin + direction * along - piece.position);
    return std::fabs(local.x) <= piece.size.x * 0.5f + margin && std::fabs(local.y) <= piece.size.y * 0.5f + margin;
}

// the shortest way between two yaws, degrees
inline float yawDifference(float to, float from) {
    float difference = std::fmod(to - from, 360.0f);
    if (difference > 180.0f) {
        difference -= 360.0f;
    } else if (difference < -180.0f) {
        difference += 360.0f;
    }
    return difference;
}

inline bool isParked(float pitchDegrees) {
    return pitchDegrees >= PARKED_PITCH_DEGREES;
}

// Web entity sizes: the surface's pixels are a Web entity's metres x METRES_TO_INCHES x its whole-number dpi. The dpi is
// rounded from the wanted width, then the metres are set back from it, so the surface has exactly the pixels asked for.
const float METRES_TO_INCHES = 39.3701f;

inline float dpiForWidth(float pixels, float metres) {
    return std::fmax(1.0f, std::round(pixels / (metres * METRES_TO_INCHES)));
}
inline glm::vec2 metresForPixels(const glm::vec2& pixels, float dpi) {
    return pixels / (dpi * METRES_TO_INCHES);
}

}   // namespace vrscreen
