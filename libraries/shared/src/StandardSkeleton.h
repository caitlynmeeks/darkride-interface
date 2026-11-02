//
//  StandardSkeleton.h
//  libraries/shared/src
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_StandardSkeleton_h
#define hifi_StandardSkeleton_h

#include <vector>

#include <QString>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace autorig {

struct TemplateJoint {
    const char* name;
    const char* parent;
    glm::vec3 position;  // Mixamo's T-pose, centimetres
    glm::quat rotation;  // the joint's absolute axes in that pose
};

// The skeleton every Darkride avatar uses: the default avatar's 67 joints with Mixamo's names, hierarchy and joint
// axes. Animations copy each joint's absolute rotation onto the avatar (AnimClip.cpp), so the axes must be these.
class StandardSkeleton {
public:
    static const StandardSkeleton& get();

    int size() const { return (int)_parents.size(); }
    const TemplateJoint& joint(int index) const;
    int parentIndex(int index) const { return _parents[index]; }
    int indexOf(const QString& name) const;
    // The child whose direction is the bone's direction, or -1 for the end joints.
    int boneChild(int index) const { return _boneChildren[index]; }

private:
    StandardSkeleton();

    std::vector<int> _parents;
    std::vector<int> _boneChildren;
};

}

#endif // hifi_StandardSkeleton_h
