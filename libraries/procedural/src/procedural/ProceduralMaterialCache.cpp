//
//  Created by Sam Gondelman on 2/9/2018
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "ProceduralMaterialCache.h"

#include "QJsonObject"
#include "QJsonDocument"
#include "QJsonArray"

#include "RegisteredMetaTypes.h"

#include "Procedural.h"

NetworkMaterialResource::NetworkMaterialResource(const QUrl& url) : Resource(url) {
}

void NetworkMaterialResource::downloadFinished(const QByteArray& data) {
    parsedMaterials.reset();

    if (_url.toString().contains(".json")) {
        parsedMaterials = parseJSONMaterials(QJsonDocument::fromJson(data), _url);
    }

    // TODO: parse other material types

    finishedLoading(true);
}

/**jsdoc
 * <p>An RGB or SRGB color value.</p>
 * <table>
 *   <thead>
 *     <tr><th>Index</th><th>Type</th><th>Attributes</th><th>Default</th><th>Value</th></tr>
 *   </thead>
 *   <tbody>
 *     <tr><td><code>0</code></td><td>number</td><td></td><td></td>
 *       <td>Red component value. Number in the range <code>0.0</code> &ndash; <code>1.0</code>.</td></tr>
 *     <tr><td><code>1</code></td><td>number</td><td></td><td></td>
 *       <td>Green component value. Number in the range <code>0.0</code> &ndash; <code>1.0</code>.</td></tr>
 *     <tr><td><code>2</code></td><td>number</td><td></td><td></td>
 *       <td>Blue component value. Number in the range <code>0.0</code> &ndash; <code>1.0</code>.</td></tr>
 *     <tr><td><code>3</code></td><td>boolean</td><td>&lt;optional&gt;</td><td>false</td>
 *       <td>If <code>true</code> then the color is an SRGB color.</td></tr>
 *   </tbody>
 * </table>
 * @typedef {array} RGBS
 */
bool NetworkMaterialResource::parseJSONColor(const QJsonValue& array, glm::vec3& color, bool& isSRGB) {
    if (array.isArray()) {
        QJsonArray colorArray = array.toArray();
        if (colorArray.size() >= 3 && colorArray[0].isDouble() && colorArray[1].isDouble() && colorArray[2].isDouble()) {
            isSRGB = true;
            if (colorArray.size() >= 4) {
                if (colorArray[3].isBool()) {
                    isSRGB = colorArray[3].toBool();
                }
            }
            color = glm::vec3(colorArray[0].toDouble(), colorArray[1].toDouble(), colorArray[2].toDouble());
            return true;
        }
    } else if (array.isObject()) {
        bool toReturn;
        isSRGB = true;
        color = vec3FromVariant(array.toObject(), toReturn);
        return toReturn;
    }
    return false;
}

// URP/Lit's Smoothness when a material doesn't give one
static const float UNITY_DEFAULT_SMOOTHNESS { 0.5f };

// the channel of a packed picture a map is read from: "r", "g", "b" or "a" (or "red", "green", "blue", "alpha");
// anything else reads the picture as it is
static image::ColorChannel parseJSONChannel(const QJsonValue& value) {
    QString name = value.toString().toLower();
    if (name == "r" || name == "red") {
        return image::ColorChannel::RED;
    } else if (name == "g" || name == "green") {
        return image::ColorChannel::GREEN;
    } else if (name == "b" || name == "blue") {
        return image::ColorChannel::BLUE;
    } else if (name == "a" || name == "alpha") {
        return image::ColorChannel::ALPHA;
    }
    return image::ColorChannel::NONE;
}

/**jsdoc
 * A material or set of materials used by a {@link Entities.EntityType|Material entity}.
 * @typedef {object} Entities.MaterialResource
 * @property {number} materialVersion=1 - The version of the material. <em>Currently not used.</em>
 * @property {Entities.Material|Entities.Material[]} materials - The details of the material or materials.
 */
NetworkMaterialResource::ParsedMaterials NetworkMaterialResource::parseJSONMaterials(const QJsonDocument& materialJSON,
                                                                                     const QUrl& baseUrl) {
    ParsedMaterials toReturn;
    if (!materialJSON.isNull() && materialJSON.isObject()) {
        QJsonObject materialJSONObject = materialJSON.object();
        for (auto& key : materialJSONObject.keys()) {
            if (key == "materialVersion") {
                auto value = materialJSONObject.value(key);
                if (value.isDouble()) {
                    toReturn.version = (uint)value.toInt();
                }
            } else if (key == "materials") {
                auto materialsValue = materialJSONObject.value(key);
                if (materialsValue.isArray()) {
                    QJsonArray materials = materialsValue.toArray();
                    for (auto material : materials) {
                        if (!material.isNull() && material.isObject()) {
                            auto parsedMaterial = parseJSONMaterial(material.toObject(), baseUrl);
                            toReturn.networkMaterials[parsedMaterial.first] = parsedMaterial.second;
                            toReturn.names.push_back(parsedMaterial.first);
                        }
                    }
                } else if (materialsValue.isObject()) {
                    auto parsedMaterial = parseJSONMaterial(materialsValue.toObject(), baseUrl);
                    toReturn.networkMaterials[parsedMaterial.first] = parsedMaterial.second;
                    toReturn.names.push_back(parsedMaterial.first);
                }
            }
        }
    }

    return toReturn;
}

