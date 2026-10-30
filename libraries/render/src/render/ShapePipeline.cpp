//
//  ShapePipeline.cpp
//  render/src/render
//
//  Created by Zach Pomerantz on 12/31/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ShapePipeline.h"

#include <PerfStat.h>
#include <DependencyManager.h>
#include <gpu/ShaderConstants.h>
#include <graphics/ShaderConstants.h>
#include <../render-utils/src/render-utils/ShaderConstants.h>

#include "Logging.h"


using namespace render;

namespace ru {
    using render_utils::slot::texture::Texture;
    using render_utils::slot::buffer::Buffer;
}

namespace gr {
    using graphics::slot::texture::Texture;
    using graphics::slot::buffer::Buffer;
}

ShapePipeline::CustomFactoryMap ShapePipeline::_globalCustomFactoryMap;

ShapePipeline::CustomKey ShapePipeline::registerCustomShapePipelineFactory(CustomFactory factory) {  
    ShapePipeline::CustomKey custom = (ShapePipeline::CustomKey) _globalCustomFactoryMap.size() + 1;
    _globalCustomFactoryMap[custom] = factory;
    return custom;  
}


void ShapePipeline::prepare(gpu::Batch& batch, RenderArgs* args) {
    if (_batchSetter) {
        _batchSetter(*this, batch, args);
    }
}

void ShapePipeline::prepareShapeItem(RenderArgs* args, const ShapeKey& key, const Item& shape) {
    if (_itemSetter) {
        _itemSetter(*this, args, shape);
    }
}


ShapeKey::Filter::Builder::Builder() {
    _mask.set(OWN_PIPELINE);
    _mask.set(INVALID);
}

ShapePlumber::PipelineMap::iterator ShapePlumber::resolvePipeline(const Key& key) const {
    auto thisThread = std::this_thread::get_id();
    if (_pickThread == std::thread::id()) {
        _pickThread = thisThread;
    } else if (_pickThread != thisThread && !_warnedOfSecondThread) {
        _warnedOfSecondThread = true;   // once per plumber: a caller on the wrong thread mustn't flood the log
        qCCritical(renderlogging) << "ShapePlumber: a pipeline looked up from a second thread; a plumber isn't thread-safe";
        assert(false);
    }

    // The first filter added that matches the key gives its pipeline, as when every filter was expanded into the map
    // in order and insert kept the first
    for (const auto& filtered : _filteredPipelines) {
        if (filtered.filter.matches(key)) {
            auto entry = _pipelineMap.emplace(key, filtered.pipeline).first;
            auto size = _pipelineMap.size();
            if (size >= 64 && (size & (size - 1)) == 0) {
                qCDebug(renderlogging) << "ShapePlumber:" << size << "keys drawn from" << _filteredPipelines.size() << "filters";
            }
            return entry;
        }
    }
    return _pipelineMap.end();
}

ShapePlumber::PipelinePointer ShapePlumber::findPipeline(const Key& key) const {
    auto pipelineIterator = _pipelineMap.find(key);
    if (pipelineIterator == _pipelineMap.end()) {
        pipelineIterator = resolvePipeline(key);
    }
    return pipelineIterator != _pipelineMap.end() ? pipelineIterator->second : PipelinePointer();
}

void ShapePlumber::addPipeline(const Key& key, const gpu::ShaderPointer& program, const gpu::StatePointer& state,
        BatchSetter batchSetter, ItemSetter itemSetter) {
    addPipeline(Filter{key}, program, state, batchSetter, itemSetter);
}

