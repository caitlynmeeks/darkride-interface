//
//  Created by Sabrina Shanman 2018/08/22
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "PickTransformNode.h"

#include "DependencyManager.h"
#include "PickManager.h"

// The parent is decided here, when the node is made, because that is the maker's call (its transform is read
// later, on the main thread, for no one in particular): a world's script parents its pick only to a pick a world's
// script made. Any other number is, for it, no pick: the node has no parent and gives the identity transform.
PickTransformNode::PickTransformNode(unsigned int uid) :
    _uid(0)
{
    auto pickManager = DependencyManager::get<PickManager>();
    if (pickManager && pickManager->callerMayName(uid)) {
        _uid = uid;
    }
}

Transform PickTransformNode::getTransform() {
    auto pickManager = DependencyManager::get<PickManager>();
    if (!pickManager) {
        return Transform();
    }

    return pickManager->getResultTransform(_uid);
}

QVariantMap PickTransformNode::toVariantMap() const {
    QVariantMap map;
    map["parentID"] = _uid;
    return map;
}