/**jsdoc
 * A material used in a {@link Entities.MaterialResource|MaterialResource}.
 * @typedef {object} Entities.Material
 * @property {string} name="" - A name for the material. Supported by all material models.
 * @property {string} model="hifi_pbr" - Different material models support different properties and rendering modes.
 *     Supported models are: <code>"hifi_pbr"</code>, <code>"hifi_shader_simple"</code>.
 * @property {ColorFloat|RGBS|string} emissive - The emissive color, i.e., the color that the material emits. A 
 *     {@link ColorFloat} value is treated as sRGB and must have component values in the range <code>0.0</code> &ndash; 
 *     <code>1.0</code>. A {@link RGBS} value can be either RGB or sRGB. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {number|string} opacity=1.0 - The opacity, range <code>0.0</code> &ndash; <code>1.0</code>.
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> and
 *     <code>"hifi_shader_simple"</code> models only.
 * @property {boolean|string} unlit=false - <code>true</code> if the material is unaffected by lighting, <code>false</code> if 
 *     it is lit by the key light and local lights.
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {ColorFloat|RGBS|string} albedo - The albedo color. A {@link ColorFloat} value is treated as sRGB and must have
 *     component values in the range <code>0.0</code> &ndash; <code>1.0</code>. A {@link RGBS} value can be either RGB or sRGB.
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> and
 *     <code>"hifi_shader_simple"</code> models only.
 * @property {number|string} roughness - The roughness, range <code>0.0</code> &ndash; <code>1.0</code>. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {number|string} metallic - The metallicness, range <code>0.0</code> &ndash; <code>1.0</code>. 
 *     With a <code>"unity"</code> packing and a <code>metallicMap</code>, or an <code>"orm"</code> packing and an
 *     <code>ormMap</code>, it goes unused.
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} packing="custom" - How occlusion, roughness and metallic are packed into pictures:
 *     <ul>
 *         <li><code>"unity"</code>: as Unity's URP/Lit packs them. <code>metallicMap</code> holds metallic in red and 
 *         smoothness in alpha, <code>smoothness</code> scales the smoothness, and <code>occlusionMap</code> is read from
 *         green.</li>
 *         <li><code>"orm"</code>: as glTF and Unreal pack them. <code>ormMap</code> holds occlusion, roughness and metallic
 *         in red, green and blue.</li>
 *         <li><code>"custom"</code> (the default): each map is a picture of its own, read from red unless its 
 *         <code>&lt;map&gt;Channel</code> says otherwise.</li>
 *     </ul>
 *     Each packing ignores the others' keys, so a material can keep the maps of several and switch between them.
 *     <code>"hifi_pbr"</code> model only.
 * @property {number|string} smoothness=0.5 - With a <code>"unity"</code> packing, the smoothness, range <code>0.0</code>
 *     &ndash; <code>1.0</code>, used instead of <code>roughness</code>; with a <code>metallicMap</code>, it scales the map's
 *     smoothness, as URP/Lit's Smoothness does.
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} smoothnessSource="metallicAlpha" - With a <code>"unity"</code> packing, where smoothness comes from:
 *     <code>"metallicAlpha"</code>, the <code>metallicMap</code>'s alpha, or <code>"albedoAlpha"</code>, the
 *     <code>albedoMap</code>'s alpha (which then isn't opacity). <code>"hifi_pbr"</code> model only.
 * @property {string} ormMap - With an <code>"orm"</code> packing, the URL of the picture holding occlusion, roughness and
 *     metallic in red, green and blue. Its values are used as they are: <code>roughness</code> and <code>metallic</code>
 *     apply only without it.
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {number} occlusionStrength=1.0 - How much the occlusion map darkens, range <code>0.0</code> (not at all)
 *     &ndash; <code>1.0</code> (fully). <code>"hifi_pbr"</code> model only.
 * @property {string} roughnessMapChannel - With a <code>"custom"</code> packing, the channel of <code>roughnessMap</code>'s
 *     picture to read: <code>"r"</code>, <code>"g"</code>, <code>"b"</code> or <code>"a"</code>. So are 
 *     <code>glossMapChannel</code>, <code>metallicMapChannel</code>, <code>specularMapChannel</code> and
 *     <code>occlusionMapChannel</code> for their maps. Without one, a map is read from red.
 *     <code>"hifi_pbr"</code> model only.
 * @property {number|string} scattering - The scattering, range <code>0.0</code> &ndash; <code>1.0</code>. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} emissiveMap - The URL of the emissive texture image. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} albedoMap - The URL of the albedo texture image. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} opacityMap - The URL of the opacity texture image. Set the value the same as the <code>albedoMap</code>
 *     value to let the picture decide the transparency when <code>opacityMapMode</code> is not set: a picture whose alpha
 *     is all but 0 or 1 masks, any other blends. With an <code>opacityMapMode</code>, the <code>albedoMap</code>'s alpha is
 *     read as the mode says and <code>opacityMap</code> is not needed.
 *     <code>"hifi_pbr"</code> model only.
 * @property {string} opacityMapMode - The mode defining the interpretation of the opacity map. Values can be:
 *     <ul>
 *         <li><code>"OPACITY_MAP_OPAQUE"</code> for ignoring the opacity map information.</li>
 *         <li><code>"OPACITY_MAP_MASK"</code> for using the <code>opacityMap</code> as a mask, where only the texel greater
 *         than <code>opacityCutoff</code> are visible and rendered opaque.</li>
 *         <li><code>"OPACITY_MAP_BLEND"</code> for using the <code>opacityMap</code> for alpha blending the material surface
 *         with the background.</li>
 *         <li><code>"OPACITY_MAP_MASK_BLEND"</code> for both: the texels below <code>opacityCutoff</code> are cut out and
 *         the rest alpha blended, as Unity's Transparent surface with Alpha Clipping does.</li>
 *     </ul>
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {number|string} opacityCutoff=0.5 - The opacity cutoff threshold used to determine the opaque texels of the
 *     <code>opacityMap</code> when <code>opacityMapMode</code> is <code>"OPACITY_MAP_MASK"</code> or
 *     <code>"OPACITY_MAP_MASK_BLEND"</code>. Range <code>0.0</code> &ndash; <code>1.0</code>.
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} cullFaceMode="CULL_BACK" - The mode defining which side of the geometry should be rendered. Values can be:
 *     <ul>
 *         <li><code>"CULL_NONE"</code> to render both sides of the geometry.</li>
 *         <li><code>"CULL_FRONT"</code> to cull the front faces of the geometry.</li>
 *         <li><code>"CULL_BACK"</code> (the default) to cull the back faces of the geometry.</li>
 *     </ul>
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} doubleSidedNormals="flip" - How the back faces of a double-sided surface are lit:
 *     <code>"flip"</code> (the default, as glTF asks) turns the normal round to face the viewer;
 *     <code>"keep"</code> lights them with the normal as it is, as Unity HDRP's double-sided normal mode None does, for
 *     foliage cards whose normals point out of the canopy. Goes with <code>cullFaceMode</code>, falling through with it.
 *     A glTF material takes it from its extras (a Blender material's custom property). <code>"hifi_pbr"</code> model only.
 * @property {string} roughnessMap - The URL of the roughness texture image. You can use this or <code>glossMap</code>, but not 
 *     both. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} glossMap - The URL of the gloss texture image. You can use this or <code>roughnessMap</code>, but not 
 *     both. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} metallicMap - The URL of the metallic texture image. You can use this or <code>specularMap</code>, but 
 *     not both. With a <code>"unity"</code> packing, it holds metallic in red and smoothness in alpha. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} specularMap - The URL of the specular texture image. You can use this or <code>metallicMap</code>, but 
 *     not both. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} normalMap - The URL of the normal texture image. You can use this or <code>bumpMap</code>, but not both. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} bumpMap - The URL of the bump texture image. You can use this or <code>normalMap</code>, but not both. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} occlusionMap - The URL of the occlusion texture image. With a <code>"unity"</code> packing, it is read
 *     from green, with the other maps' texture coordinates rather than the lightmap's.
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} scatteringMap - The URL of the scattering texture image. Only used if <code>normalMap</code> or 
 *     <code>bumpMap</code> is specified. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {string} lightMap - The URL of the light map texture image. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {Mat4|string} texCoordTransform0 - The transform to use for all of the maps apart from <code>occlusionMap</code> 
 *     and <code>lightMap</code>. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {Mat4|string} texCoordTransform1 - The transform to use for <code>occlusionMap</code> and <code>lightMap</code>. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only.
 * @property {number[]|string} lightmapParams - The light map's <code>[offset, scale]</code>: light = offset + scale * the
 *     map's value. <code>"fallthrough"</code> takes the material below's.
 * @property {string} lightMapDirection - The URL of the light map's direction map: Unity's directional lightmap, the
 *     dominant light direction and its share, which lets normal maps shape the baked light.
 * @property {string} lightmapMode - What the light map holds: <code>"indirect"</code> (the default: Unity's Baked
 *     Indirect, the baked light replacing the ambient light while the sun and the local lights run as usual) or
 *     <code>"full"</code> (the whole diffuse lighting, as High Fidelity's bakes). See LIGHTMAPS.md.
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only. 
 *     <p><em>Currently not used.</em></p>
 * @property {string} materialParams - Parameters for controlling the material projection and repetition. 
 *     Set to <code>"fallthrough"</code> to fall through to the material below. <code>"hifi_pbr"</code> model only. 
 *     <p><em>Currently not used.</em></p>
 * @property {boolean} defaultFallthrough=false - <code>true</code> if all properties fall through to the material below 
 *     unless they are set, <code>false</code> if properties respect their individual fall-through settings. 
 *     <code>"hifi_pbr"</code> and <code>"hifi_shader_simple"</code> models only.
 * @property {ProceduralData} procedural - The definition of a procedural shader material.  <code>"hifi_shader_simple"</code> model only.
 */