void ShapePlumber::addPipeline(const Filter& filter, const gpu::ShaderPointer& program, const gpu::StatePointer& state,
        BatchSetter batchSetter, ItemSetter itemSetter) {
    ShapeKey key{ filter._flags };
    auto reflection = program->getReflection();
    auto locations = std::make_shared<Locations>();
    locations->albedoTextureUnit = reflection.validTexture(graphics::slot::texture::MaterialAlbedo);
    locations->roughnessTextureUnit = reflection.validTexture(graphics::slot::texture::MaterialRoughness);
    locations->normalTextureUnit = reflection.validTexture(graphics::slot::texture::MaterialNormal);
    locations->metallicTextureUnit = reflection.validTexture(graphics::slot::texture::MaterialMetallic);
    locations->emissiveTextureUnit = reflection.validTexture(graphics::slot::texture::MaterialEmissive);
    locations->occlusionTextureUnit = reflection.validTexture(graphics::slot::texture::MaterialOcclusion);
    locations->lightingModelBufferUnit = reflection.validUniformBuffer(render_utils::slot::buffer::LightModel);
    locations->skinClusterBufferUnit = reflection.validUniformBuffer(graphics::slot::buffer::Skinning);
    locations->materialBufferUnit = reflection.validUniformBuffer(graphics::slot::buffer::Material);
    locations->keyLightBufferUnit = reflection.validUniformBuffer(graphics::slot::buffer::KeyLight);
    locations->lightBufferUnit = reflection.validUniformBuffer(graphics::slot::buffer::Light);
    locations->lightAmbientBufferUnit = reflection.validUniformBuffer(graphics::slot::buffer::AmbientLight);
    locations->lightAmbientMapUnit = reflection.validTexture(graphics::slot::texture::Skybox);
    locations->fadeMaskTextureUnit = reflection.validTexture(render_utils::slot::texture::FadeMask);
    locations->fadeParameterBufferUnit = reflection.validUniformBuffer(render_utils::slot::buffer::FadeParameters);
    locations->fadeObjectParameterBufferUnit = reflection.validUniformBuffer(render_utils::slot::buffer::FadeObjectParameters);
    locations->hazeParameterBufferUnit = reflection.validUniformBuffer(graphics::slot::buffer::HazeParams);
    if (key.isTranslucent()) {
        locations->lightClusterParamsBufferUnit = reflection.validUniformBuffer(render_utils::slot::buffer::LightClusterParams);
        locations->lightClusterTilesBufferUnit = reflection.validUniformBuffer(render_utils::slot::buffer::LightClusterTiles);
        locations->lightClusterBinsBufferUnit = reflection.validUniformBuffer(render_utils::slot::buffer::LightClusterBins);
    }

    {
        PROFILE_RANGE(app, "Pipeline::create");
        auto gpuPipeline = gpu::Pipeline::create(program, state);
        auto shapePipeline = std::make_shared<Pipeline>(gpuPipeline, locations, batchSetter, itemSetter);
        _filteredPipelines.push_back({ filter, shapePipeline });
        // a key no filter matched so far may match this one
        _missingKeys.clear();
    }
}

const ShapePipelinePointer ShapePlumber::pickPipeline(RenderArgs* args, const Key& key) const {
    assert(!_filteredPipelines.empty() || !_pipelineMap.empty());
    assert(args);
    assert(args->_batch);

    PerformanceTimer perfTimer("ShapePlumber::pickPipeline");

    auto pipelineIterator = _pipelineMap.find(key);
    if (pipelineIterator == _pipelineMap.end() && _missingKeys.find(key) == _missingKeys.end()) {
        // the first time this key is drawn
        pipelineIterator = resolvePipeline(key);
    }
    if (pipelineIterator == _pipelineMap.end()) {
        // The first time we can't find a pipeline, we should try things to solve that
        if (_missingKeys.find(key) == _missingKeys.end()) {
            if (key.isCustom()) {
                auto factoryIt = ShapePipeline::_globalCustomFactoryMap.find(key.getCustom());
                if ((factoryIt != ShapePipeline::_globalCustomFactoryMap.end()) && (factoryIt)->second) {
                    // found a factory for the custom key, can now generate a shape pipeline for this case:
                    _pipelineMap.emplace(key, (factoryIt)->second(*this, key, args));

                    return pickPipeline(args, key);
                } else {
                    qCDebug(renderlogging) << "ShapePlumber::Couldn't find a custom pipeline factory for " << key.getCustom() << " key is: " << key;
                }
            }

           _missingKeys.insert(key);
            qCDebug(renderlogging) << "ShapePlumber::Couldn't find a pipeline for" << key;
        }
        return PipelinePointer(nullptr);
    }

    PipelinePointer shapePipeline(pipelineIterator->second);

    // Setup the one pipeline (to rule them all)
    args->_batch->setPipeline(shapePipeline->pipeline);

    // Run the pipeline's BatchSetter on the passed in batch
    if (shapePipeline->_batchSetter) {
        shapePipeline->_batchSetter(*shapePipeline, *(args->_batch), args);
    }

    return shapePipeline;
}
