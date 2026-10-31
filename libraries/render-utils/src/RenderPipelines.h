//
//  RenderPipelines.h
//  render-utils/src/
//
//  Created by Sam Gondelman on 2/15/18
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#ifndef hifi_RenderPipelines_h
#define hifi_RenderPipelines_h

#include <graphics/Material.h>
#include <render/Args.h>

class RenderPipelines {
public:
    static void updateMultiMaterial(graphics::MultiMaterial& multiMaterial);
    static bool bindMaterial(graphics::MaterialPointer& material, gpu::Batch& batch, render::Args::RenderMode renderMode, bool enableTextures);
    static bool bindMaterials(graphics::MultiMaterial& multiMaterial, gpu::Batch& batch, render::Args::RenderMode renderMode, bool enableTextures);
    // for a procedural on top (simple_procedural.slf's getUnderlying* functions): the schema of the layers beneath it
    // and their albedo, normal, roughness, metallic and occlusion maps (its channels take units 2 to 5)
    static void bindUnderlyingMaterial(graphics::MultiMaterial& multiMaterial, gpu::Batch& batch);
    // the same for a procedural with nothing beneath it (a shape, a Material entity's own sphere): the defaults, so it
    // does not read the last model part's
    static void bindDefaultUnderlyingMaterial(gpu::Batch& batch);
};


#endif // hifi_RenderPipelines_h