// Note: See MaterialEntityItem.h for default values used in practice.
std::pair<std::string, std::shared_ptr<NetworkMaterial>> NetworkMaterialResource::parseJSONMaterial(
    const QJsonObject& materialJSON,
    const QUrl& baseUrl) {
    std::string name = "";
    std::shared_ptr<NetworkMaterial> networkMaterial;

    std::string modelString = graphics::Material::HIFI_PBR;
    auto modelJSONIter = materialJSON.find("model");
    if (modelJSONIter != materialJSON.end() && modelJSONIter.value().isString()) {
        modelString = modelJSONIter.value().toString().toStdString();
    }

    std::array<glm::mat4, graphics::Material::NUM_TEXCOORD_TRANSFORMS> texcoordTransforms;
    glm::vec2 lightmapParams;
    bool hasLightmapParams = false;

    const QString FALLTHROUGH("fallthrough");
    if (modelString == graphics::Material::HIFI_PBR) {
        auto material = std::make_shared<NetworkMaterial>();

        // How occlusion, roughness and metallic are packed into pictures (see "packing" above). Each packing reads
        // its own keys and leaves the others' alone, so that switching between them loses nothing.
        const QString packing = materialJSON.value("packing").toString();
        const bool unityPacking = packing == "unity";
        const bool ormPacking = packing == "orm";
        const bool customPacking = !unityPacking && !ormPacking;
        // Unity's URP/Lit reads smoothness from the metallic map's alpha, or from the albedo map's
        const bool smoothnessFromAlbedo = unityPacking && materialJSON.value("smoothnessSource").toString() == "albedoAlpha";
        // and with a metallic map, its Metallic value goes unused; an ORM map is used as it is, as in Unreal, so that
        // switching a material between the packings needs nothing else changed
        const bool unityMetallicMap = unityPacking && !materialJSON.value("metallicMap").toString().isEmpty();
        const bool ormMap = ormPacking && !materialJSON.value("ormMap").toString().isEmpty();
        bool smoothnessSet = false;

        for (auto& key : materialJSON.keys()) {
            if (key == "name") {
                auto nameJSON = materialJSON.value(key);
                if (nameJSON.isString()) {
                    name = nameJSON.toString().toStdString();
                    material->setName(name);
                }
            } else if (key == "emissive") {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() == FALLTHROUGH) {
                    material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::EMISSIVE_VAL_BIT);
                } else {
                    glm::vec3 color;
                    bool isSRGB;
                    bool valid = parseJSONColor(value, color, isSRGB);
                    if (valid) {
                        material->setEmissive(color, isSRGB);
                    }
                }
            } else if (key == "opacity") {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() == FALLTHROUGH) {
                    material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::OPACITY_VAL_BIT);
                } else if (value.isDouble()) {
                    material->setOpacity(value.toDouble());
                }
            } else if (key == "unlit") {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() == FALLTHROUGH) {
                    material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::UNLIT_VAL_BIT);
                } else if (value.isBool()) {
                    material->setUnlit(value.toBool());
                }
            } else if (key == "albedo") {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() == FALLTHROUGH) {
                    material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::ALBEDO_VAL_BIT);
                } else {
                    glm::vec3 color;
                    bool isSRGB;
                    bool valid = parseJSONColor(value, color, isSRGB);
                    if (valid) {
                        material->setAlbedo(color, isSRGB);
                    }
                }
            } else if (key == "roughness" && !unityPacking && !ormMap) {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() == FALLTHROUGH) {
                    material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::GLOSSY_VAL_BIT);
                } else if (value.isDouble()) {
                    material->setRoughness(value.toDouble());
                }
            } else if (key == "metallic" && !unityMetallicMap && !ormMap) {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() == FALLTHROUGH) {
                    material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::METALLIC_VAL_BIT);
                } else if (value.isDouble()) {
                    material->setMetallic(value.toDouble());
                }
            } else if (key == "smoothness" && unityPacking) {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() == FALLTHROUGH) {
                    material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::GLOSSY_VAL_BIT);
                    smoothnessSet = true;
                } else if (value.isDouble()) {
                    material->setRoughness(1.0f - glm::clamp((float)value.toDouble(), 0.0f, 1.0f));
                    smoothnessSet = true;
                }
            } else if (key == "occlusionStrength") {
                auto value = materialJSON.value(key);
                if (value.isDouble()) {
                    material->setOcclusionStrength(value.toDouble());
                }
            } else if (key == "opacityMapMode") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::OPACITY_MAP_MODE_BIT);
                    } else {
                        graphics::MaterialKey::OpacityMapMode mode;
                        if (graphics::MaterialKey::getOpacityMapModeFromName(valueString.toStdString(), mode)) {
                            material->setOpacityMapMode(mode);
                        }
                    }
                }
            } else if (key == "opacityCutoff") {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() == FALLTHROUGH) {
                    material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::OPACITY_CUTOFF_VAL_BIT);
                } else if (value.isDouble()) {
                    material->setOpacityCutoff(value.toDouble());
                }
            } else if (key == "cullFaceMode") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::Material::ExtraFlagBit::CULL_FACE_MODE);
                    } else {
                        graphics::MaterialKey::CullFaceMode mode;
                        if (graphics::MaterialKey::getCullFaceModeFromName(valueString.toStdString(), mode)) {
                            material->setCullFaceMode(mode);
                        }
                    }
                }
            } else if (key == "doubleSidedNormals") {
                // goes with the cull face mode, so it has no fallthrough of its own
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    material->setKeepBackFaceNormal(value.toString() == "keep");
                }
            } else if (key == "scattering") {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() == FALLTHROUGH) {
                    material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::SCATTERING_VAL_BIT);
                } else if (value.isDouble()) {
                    material->setScattering(value.toDouble());
                }
            } else if (key == "emissiveMap") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::EMISSIVE_MAP_BIT);
                    } else {
                        material->setEmissiveMap(baseUrl.resolved(valueString));
                    }
                }
            } else if (key == "albedoMap") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    QString valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::ALBEDO_MAP_BIT);
                        if (smoothnessFromAlbedo) {
                            material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::ROUGHNESS_MAP_BIT);
                        }
                    } else {
                        bool useAlphaChannel = false;
                        auto opacityMap = materialJSON.find("opacityMap");
                        if (opacityMap != materialJSON.end() && opacityMap->isString() &&
                            opacityMap->toString() == valueString) {
                            useAlphaChannel = true;
                        }
                        material->setAlbedoMap(baseUrl.resolved(valueString), useAlphaChannel);
                        if (smoothnessFromAlbedo) {
                            material->setRoughnessMap(baseUrl.resolved(valueString), true, image::ColorChannel::ALPHA);
                            material->setRoughnessFromSmoothness(true);
                            // its alpha is smoothness, not opacity (an "opacityMapMode" of the material's own still wins)
                            material->setOpacityMapMode(graphics::MaterialKey::OPACITY_MAP_OPAQUE);
                        }
                    }
                }
            } else if (key == "roughnessMap" && customPacking) {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::ROUGHNESS_MAP_BIT);
                    } else {
                        material->setRoughnessMap(baseUrl.resolved(valueString), false,
                                                  parseJSONChannel(materialJSON.value("roughnessMapChannel")));
                    }
                }
            } else if (key == "glossMap" && customPacking) {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::ROUGHNESS_MAP_BIT);
                    } else {
                        material->setRoughnessMap(baseUrl.resolved(valueString), true,
                                                  parseJSONChannel(materialJSON.value("glossMapChannel")));
                    }
                }
            } else if (key == "metallicMap" && !ormPacking) {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::METALLIC_MAP_BIT);
                        if (unityPacking && !smoothnessFromAlbedo) {
                            material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::ROUGHNESS_MAP_BIT);
                        }
                    } else if (unityPacking) {
                        // URP/Lit's Metallic Map: metallic in red, smoothness in alpha
                        material->setMetallicMap(baseUrl.resolved(valueString), false, image::ColorChannel::RED);
                        if (!smoothnessFromAlbedo) {
                            material->setRoughnessMap(baseUrl.resolved(valueString), true, image::ColorChannel::ALPHA);
                            material->setRoughnessFromSmoothness(true);
                        }
                    } else {
                        material->setMetallicMap(baseUrl.resolved(valueString), false,
                                                 parseJSONChannel(materialJSON.value("metallicMapChannel")));
                    }
                }
            } else if (key == "specularMap" && customPacking) {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::METALLIC_MAP_BIT);
                    } else {
                        material->setMetallicMap(baseUrl.resolved(valueString), true,
                                                 parseJSONChannel(materialJSON.value("specularMapChannel")));
                    }
                }
            } else if (key == "ormMap" && ormPacking) {
                // glTF's and Unreal's: occlusion, roughness and metallic in red, green and blue
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::OCCLUSION_MAP_BIT);
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::ROUGHNESS_MAP_BIT);
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::METALLIC_MAP_BIT);
                    } else {
                        QUrl url = baseUrl.resolved(valueString);
                        material->setOcclusionMap(url, image::ColorChannel::RED);
                        material->setOcclusionTexcoord0(true);
                        material->setRoughnessMap(url, false, image::ColorChannel::GREEN);
                        material->setMetallicMap(url, false, image::ColorChannel::BLUE);
                    }
                }
            } else if (key == "normalMap") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::NORMAL_MAP_BIT);
                    } else {
                        material->setNormalMap(baseUrl.resolved(valueString), false);
                    }
                }
            } else if (key == "bumpMap") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::NORMAL_MAP_BIT);
                    } else {
                        material->setNormalMap(baseUrl.resolved(valueString), true);
                    }
                }
            } else if (key == "occlusionMap" && !ormPacking) {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::OCCLUSION_MAP_BIT);
                    } else if (unityPacking) {
                        // URP/Lit's Occlusion Map: green, with the other maps' texture coordinates
                        material->setOcclusionMap(baseUrl.resolved(valueString), image::ColorChannel::GREEN);
                        material->setOcclusionTexcoord0(true);
                    } else {
                        material->setOcclusionMap(baseUrl.resolved(valueString),
                                                  parseJSONChannel(materialJSON.value("occlusionMapChannel")));
                    }
                }
            } else if (key == "scatteringMap") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::SCATTERING_MAP_BIT);
                    } else {
                        material->setScatteringMap(baseUrl.resolved(valueString));
                    }
                }
            } else if (key == "lightMap") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::LIGHT_MAP_BIT);
                    } else {
                        material->setLightMap(baseUrl.resolved(valueString));
                    }
                }
            } else if (key == "texCoordTransform0") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::Material::ExtraFlagBit::TEXCOORDTRANSFORM0);
                    }
                } else if (value.isObject()) {
                    auto valueVariant = value.toVariant();
                    glm::mat4 transform = mat4FromVariant(valueVariant);
                    texcoordTransforms[0] = transform;
                }
            } else if (key == "texCoordTransform1") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::Material::ExtraFlagBit::TEXCOORDTRANSFORM1);
                    }
                } else if (value.isObject()) {
                    auto valueVariant = value.toVariant();
                    glm::mat4 transform = mat4FromVariant(valueVariant);
                    texcoordTransforms[1] = transform;
                }
            } else if (key == "lightMapDirection") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::LIGHTMAP_DIRECTION_MAP_BIT);
                    } else {
                        material->setLightMapDirection(baseUrl.resolved(valueString));
                    }
                }
            } else if (key == "lightmapParams") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::Material::ExtraFlagBit::LIGHTMAP_PARAMS);
                    }
                } else if (value.isArray()) {
                    // [offset, scale], applied after the maps are set (setTextureMap of the light map resets them)
                    auto array = value.toArray();
                    if (array.size() == 2 && array[0].isDouble() && array[1].isDouble()) {
                        lightmapParams = glm::vec2(array[0].toDouble(), array[1].toDouble());
                        hasLightmapParams = true;
                    }
                }
            } else if (key == "lightmapMode") {
                // goes with the light map, so it has no fallthrough of its own
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    graphics::MaterialKey::LightmapMode mode;
                    if (graphics::MaterialKey::getLightmapModeFromName(value.toString().toStdString(), mode)) {
                        material->setLightmapMode(mode);
                    }
                }
            } else if (key == "materialParams") {
                auto value = materialJSON.value(key);
                if (value.isString()) {
                    auto valueString = value.toString();
                    if (valueString == FALLTHROUGH) {
                        material->setPropertyDoesFallthrough(graphics::Material::ExtraFlagBit::MATERIAL_PARAMS);
                    }
                }
                // TODO: implement materialParams and update JSDoc
            } else if (key == "defaultFallthrough") {
                auto value = materialJSON.value(key);
                if (value.isBool()) {
                    material->setDefaultFallthrough(value.toBool());
                }
            }
        }

        if (unityPacking && !smoothnessSet) {
            material->setRoughness(1.0f - UNITY_DEFAULT_SMOOTHNESS);
        }

        // Do this after the texture maps are defined, so it overrides the default transforms
        for (int i = 0; i < graphics::Material::NUM_TEXCOORD_TRANSFORMS; i++) {
            mat4 newTransform = texcoordTransforms[i];
            if (newTransform != mat4() || newTransform != material->getTexCoordTransform(i)) {
                material->setTexCoordTransform(i, newTransform);
            }
        }
        if (hasLightmapParams) {
            material->setLightmapParams(lightmapParams);
        }
        networkMaterial = material;
    } else if (modelString == graphics::Material::HIFI_SHADER_SIMPLE) {
        auto material = std::make_shared<graphics::ProceduralMaterial>();
        for (auto& key : materialJSON.keys()) {
            if (key == "name") {
                auto nameJSON = materialJSON.value(key);
                if (nameJSON.isString()) {
                    name = nameJSON.toString().toStdString();
                    material->setName(name);
                }
            } else if (key == "opacity") {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() == FALLTHROUGH) {
                    material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::OPACITY_VAL_BIT);
                } else if (value.isDouble()) {
                    material->setOpacity(value.toDouble());
                }
            } else if (key == "albedo") {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() == FALLTHROUGH) {
                    material->setPropertyDoesFallthrough(graphics::MaterialKey::FlagBit::ALBEDO_VAL_BIT);
                } else {
                    glm::vec3 color;
                    bool isSRGB;
                    bool valid = parseJSONColor(value, color, isSRGB);
                    if (valid) {
                        material->setAlbedo(color, isSRGB);
                    }
                }
            } else if (key == "defaultFallthrough") {
                auto value = materialJSON.value(key);
                if (value.isBool()) {
                    material->setDefaultFallthrough(value.toBool());
                }
            // the plain material under the shader, drawn whenever the shader can't be (Settings > Custom Shaders on
            // Models off, the world still loading, a shader this GPU doesn't compile): a colour texture, a glow map
            // (the material's emissive tints it when both are given), an emissive colour. The Lightling's lines, 10-28
            } else if (key == "albedoMap") {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() != FALLTHROUGH) {
                    material->setAlbedoMap(baseUrl.resolved(value.toString()), false);
                }
            } else if (key == "emissiveMap") {
                auto value = materialJSON.value(key);
                if (value.isString() && value.toString() != FALLTHROUGH) {
                    material->setEmissiveMap(baseUrl.resolved(value.toString()));
                }
            } else if (key == "emissive") {
                glm::vec3 color;
                bool isSRGB;
                if (parseJSONColor(materialJSON.value(key), color, isSRGB)) {
                    material->setEmissive(color, isSRGB);
                }
            } else if (key == "procedural") {
                auto value = materialJSON.value(key);
                material->setProceduralData(QJsonDocument::fromVariant(value.toVariant()).toJson());
            }
        }
        networkMaterial = material;
    }

    if (networkMaterial) {
        networkMaterial->setModel(modelString);
    }

    return std::pair<std::string, std::shared_ptr<NetworkMaterial>>(name, networkMaterial);
}

