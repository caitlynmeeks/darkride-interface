//
//  EntityScriptRights.cpp
//  libraries/entities/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "EntityScriptRights.h"

#include <DependencyManager.h>
#include <NodeList.h>

namespace entity_script_rights {

bool mayChangeScripts() {
    auto nodeList = DependencyManager::get<NodeList>();
    return nodeList && (nodeList->isAllowedEditor() || nodeList->getThisNodeCanRez() ||
        nodeList->getThisNodeCanRezCertified());
}

bool mayChangeScriptsOf(bool entityLocked) {
    return !entityLocked && mayChangeScripts();
}

}
