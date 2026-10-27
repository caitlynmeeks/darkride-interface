//
//  Created by Bradley Austin Davis on 2015/09/05
//  Copyright 2013-2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once

#include <atomic>

#include <QtCore/qglobal.h>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>

#include <gpu/Shader.h>
#include <gpu/Pipeline.h>
#include <gpu/Batch.h>
#include <material-networking/ShaderCache.h>
#include <material-networking/TextureCache.h>
#include "ProceduralMaterialCache.h"

using UniformLambdas = std::list<std::function<void(gpu::Batch& batch)>>;
const size_t MAX_PROCEDURAL_TEXTURE_CHANNELS{ 4 };

/**jsdoc
 * An object containing user-defined uniforms for communicating data to shaders.
 * @typedef {object} ProceduralUniforms
 */

/**jsdoc
 * The data used to define a Procedural shader material.
 * @typedef {object} ProceduralData
 * @property {number} version=1 - The version of the procedural shader.
 * @property {string} vertexShaderURL - A link to a vertex shader.  Currently, only GLSL shaders are supported.  The shader must implement a different method depending on the version.
 *     If a procedural material contains a vertex shader, the bounding box of the material entity is used to cull the object to which the material is applied
 *     (a Material entity applied by tag leaves each model its own).
 * @property {string} fragmentShaderURL - A link to a fragment shader.  Currently, only GLSL shaders are supported.  The shader must implement a different method depending on the version.
 *     <code>shaderUrl</code> is an alias.
 * @property {string[]} channels=[] - An array of input texture URLs.  Currently, up to 4 are supported.
 *     <p>On a model, the fragment shader can also read the material beneath it (the model's own, or the layers below):
 *     <code>getUnderlyingAlbedo(uv)</code> (colour and opacity), <code>getUnderlyingNormal(uv)</code> (world space, with
 *     its normal map), <code>isUnderlyingMasked()</code>, <code>getUnderlyingOpacityCutoff()</code>,
 *     <code>getUnderlyingRoughness(uv)</code>, <code>getUnderlyingMetallic(uv)</code>, <code>getUnderlyingOcclusion(uv)</code>
 *     (an ORM texture gives all three) and <code>getUnderlyingEmissive()</code> (its factor),
 *     with <code>uv</code> the mesh's first texture coordinates, <code>_texCoord0</code>.</p>
 * @property {ProceduralUniforms} uniforms={} - A {@link ProceduralUniforms} object containing all the custom uniforms to be passed to the shader.
 */

struct ProceduralData {
    static QJsonValue getProceduralData(const QString& proceduralJson);
    static ProceduralData parse(const QString& userDataJson);
    void parse(const QJsonObject&);

    // Rendering object descriptions, from userData
    uint8_t version{ 0 };
    QUrl fragmentShaderUrl;
    QUrl vertexShaderUrl;
    QJsonObject uniforms;
    QJsonArray channels;
};

class ProceduralProgramKey {
public:
    enum FlagBit
    {
        IS_TRANSPARENT = 0,
        IS_SKINNED,
        IS_SKINNED_DQ,
        IS_FORWARD,

        NUM_FLAGS
    };
    typedef std::bitset<NUM_FLAGS> Flags;

    Flags _flags;

    bool isTransparent() const { return _flags[IS_TRANSPARENT]; }
    bool isSkinned() const { return _flags[IS_SKINNED]; }
    bool isSkinnedDQ() const { return _flags[IS_SKINNED_DQ]; }
    bool isForward() const { return _flags[IS_FORWARD]; }

    // forward: the view lights in place (the forward path), so an opaque procedural draws the variant that lights itself;
    // a transparent one lights itself in either path
    ProceduralProgramKey(bool transparent = false, bool isSkinned = false, bool isSkinnedDQ = false, bool forward = false) {
        _flags.set(IS_TRANSPARENT, transparent);
        _flags.set(IS_SKINNED, isSkinned);
        _flags.set(IS_SKINNED_DQ, isSkinnedDQ);
        _flags.set(IS_FORWARD, forward && !transparent);
    }
};
namespace std {
template <>
struct hash<ProceduralProgramKey> {
    size_t operator()(const ProceduralProgramKey& key) const {
        return std::hash<std::bitset<ProceduralProgramKey::FlagBit::NUM_FLAGS>>()(key._flags);
    }
};
}  // namespace std
inline bool operator==(const ProceduralProgramKey& a, const ProceduralProgramKey& b) {
    return a._flags == b._flags;
}
inline bool operator!=(const ProceduralProgramKey& a, const ProceduralProgramKey& b) {
    return a._flags != b._flags;
}