NetworkMaterialResourcePointer MaterialCache::getMaterial(const QUrl& url) {
    return ResourceCache::getResource(url).staticCast<NetworkMaterialResource>();
}

QSharedPointer<Resource> MaterialCache::createResource(const QUrl& url) {
    return QSharedPointer<Resource>(new NetworkMaterialResource(url), &Resource::deleter);
}

QSharedPointer<Resource> MaterialCache::createResourceCopy(const QSharedPointer<Resource>& resource) {
    return QSharedPointer<Resource>(new NetworkMaterialResource(*resource.staticCast<NetworkMaterialResource>()),
                                    &Resource::deleter);
}

NetworkMaterial::NetworkMaterial(const NetworkMaterial& m) :
    Material(m), _textures(m._textures), _albedoTransform(m._albedoTransform), _lightmapTransform(m._lightmapTransform),
    _lightmapParams(m._lightmapParams), _isOriginal(m._isOriginal) {
}

const QString NetworkMaterial::NO_TEXTURE = QString();

const QString& NetworkMaterial::getTextureName(MapChannel channel) {
    if (_textures[channel].texture) {
        return _textures[channel].name;
    }
    return NO_TEXTURE;
}

QUrl NetworkMaterial::getTextureUrl(const QUrl& baseUrl, const HFMTexture& texture) {
    if (texture.content.isEmpty()) {
        // External file: search relative to the baseUrl, in case filename is relative
        return baseUrl.resolved(QUrl(texture.filename));
    } else {
        // Inlined file: cache under the fbx file to avoid namespace clashes
        // NOTE: We cannot resolve the path because filename may be an absolute path
        assert(texture.filename.size() > 0);
        auto baseUrlStripped = baseUrl.toDisplayString(QUrl::RemoveFragment | QUrl::RemoveQuery | QUrl::RemoveUserInfo);
        if (texture.filename.at(0) == '/') {
            return baseUrlStripped + texture.filename;
        } else {
            return baseUrlStripped + '/' + texture.filename;
        }
    }
}

