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

// The arc around a head at the origin, looking along -Z and level. The middle panel stays straight ahead whether or not
// the right one is folded, so nothing moves when the radio turns on or off. The tops line up.
inline Arrangement arrange(const SlotPixels& pixels, float degreesPerPixel) {
    const glm::vec3 UP { 0.0f, 1.0f, 0.0f };
    auto panel = [&](const glm::vec2& px, float yawDegrees, float top) {
        Pose pose;
        pose.size = { metresForDegrees(px.x * degreesPerPixel), metresForDegrees(px.y * degreesPerPixel) };
        pose.rotation = glm::angleAxis(glm::radians(yawDegrees), UP);
        pose.position = pose.rotation * glm::vec3(0.0f, top - pose.size.y * 0.5f, -DISTANCE);
        return pose;
    };
    const float middleDegrees = pixels.middle.x * degreesPerPixel;
    const float top = metresForDegrees(pixels.middle.y * degreesPerPixel) * 0.5f;

    Arrangement arrangement;
    arrangement.middle = panel(pixels.middle, 0.0f, top);
    const float leftDegrees = pixels.left.x * degreesPerPixel;
    arrangement.left = panel(pixels.left, (middleDegrees + leftDegrees) * 0.5f + GAP_DEGREES, top);
    const float rightDegrees = pixels.right.x * degreesPerPixel;
    arrangement.right = panel(pixels.right, -((middleDegrees + rightDegrees) * 0.5f + GAP_DEGREES), top);
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