// FIXME better encapsulation
// FIXME better mechanism for extending to things rendered using shaders other than simple.slv
struct Procedural {
public:
    Procedural();
    void setProceduralData(const ProceduralData& proceduralData);

    bool isReady() const;
    bool isEnabled() const { return _enabled; }
    // this variant's program failed to compile on this GPU: known from the frame after its first prepare (the backend
    // compiles when it first draws with it, and draws nothing then)
    bool compilationFailed(const ProceduralProgramKey& key) const;
    // inputsBuffer, when given, takes this draw's standard inputs (iWorldPosition, iWorldScale, iWorldOrientation...)
    // instead of the procedural's own buffer: a buffer's content is uploaded once a frame, so one procedural drawn at
    // many places (a Material entity applied to every model with a tag) needs a buffer per place, or every draw gets
    // the last one's
    void prepare(gpu::Batch& batch,
                 const glm::vec3& position,
                 const glm::vec3& size,
                 const glm::quat& orientation,
                 const uint64_t& created,
                 const ProceduralProgramKey key = ProceduralProgramKey(),
                 const gpu::BufferPointer& inputsBuffer = nullptr);
    static gpu::BufferPointer makeInputsBuffer();

    glm::vec4 getColor(const glm::vec4& entityColor) const;
    uint64_t getFadeStartTime() const { return _fadeStartTime; }
    bool isFading() const { return _doesFade && _isFading; }
    void setIsFading(bool isFading) { _isFading = isFading; }
    void setDoesFade(bool doesFade) { _doesFade = doesFade; }

    bool hasVertexShader() const;
    void setBoundOperator(const std::function<AABox()>& boundOperator) { _boundOperator = boundOperator; }
    bool hasBoundOperator() const { return (bool)_boundOperator; }
    AABox getBound() { return _boundOperator(); }

    gpu::Shader::Source _vertexSource;
    gpu::Shader::Source _vertexSourceSkinned;
    gpu::Shader::Source _vertexSourceSkinnedDQ;
    gpu::Shader::Source _opaqueFragmentSource;
    gpu::Shader::Source _opaqueForwardFragmentSource;   // lit in place, for the forward path (empty: the opaque one)
    gpu::Shader::Source _transparentFragmentSource;

    gpu::StatePointer _opaqueState{ std::make_shared<gpu::State>() };
    gpu::StatePointer _transparentState{ std::make_shared<gpu::State>() };

    static std::function<void(gpu::StatePointer)> opaqueStencil;
    static std::function<void(gpu::StatePointer)> transparentStencil;

protected:
    // DO NOT TOUCH
    // We have to pack these in a particular way to match the ProceduralCommon.slh
    // layout.
    struct StandardInputs {
        vec4 date;
        vec4 position;
        vec4 scale;
        float timeSinceLastCompile;
        float timeSinceFirstCompile;
        float timeSinceEntityCreation;
        int frameCount;
        vec4 resolution[4];
        mat4 orientation;
    };

    static_assert(0 == offsetof(StandardInputs, date), "ProceduralOffsets");
    static_assert(16 == offsetof(StandardInputs, position), "ProceduralOffsets");
    static_assert(32 == offsetof(StandardInputs, scale), "ProceduralOffsets");
    static_assert(48 == offsetof(StandardInputs, timeSinceLastCompile), "ProceduralOffsets");
    static_assert(52 == offsetof(StandardInputs, timeSinceFirstCompile), "ProceduralOffsets");
    static_assert(56 == offsetof(StandardInputs, timeSinceEntityCreation), "ProceduralOffsets");
    static_assert(60 == offsetof(StandardInputs, frameCount), "ProceduralOffsets");
    static_assert(64 == offsetof(StandardInputs, resolution), "ProceduralOffsets");
    static_assert(128 == offsetof(StandardInputs, orientation), "ProceduralOffsets");