graphics::TextureMapPointer NetworkMaterial::fetchTextureMap(const QUrl& baseUrl,
                                                             const HFMTexture& hfmTexture,
                                                             image::TextureUsage::Type type,
                                                             MapChannel channel) {
    if (baseUrl.isEmpty()) {
        return nullptr;
    }

    const auto url = getTextureUrl(baseUrl, hfmTexture);
    const auto texture = DependencyManager::get<TextureCache>()->getTexture(url, type, hfmTexture.content,
                                                                            hfmTexture.maxNumPixels, hfmTexture.sourceChannel);
    _textures[channel] = Texture{ hfmTexture.name, texture };

    auto map = std::make_shared<graphics::TextureMap>();
    if (texture) {
        map->setTextureSource(texture->_textureSource);
    }
    map->setTextureTransform(hfmTexture.transform);

    return map;
}

graphics::TextureMapPointer NetworkMaterial::fetchTextureMap(const QUrl& url,
                                                             image::TextureUsage::Type type,
                                                             MapChannel channel,
                                                             image::ColorChannel sourceChannel) {
    auto textureCache = DependencyManager::get<TextureCache>();
    if (textureCache && !url.isEmpty()) {
        // a channel of a packed picture is copied into a texture of its own as it loads, as glTF's are
        auto texture = textureCache->getTexture(url, type, QByteArray(), ABSOLUTE_MAX_TEXTURE_NUM_PIXELS, sourceChannel);
        _textures[channel].texture = texture;

        auto map = std::make_shared<graphics::TextureMap>();
        if (texture) {
            map->setTextureSource(texture->_textureSource);
        }

        return map;
    }
    return nullptr;
}

