//
//  main.cpp
//  tools/auto-rig/src
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <DependencyManager.h>
#include <ResourceManager.h>
#include <SharedUtil.h>

#include "AutoRigApp.h"

int main(int argc, char* argv[]) {
    setupHifiApplication("Auto Rig");
    // the glTF and Assimp loaders resolve file names through it
    DependencyManager::set<ResourceManager>(false);
    AutoRigApp app(argc, argv);
    return app.getReturnCode();
}