    // Procedural metadata
    ProceduralData _data;

    bool _enabled{ false };
    uint64_t _lastCompile{ 0 };
    uint64_t _firstCompile{ 0 };
    int32_t _frameCount{ 0 };

    // Rendering object descriptions
    QString _vertexShaderSource;
    QString _vertexShaderPath;
    uint64_t _vertexShaderModified{ 0 };
    NetworkShaderPointer _networkVertexShader;
    QString _fragmentShaderSource;
    QString _fragmentShaderPath;
    uint64_t _fragmentShaderModified{ 0 };
    NetworkShaderPointer _networkFragmentShader;
    bool _shaderDirty{ true };
    bool _uniformsDirty{ true };

    // Rendering objects
    UniformLambdas _uniforms;
    NetworkTexturePointer _channels[MAX_PROCEDURAL_TEXTURE_CHANNELS];

    std::unordered_map<ProceduralProgramKey, gpu::PipelinePointer> _proceduralPipelines;

    StandardInputs _standardInputs;
    gpu::BufferPointer _standardInputsBuffer;
    gpu::BufferPointer _currentInputsBuffer;   // the draw's (see prepare), or _standardInputsBuffer

    // Entity metadata
    glm::vec3 _entityDimensions;
    glm::vec3 _entityPosition;
    glm::mat3 _entityOrientation;
    uint64_t _entityCreated;

private:
    void setupUniforms();

    mutable uint64_t _fadeStartTime{ 0 };
    mutable bool _hasStartedFade{ false };
    mutable bool _isFading{ false };
    bool _doesFade{ true };
    ProceduralProgramKey _prevKey;

    std::function<AABox()> _boundOperator{ nullptr };

    mutable std::mutex _mutex;
};

namespace graphics {

class ProceduralMaterial : public NetworkMaterial {
public:
    ProceduralMaterial() : NetworkMaterial() { initializeProcedural(); }
    ProceduralMaterial(const NetworkMaterial& material) : NetworkMaterial(material) { initializeProcedural(); }

    bool isProcedural() const override { return true; }
    bool isEnabled() const override { return _procedural.isEnabled(); }
    bool isReady() const override { return _procedural.isReady(); }
    bool compilationFailed(const ProceduralProgramKey& key) const { return _procedural.compilationFailed(key); }
    QString getProceduralString() const override { return _proceduralString; }

    void setProceduralData(const QString& data) {
        _proceduralString = data;
        _procedural.setProceduralData(ProceduralData::parse(data));
    }
    glm::vec4 getColor(const glm::vec4& color) const { return _procedural.getColor(color); }
    bool isFading() const { return _procedural.isFading(); }
    void setIsFading(bool isFading) { _procedural.setIsFading(isFading); }
    uint64_t getFadeStartTime() const { return _procedural.getFadeStartTime(); }
    bool hasVertexShader() const { return _procedural.hasVertexShader(); }
    void prepare(gpu::Batch& batch,
                 const glm::vec3& position,
                 const glm::vec3& size,
                 const glm::quat& orientation,
                 const uint64_t& created,
                 const ProceduralProgramKey key = ProceduralProgramKey(),
                 const gpu::BufferPointer& inputsBuffer = nullptr) {
        _procedural.prepare(batch, position, size, orientation, created, key, inputsBuffer);
    }

    void initializeProcedural();

    void setBoundOperator(const std::function<AABox()>& boundOperator) { _procedural.setBoundOperator(boundOperator); }
    bool hasBoundOperator() const { return _procedural.hasBoundOperator(); }
    AABox getBound() { return _procedural.getBound(); }

private:
    QString _proceduralString;
    Procedural _procedural;
};
typedef std::shared_ptr<ProceduralMaterial> ProceduralMaterialPointer;

}  // namespace graphics