void NetworkMaterial::setAlbedoMap(const QUrl& url, bool useAlphaChannel) {
    auto map = fetchTextureMap(url, image::TextureUsage::ALBEDO_TEXTURE, MapChannel::ALBEDO_MAP);
    if (map) {
        map->setUseAlphaChannel(useAlphaChannel);
        setTextureMap(MapChannel::ALBEDO_MAP, map);
    }
}

void NetworkMaterial::setNormalMap(const QUrl& url, bool isBumpmap) {
    auto map = fetchTextureMap(url, isBumpmap ? image::TextureUsage::BUMP_TEXTURE : image::TextureUsage::NORMAL_TEXTURE,
                               MapChannel::NORMAL_MAP);
    if (map) {
        setTextureMap(MapChannel::NORMAL_MAP, map);
    }
}

void NetworkMaterial::setRoughnessMap(const QUrl& url, bool isGloss, image::ColorChannel channel) {
    auto map = fetchTextureMap(url, isGloss ? image::TextureUsage::GLOSS_TEXTURE : image::TextureUsage::ROUGHNESS_TEXTURE,
                               MapChannel::ROUGHNESS_MAP, channel);
    if (map) {
        setTextureMap(MapChannel::ROUGHNESS_MAP, map);
    }
}

void NetworkMaterial::setMetallicMap(const QUrl& url, bool isSpecular, image::ColorChannel channel) {
    auto map = fetchTextureMap(url, isSpecular ? image::TextureUsage::SPECULAR_TEXTURE : image::TextureUsage::METALLIC_TEXTURE,
                               MapChannel::METALLIC_MAP, channel);
    if (map) {
        setTextureMap(MapChannel::METALLIC_MAP, map);
    }
}

