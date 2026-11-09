//
//  ReflectionProbeStage.h
//  render-utils/src
//
//  The scene's reflection probes (graphics::ReflectionProbe, LIGHTMAPS.md §7) and their cube map array: one layer a
//  probe, all of one size and format, which the frame's light culling (LightClusters) lists the probes in view by
//  and the shaders sample (ReflectionProbe.slh). The entity renderers add and remove probes and hand them their cube
//  maps as they load; the array is rebuilt, on the render thread, when the set of probes with a cube map changes.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_render_utils_ReflectionProbeStage_h
#define hifi_render_utils_ReflectionProbeStage_h

#include <vector>

#include <gpu/Texture.h>
#include <graphics/ReflectionProbe.h>
#include <render/IndexedContainer.h>
#include <render/Stage.h>
#include <render/Engine.h>

class ReflectionProbeStage : public render::Stage {
public:
    static std::string _stageName;
    static const std::string& getName() { return _stageName; }

    using Index = render::indexed_container::Index;
    static const Index INVALID_INDEX;
    static bool isIndexInvalid(Index index) { return index == INVALID_INDEX; }

    using ProbePointer = graphics::ReflectionProbePointer;
    using Probes = render::indexed_container::IndexedPointerVector<graphics::ReflectionProbe>;

    // the layers the array can hold (the frame's list holds as many, LIGHT_CLUSTER_MAX_PROBES), and the largest face
    // a layer has: a bigger cube map contributes its smaller mips
    static constexpr int MAX_LAYERS = 32;   // constexpr: used by reference (std::min), so an unoptimized build links
    static constexpr int MAX_FACE_SIZE = 512;

    Index addProbe(const ProbePointer& probe);
    ProbePointer removeProbe(Index index);
    bool checkProbeId(Index index) const { return _probes.checkIndex(index); }
    Index getNumProbes() const { return _probes.getNumElements(); }
    Index getNumAllocatedProbes() const { return _probes.getNumAllocatedIndices(); }
    ProbePointer getProbe(Index index) const { return _probes.get(index); }

    // The cube map array of the probes that have a cube map: built again when that set, or one of the cube maps,
    // changes (render thread, once a frame before the culling). A probe without a layer is not in the frame's list.
    void updateArray();
    const gpu::TexturePointer& getArray() const { return _array; }
    int getArrayNumMips() const { return _arrayNumMips; }
    int getLayer(Index index) const { return (index >= 0 && index < (Index)_layers.size()) ? _layers[index] : -1; }
    // one black layer, for the shaders' sampler when the frame has no probe
    static const gpu::TexturePointer& getEmptyArray();

protected:
    struct ArrayEntry {
        Index index { INVALID_INDEX };
        const gpu::Texture* texture { nullptr };
        gpu::Stamp dataStamp { 0 };
        bool operator==(const ArrayEntry& other) const {
            return index == other.index && texture == other.texture && dataStamp == other.dataStamp;
        }
    };

    Probes _probes;
    std::vector<ArrayEntry> _arrayEntries;
    std::vector<int> _layers;
    gpu::TexturePointer _array;
    int _arrayNumMips { 0 };
};
using ReflectionProbeStagePointer = std::shared_ptr<ReflectionProbeStage>;

class ReflectionProbeStageSetup {
public:
    using JobModel = render::Job::Model<ReflectionProbeStageSetup>;

    ReflectionProbeStageSetup() {}
    void run(const render::RenderContextPointer& renderContext);
};

#endif // hifi_render_utils_ReflectionProbeStage_h