void NetworkMaterial::setOcclusionMap(const QUrl& url, image::ColorChannel channel) {
    auto map = fetchTextureMap(url, image::TextureUsage::OCCLUSION_TEXTURE, MapChannel::OCCLUSION_MAP, channel);
    if (map) {
        setTextureMap(MapChannel::OCCLUSION_MAP, map);
    }
}

void NetworkMaterial::setEmissiveMap(const QUrl& url) {
    auto map = fetchTextureMap(url, image::TextureUsage::EMISSIVE_TEXTURE, MapChannel::EMISSIVE_MAP);
    if (map) {
        setTextureMap(MapChannel::EMISSIVE_MAP, map);
    }
}

void NetworkMaterial::setScatteringMap(const QUrl& url) {
    auto map = fetchTextureMap(url, image::TextureUsage::SCATTERING_TEXTURE, MapChannel::SCATTERING_MAP);
    if (map) {
        setTextureMap(MapChannel::SCATTERING_MAP, map);
    }
}

void NetworkMaterial::setLightMap(const QUrl& url) {
    auto map = fetchTextureMap(url, image::TextureUsage::LIGHTMAP_TEXTURE, MapChannel::LIGHT_MAP);
    if (map) {
        //map->setTextureTransform(_lightmapTransform);
        //map->setLightmapOffsetScale(_lightmapParams.x, _lightmapParams.y);
        setTextureMap(MapChannel::LIGHT_MAP, map);
    }
}

void NetworkMaterial::setLightMapDirection(const QUrl& url) {
    auto map = fetchTextureMap(url, image::TextureUsage::LIGHTMAP_DIRECTION_TEXTURE, MapChannel::LIGHTMAP_DIRECTION_MAP);
    if (map) {
        setTextureMap(MapChannel::LIGHTMAP_DIRECTION_MAP, map);
    }
}

NetworkMaterial::NetworkMaterial(const HFMMaterial& material, const QUrl& textureBaseUrl) :
    graphics::Material(*material._material) {
    _name = material.name.toStdString();
    if (!material.albedoTexture.filename.isEmpty()) {
        auto map = fetchTextureMap(textureBaseUrl, material.albedoTexture, image::TextureUsage::ALBEDO_TEXTURE,
                                   MapChannel::ALBEDO_MAP);
        if (map) {
            _albedoTransform = material.albedoTexture.transform;
            map->setTextureTransform(_albedoTransform);

            if (!material.opacityTexture.filename.isEmpty()) {
                if (material.albedoTexture.filename == material.opacityTexture.filename) {
                    // Best case scenario, just indicating that the albedo map contains transparency
                    // TODO: Different albedo/opacity maps are not currently supported
                    map->setUseAlphaChannel(true);
                }
            }
        }

        setTextureMap(MapChannel::ALBEDO_MAP, map);
    }

    if (!material.normalTexture.filename.isEmpty()) {
        auto type =
            (material.normalTexture.isBumpmap ? image::TextureUsage::BUMP_TEXTURE : image::TextureUsage::NORMAL_TEXTURE);
        auto map = fetchTextureMap(textureBaseUrl, material.normalTexture, type, MapChannel::NORMAL_MAP);
        setTextureMap(MapChannel::NORMAL_MAP, map);
    }

    if (!material.roughnessTexture.filename.isEmpty()) {
        auto map = fetchTextureMap(textureBaseUrl, material.roughnessTexture, image::TextureUsage::ROUGHNESS_TEXTURE,
                                   MapChannel::ROUGHNESS_MAP);
        setTextureMap(MapChannel::ROUGHNESS_MAP, map);
    } else if (!material.glossTexture.filename.isEmpty()) {
        auto map = fetchTextureMap(textureBaseUrl, material.glossTexture, image::TextureUsage::GLOSS_TEXTURE,
                                   MapChannel::ROUGHNESS_MAP);
        setTextureMap(MapChannel::ROUGHNESS_MAP, map);
    }

    if (!material.metallicTexture.filename.isEmpty()) {
        auto map = fetchTextureMap(textureBaseUrl, material.metallicTexture, image::TextureUsage::METALLIC_TEXTURE,
                                   MapChannel::METALLIC_MAP);
        setTextureMap(MapChannel::METALLIC_MAP, map);
    } else if (!material.specularTexture.filename.isEmpty()) {
        auto map = fetchTextureMap(textureBaseUrl, material.specularTexture, image::TextureUsage::SPECULAR_TEXTURE,
                                   MapChannel::METALLIC_MAP);
        setTextureMap(MapChannel::METALLIC_MAP, map);
    }

    if (!material.occlusionTexture.filename.isEmpty()) {
        auto map = fetchTextureMap(textureBaseUrl, material.occlusionTexture, image::TextureUsage::OCCLUSION_TEXTURE,
                                   MapChannel::OCCLUSION_MAP);
        if (map) {
            map->setTextureTransform(material.occlusionTexture.transform);
        }
        setTextureMap(MapChannel::OCCLUSION_MAP, map);
    }

    if (!material.emissiveTexture.filename.isEmpty()) {
        auto map = fetchTextureMap(textureBaseUrl, material.emissiveTexture, image::TextureUsage::EMISSIVE_TEXTURE,
                                   MapChannel::EMISSIVE_MAP);
        setTextureMap(MapChannel::EMISSIVE_MAP, map);
    }

    if (!material.scatteringTexture.filename.isEmpty()) {
        auto map = fetchTextureMap(textureBaseUrl, material.scatteringTexture, image::TextureUsage::SCATTERING_TEXTURE,
                                   MapChannel::SCATTERING_MAP);
        setTextureMap(MapChannel::SCATTERING_MAP, map);
    }

    if (!material.lightmapTexture.filename.isEmpty()) {
        auto map = fetchTextureMap(textureBaseUrl, material.lightmapTexture, image::TextureUsage::LIGHTMAP_TEXTURE,
                                   MapChannel::LIGHT_MAP);
        if (map) {
            _lightmapTransform = material.lightmapTexture.transform;
            _lightmapParams = material.lightmapParams;
            map->setTextureTransform(_lightmapTransform);
            map->setLightmapOffsetScale(_lightmapParams.x, _lightmapParams.y);
        }
        setTextureMap(MapChannel::LIGHT_MAP, map);
        setLightmapMode(material.lightmapMode);
    }

    if (!material.lightmapDirectionTexture.filename.isEmpty()) {
        auto map = fetchTextureMap(textureBaseUrl, material.lightmapDirectionTexture, image::TextureUsage::LIGHTMAP_DIRECTION_TEXTURE,
                                   MapChannel::LIGHTMAP_DIRECTION_MAP);
        setTextureMap(MapChannel::LIGHTMAP_DIRECTION_MAP, map);
    }
}

void NetworkMaterial::setTextures(const QVariantMap& textureMap) {
    _isOriginal = false;

    const auto& albedoName = getTextureName(MapChannel::ALBEDO_MAP);
    const auto& normalName = getTextureName(MapChannel::NORMAL_MAP);
    const auto& roughnessName = getTextureName(MapChannel::ROUGHNESS_MAP);
    const auto& metallicName = getTextureName(MapChannel::METALLIC_MAP);
    const auto& occlusionName = getTextureName(MapChannel::OCCLUSION_MAP);
    const auto& emissiveName = getTextureName(MapChannel::EMISSIVE_MAP);
    const auto& lightmapName = getTextureName(MapChannel::LIGHT_MAP);
    const auto& scatteringName = getTextureName(MapChannel::SCATTERING_MAP);

    if (!albedoName.isEmpty()) {
        auto url = textureMap.contains(albedoName) ? textureMap[albedoName].toUrl() : QUrl();
        auto map = fetchTextureMap(url, image::TextureUsage::ALBEDO_TEXTURE, MapChannel::ALBEDO_MAP);
        if (map) {
            map->setTextureTransform(_albedoTransform);
            // when reassigning the albedo texture we also check for the alpha channel used as opacity
            map->setUseAlphaChannel(true);
        }
        setTextureMap(MapChannel::ALBEDO_MAP, map);
    }

    if (!normalName.isEmpty()) {
        auto url = textureMap.contains(normalName) ? textureMap[normalName].toUrl() : QUrl();
        auto map = fetchTextureMap(url, image::TextureUsage::NORMAL_TEXTURE, MapChannel::NORMAL_MAP);
        setTextureMap(MapChannel::NORMAL_MAP, map);
    }

    if (!roughnessName.isEmpty()) {
        auto url = textureMap.contains(roughnessName) ? textureMap[roughnessName].toUrl() : QUrl();
        // FIXME: If passing a gloss map instead of a roughmap how do we know?
        auto map = fetchTextureMap(url, image::TextureUsage::ROUGHNESS_TEXTURE, MapChannel::ROUGHNESS_MAP);
        setTextureMap(MapChannel::ROUGHNESS_MAP, map);
    }

    if (!metallicName.isEmpty()) {
        auto url = textureMap.contains(metallicName) ? textureMap[metallicName].toUrl() : QUrl();
        // FIXME: If passing a specular map instead of a metallic how do we know?
        auto map = fetchTextureMap(url, image::TextureUsage::METALLIC_TEXTURE, MapChannel::METALLIC_MAP);
        setTextureMap(MapChannel::METALLIC_MAP, map);
    }

    if (!occlusionName.isEmpty()) {
        auto url = textureMap.contains(occlusionName) ? textureMap[occlusionName].toUrl() : QUrl();
        // FIXME: we need to handle the occlusion map transform here
        auto map = fetchTextureMap(url, image::TextureUsage::OCCLUSION_TEXTURE, MapChannel::OCCLUSION_MAP);
        setTextureMap(MapChannel::OCCLUSION_MAP, map);
    }

    if (!emissiveName.isEmpty()) {
        auto url = textureMap.contains(emissiveName) ? textureMap[emissiveName].toUrl() : QUrl();
        auto map = fetchTextureMap(url, image::TextureUsage::EMISSIVE_TEXTURE, MapChannel::EMISSIVE_MAP);
        setTextureMap(MapChannel::EMISSIVE_MAP, map);
    }

    if (!scatteringName.isEmpty()) {
        auto url = textureMap.contains(scatteringName) ? textureMap[scatteringName].toUrl() : QUrl();
        auto map = fetchTextureMap(url, image::TextureUsage::SCATTERING_TEXTURE, MapChannel::SCATTERING_MAP);
        setTextureMap(MapChannel::SCATTERING_MAP, map);
    }

    if (!lightmapName.isEmpty()) {
        auto url = textureMap.contains(lightmapName) ? textureMap[lightmapName].toUrl() : QUrl();
        auto map = fetchTextureMap(url, image::TextureUsage::LIGHTMAP_TEXTURE, MapChannel::LIGHT_MAP);
        if (map) {
            map->setTextureTransform(_lightmapTransform);
            map->setLightmapOffsetScale(_lightmapParams.x, _lightmapParams.y);
        }
        setTextureMap(MapChannel::LIGHT_MAP, map);
    }
}

bool NetworkMaterial::isMissingTexture() {
    for (auto& networkTexture : _textures) {
        auto& texture = networkTexture.second.texture;
        if (!texture) {
            continue;
        }
        // Failed texture downloads need to be considered as 'loaded'
        // or the object will never fade in
        bool finished =
            texture->isFailed() || (texture->isLoaded() && texture->getGPUTexture() && texture->getGPUTexture()->isDefined());
        if (!finished) {
            return true;
        }
    }
    return false;
}

bool NetworkMaterial::checkResetOpacityMap() {
    // If material textures are loaded, check the material translucency
    // FIXME: This should not be done here.  The opacity map should already be reset in Material::setTextureMap.
    // However, currently that code can be called before the albedo map is defined, so resetOpacityMap will fail.
    // Geometry::areTexturesLoaded() is called repeatedly until it returns true, so we do the check here for now
    const auto& albedoTexture = _textures[NetworkMaterial::MapChannel::ALBEDO_MAP];
    if (albedoTexture.texture) {
        return resetOpacityMap();
    }
    return false;
}
