//
//  AssimpSerializer.cpp
//  libraries/fbx/src
//
//  Created by Maki Deprez on 12/20/20.
//  Copyright 2020 Tivoli Cloud VR, Inc.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "AssimpSerializer.h"
#include "FSTReader.h"

#include <algorithm>

#include <BlendshapeConstants.h>
#include <ResourceManager.h>
#include <PathUtils.h>
#include <hfm/ModelFormatLogging.h>

#include <assimp/Importer.hpp>
#include <assimp/LogStream.hpp>
#include <assimp/DefaultLogger.hpp>
#include <assimp/postprocess.h>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/pbrmaterial.h>

#include <glm/gtx/transform.hpp>

#include <QtGui/QImage>
#include <QtCore/QBuffer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtCore/QtEndian>

// disabled because it causes crashes with Assimp::DefaultLogger::WriteToStreams
// #define ENABLE_ASSIMP_LOGGING

MediaType AssimpSerializer::getMediaType() const {
    MediaType mediaType;

    Assimp::Importer importer;
    aiString aiExts;
    importer.GetExtensionList(aiExts);

    auto exts = QString(aiExts.C_Str()).split(";");
    for (QString anyExt : exts) {
        QString ext = anyExt.replace("*.", "");
        if (ext == "fbx" || ext == "obj") continue;

        // qDebug(modelformat) << "Available format:" << ext; 
        mediaType.extensions.push_back(ext.toStdString());
        // mediaType.webMediaTypes.push_back("");
    }

    return mediaType;
}

std::unique_ptr<hfm::Serializer::Factory> AssimpSerializer::getFactory() const {
    return std::make_unique<hfm::Serializer::SimpleFactory<AssimpSerializer>>();
}

class TivoliIOStream : public Assimp::IOStream {
    friend class TivoliIOSystem;

protected:
    hifi::URL url;
    
    TivoliIOStream(hifi::URL url) : url(url) {}

public:
    ~TivoliIOStream() {}
    size_t Read(void* pvBuffer, size_t pSize, size_t pCount) override {
        // TODO: i think this function runs twice for some reason
        auto request = DependencyManager::get<ResourceManager>()->createResourceRequest(
            nullptr, url, true, -1, "TivoliIOSystem::Exists"
        );
        if (!request)  return 0;

        QEventLoop loop;
        QObject::connect(request, &ResourceRequest::finished, &loop, &QEventLoop::quit);
        request->send();
        loop.exec();

        if (request->getResult() == ResourceRequest::Success) {
            auto data = request->getData();
            memcpy(pvBuffer, data.data(), data.size());
            // return data.size();
            return pCount;
        } else {
            return 0;
        }
    }
    size_t Write(const void* pvBuffer, size_t pSize, size_t pCount) override {
        return 0;
    }
    aiReturn Seek(size_t pOffset, aiOrigin pOrigin) override {
        return aiReturn_FAILURE;
    }
    size_t Tell() const override {
        return 0;
    }
    size_t FileSize() const override {
        return 0;
    }
    void Flush () override {
    }
};

class TivoliIOSystem : public Assimp::IOSystem {
public:
    hifi::URL originalUrl;

    TivoliIOSystem(hifi::URL originalUrl) : originalUrl(originalUrl) {}
    ~TivoliIOSystem() {}

    bool Exists(const char* pFile) const override {
        return true;
    }
    char getOsSeparator() const override {
        return '/';
    }
    Assimp::IOStream* Open(const char* pFile, const char* pMode) override {
        return new TivoliIOStream(originalUrl.resolved(QString::fromLocal8Bit(pFile)));
    }
    void Close(Assimp::IOStream* pFile) override {
        delete pFile;
    }
};

bool isEmptyVec3(glm::vec3 v) { return v.x == 0 && v.y == 0 && v.z == 0; }

glm::vec3 asVec3(aiColor3D c) { return glm::vec3(c.r, c.g, c.b); }
glm::vec3 asVec3(aiVector3D v) { return glm::vec3(v.x, v.y, v.z); }
glm::quat asQuat(aiQuaternion q) { return glm::quat(q.w, q.x, q.y, q.z); }
glm::mat4 asMat4(aiMatrix4x4 from) {
    glm::mat4 to;
    to[0][0] = from.a1; to[0][1] = from.b1; to[0][2] = from.c1; to[0][3] = from.d1;
    to[1][0] = from.a2; to[1][1] = from.b2; to[1][2] = from.c2; to[1][3] = from.d2;
    to[2][0] = from.a3; to[2][1] = from.b3; to[2][2] = from.c3; to[2][3] = from.d3;
    to[3][0] = from.a4; to[3][1] = from.b4; to[3][2] = from.c4; to[3][3] = from.d4;
    return to;
}

HFMTexture AssimpSerializer::getHfmTexture(aiString aiPath) {
    QString path = aiPath.C_Str();

    if (hfmTextures.contains(path)) {
        return hfmTextures[path];
    } else {
        HFMTexture texture = HFMTexture();

        QRegExp indexFinder = QRegExp("^\\*([0-9]+)$");
        if (indexFinder.exactMatch(path)) {
            auto index = indexFinder.cap(1).toUInt();

            if (index < scene->mNumTextures) {
                auto aiTexture = scene->mTextures[index];
                auto width = aiTexture->mWidth;
                auto height = aiTexture->mHeight;

                texture.name = aiTexture->mFilename.C_Str();
                // texture.filename = url.toEncoded().append(index);
                texture.filename = QString::number(index).toLocal8Bit();

                if (height == 0) {
                    texture.content = QByteArray((char*)aiTexture->pcData, width);
                } else {
                    QImage image = QImage(width, height, QImage::Format::Format_RGBA8888);
                    auto aiTexel = aiTexture->pcData;
                    
                    for (size_t i = 0; i < width * height; i++) {
                        auto pixel = aiTexel[i];
                        image.setPixelColor(i % width, i / width, QColor(
                            pixel.r, pixel.g, pixel.b, pixel.a
                        ));
                    }

                    QByteArray imageData;
                    QBuffer imageBuffer(&imageData);
                    imageBuffer.open(QIODevice::WriteOnly);
                    image.save(&imageBuffer, "PNG");
                    imageBuffer.close();
                    texture.content = imageData;                    
                }
            }
        } else {
            QUrl textureUrl = url.resolved(path);
            texture.name = textureUrl.fileName();
            texture.filename = textureUrl.toEncoded();
        }

        hfmTextures[path] = texture;
        return texture;
    }
}

void AssimpSerializer::processMaterials() {
    for(size_t materialIndex = 0; materialIndex < scene->mNumMaterials; materialIndex++) {
        aiMaterial* mMaterial = scene->mMaterials[materialIndex];

        // for(size_t propIndex = 0; propIndex < mMaterial->mNumProperties; propIndex++) {
        //     auto prop = mMaterial->mProperties[propIndex];
        //     qCDebug(modelformat)<<materialIndex<<prop->mKey.C_Str();
        // }

        hfmModel->materials.emplace_back();
        hfm::Material* materialPtr = &hfmModel->materials.back();

        materialPtr->name = materialPtr->materialID = mMaterial->GetName().C_Str();
        materialPtr->_material = std::make_shared<graphics::Material>();
        auto modelMaterial = materialPtr->_material;

        bool isGltf = ext == "gltf" || ext == "glb";
        if (isGltf) {
            materialPtr->isPBSMaterial = true;
        }

        // unlit colors arent srgb

        bool unlit;
        if (mMaterial->Get(AI_MATKEY_GLTF_UNLIT, unlit) == AI_SUCCESS && unlit) {
            modelMaterial->setUnlit(true);
        }

        // colors

        // glTF's colour factors are linear by its specification (UnityGLTF, for one, writes the linear value of the
        // material's colour), the other formats' colours sRGB, and an unlit colour is never converted. Converting a
        // glTF factor as sRGB darkened every tinted material by the factor's own sRGB-to-linear ratio: Rainbow_Room's
        // floor, factor 0.67, rendered at 0.67 x Unity's brightness, its walls at 0.85 x (LIGHTMAPS.md, the tone pass)
        bool colorsAreSRGB = !unlit && !isGltf;

        // whether the factor's alpha means anything: glTF's OPAQUE (the default) ignores it by the specification and
        // MASK cuts by the texture's alpha against the cutoff; only BLEND blends by it. The engine took any alpha
        // below 1 as translucency, so Unity's opaque rainbow floor (alpha 0.8) blended over the floor beneath it.
        bool alphaCounts = true;
        if (isGltf) {
            aiString gltfAlphaMode;
            alphaCounts = mMaterial->Get(AI_MATKEY_GLTF_ALPHAMODE, gltfAlphaMode) == AI_SUCCESS &&
                          QString(gltfAlphaMode.C_Str()) == "BLEND";
        }

        aiColor3D diffuse;
        if (mMaterial->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS) {
            modelMaterial->setAlbedo(asVec3(diffuse), colorsAreSRGB);
        }

        float opacity;
        if (alphaCounts && mMaterial->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
            modelMaterial->setOpacity(opacity);
        }

        aiColor3D emissive;
        if (mMaterial->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
            modelMaterial->setEmissive(asVec3(emissive), colorsAreSRGB);
        }

        aiColor4D baseColor;
        if (mMaterial->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_BASE_COLOR_FACTOR, baseColor) == AI_SUCCESS) {
            modelMaterial->setAlbedo(glm::vec3(baseColor.r, baseColor.g, baseColor.b), colorsAreSRGB);
            if (alphaCounts) {
                modelMaterial->setOpacity(baseColor.a);
            }
        }

        float roughnessFactor;
        if (mMaterial->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_ROUGHNESS_FACTOR, roughnessFactor) == AI_SUCCESS) {
            materialPtr->roughness = roughnessFactor;
            modelMaterial->setRoughness(roughnessFactor);
        }

        float metallicFactor;
        if (mMaterial->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLIC_FACTOR, metallicFactor) == AI_SUCCESS) {
            materialPtr->metallic = metallicFactor;
            modelMaterial->setMetallic(metallicFactor);
        }

        // TODO: scattering!

        // textures

        aiString diffuseTexture;
        if (mMaterial->GetTexture(aiTextureType_DIFFUSE, 0, &diffuseTexture) == AI_SUCCESS) {
            materialPtr->albedoTexture = getHfmTexture(diffuseTexture);
            materialPtr->opacityTexture = getHfmTexture(diffuseTexture);
            materialPtr->useAlbedoMap = true;
        }

        aiString emissiveTexture;
        if (mMaterial->GetTexture(aiTextureType_EMISSIVE, 0, &emissiveTexture) == AI_SUCCESS) {
            materialPtr->emissiveTexture = getHfmTexture(emissiveTexture);
            materialPtr->useEmissiveMap = true;
        }

        aiString occlusionTexture;
        if (mMaterial->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &occlusionTexture) == AI_SUCCESS) {
            materialPtr->occlusionTexture = getHfmTexture(occlusionTexture);
            materialPtr->useOcclusionMap = true;
        }

        // assimp maps occlusion as lightmap for gltf
        aiString lightmapTexture;
        if (mMaterial->GetTexture(aiTextureType_LIGHTMAP, 0, &lightmapTexture) == AI_SUCCESS) {
            // materialPtr->lightmapTexture = getHfmTexture(lightmapTexture); // probably should be avoided anyway as it breaks pbr
            materialPtr->occlusionTexture = getHfmTexture(lightmapTexture);
            materialPtr->useOcclusionMap = true;
            if (isGltf) {
                materialPtr->occlusionTexture.sourceChannel = image::ColorChannel::RED;
            }
        }

        aiString normalTexture;
        if (mMaterial->GetTexture(aiTextureType_NORMALS, 0, &normalTexture) == AI_SUCCESS) {
            materialPtr->normalTexture = getHfmTexture(normalTexture);
            materialPtr->useNormalMap = true;
        }

        aiString metallicTexture;
        if (mMaterial->GetTexture(aiTextureType_METALNESS, 0, &metallicTexture) == AI_SUCCESS) {
            materialPtr->metallicTexture = getHfmTexture(metallicTexture);
            materialPtr->useMetallicMap = true;
        }
        
        aiString metallicRoughnessTexture;
        if (mMaterial->GetTexture(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE, &metallicRoughnessTexture) == AI_SUCCESS) {          
            materialPtr->roughnessTexture = getHfmTexture(metallicRoughnessTexture);
            materialPtr->roughnessTexture.sourceChannel = image::ColorChannel::GREEN;
            materialPtr->useRoughnessMap = true;

            materialPtr->metallicTexture = getHfmTexture(metallicRoughnessTexture);
            materialPtr->metallicTexture.sourceChannel = image::ColorChannel::BLUE;
            materialPtr->useMetallicMap = true;
        }

        // extras
        
        int cullNone;
        if (mMaterial->Get(AI_MATKEY_TWOSIDED, cullNone) == AI_SUCCESS && cullNone) {
            modelMaterial->setCullFaceMode(graphics::MaterialKey::CullFaceMode::CULL_NONE);
        }

        aiString alphaMode;
        if (mMaterial->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS) {
            QString alphaModeStr = alphaMode.C_Str();
            modelMaterial->setOpacityMapMode(
                alphaModeStr == "BLEND" ?
                    graphics::MaterialKey::OPACITY_MAP_BLEND :
                alphaModeStr == "MASK" ?
                    graphics::MaterialKey::OPACITY_MAP_MASK :
                graphics::MaterialKey::OPACITY_MAP_OPAQUE
            );
        } else {
            modelMaterial->setOpacityMapMode(graphics::MaterialKey::OPACITY_MAP_OPAQUE);
        }

    }
}

aiAnimMesh* AssimpSerializer::getBlendshapeByName(QString name, aiMesh* mMesh) {
    for (size_t i = 0; i < mMesh->mNumAnimMeshes; i++) {
        auto animMesh = mMesh->mAnimMeshes[i];
        if (QString(animMesh->mName.C_Str()) == name) {
            return animMesh;
        }
    }
    return nullptr;
}

void AssimpSerializer::processMeshes(const hifi::VariantHash& mapping) {
    // The model's own blend shapes (hfm::Model::ownBlendshapeNames), when the .fst asks for them, as FBXSerializer keeps
    // them: each of a mesh's shapes that no standard name takes, by its name, in a slot after the standard ones
    const hifi::VariantHash ownBlendshapeDefaults = mapping.value("blendshapeDefault").toHash();
    const bool keepOwnBlendshapes = !ownBlendshapeDefaults.isEmpty() ||
        mapping.value("ownBlendshapes").toString().trimmed().compare("true", Qt::CaseInsensitive) == 0;
    QHash<QString, int> ownBlendshapeSlots;

    // a shape's offsets from the mesh, times its weight, for the vertices it moves
    auto fillBlendshape = [](hfm::Blendshape& blendshape, const aiMesh* mMesh, const aiAnimMesh* animMesh, float weight) {
        for (size_t vertexIndex = 0; vertexIndex < animMesh->mNumVertices; vertexIndex++) {
            auto v = (asVec3(animMesh->mVertices[vertexIndex]) - asVec3(mMesh->mVertices[vertexIndex])) * weight;

            auto n = animMesh->HasNormals() ?
                (asVec3(animMesh->mNormals[vertexIndex]) - asVec3(mMesh->mNormals[vertexIndex])) * weight :
                vec3(0.0f);

            auto t = animMesh->HasTangentsAndBitangents() ?
                (asVec3(animMesh->mTangents[vertexIndex]) - asVec3(mMesh->mTangents[vertexIndex])) * weight :
                vec3(0.0f);

            if (isEmptyVec3(v) && isEmptyVec3(n) && isEmptyVec3(t)) {
                continue;
            }

            blendshape.indices.push_back(vertexIndex);

            blendshape.vertices.push_back(v);
            if (animMesh->HasNormals()) blendshape.normals.push_back(n);
            if (animMesh->HasTangentsAndBitangents()) blendshape.tangents.push_back(t);
        }
    };

    for (size_t meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++) {
        aiMesh* mMesh = scene->mMeshes[meshIndex];

        hfmModel->meshes.emplace_back();
        hfm::Mesh* meshPtr = &hfmModel->meshes.back();

        meshPtr->parts.emplace_back();
        hfm::MeshPart* meshPartPtr = &meshPtr->parts.back();

        meshPtr->meshIndex = meshIndex;

        for (size_t vertexIndex = 0; vertexIndex < mMesh->mNumVertices; vertexIndex++) {
            auto v = mMesh->mVertices[vertexIndex];
            meshPtr->vertices.push_back(asVec3(v));

            if (mMesh->HasNormals()) {
                auto n = mMesh->mNormals[vertexIndex];
                meshPtr->normals.push_back(asVec3(n));
            }

            if (mMesh->HasTangentsAndBitangents()) {
                auto t = mMesh->mTangents[vertexIndex];
                meshPtr->tangents.push_back(asVec3(t));
            }

            if (mMesh->HasVertexColors(0)) {
                auto c = mMesh->mColors[0][vertexIndex];
                meshPtr->colors.push_back(glm::vec3(c.r, c.g, c.b));
            }

            for (size_t uvI = 0; uvI <= 1; uvI++) {
                if (!mMesh->HasTextureCoords(uvI)) break;
                auto uv = mMesh->mTextureCoords[uvI][vertexIndex];
                if (uvI == 0) {
                    meshPtr->texCoords.push_back(glm::vec2(uv.x, uv.y));
                } else {
                    meshPtr->texCoords1.push_back(glm::vec2(uv.x, uv.y));
                }
            }
        }

        for (size_t faceIndex = 0; faceIndex < mMesh->mNumFaces; faceIndex++) {
            aiFace face = mMesh->mFaces[faceIndex];
            if (face.mNumIndices != 3) continue; // must be a triangle
            for (size_t i = 0; i < face.mNumIndices; i++) {
                meshPartPtr->triangleIndices.push_back(face.mIndices[i]);
            }
        }

        hifi::VariantHash blendshapeMappings = FSTReader::getBlendshapeMappings(mapping);

        if (mMesh->mNumAnimMeshes > 0) {
            bool isBlendshapeChannelNamesEmpty = hfmModel->blendshapeChannelNames.size() == 0;
            QSet<QString> standardShapes;   // this mesh's shapes the standard names take

            for (size_t i = 0; i < (int)Blendshapes::BlendshapeCount; i++) {
                const QString fromName = BLENDSHAPE_NAMES[i];
                
                if (isBlendshapeChannelNamesEmpty) {
                    hfmModel->blendshapeChannelNames.push_back(fromName);
                }

                meshPtr->blendshapes.push_back(hfm::Blendshape());
                hfm::Blendshape* blendshapePtr = &meshPtr->blendshapes.back();

                QString toName;
                float weight = 1.0f;

                if (blendshapeMappings.contains(fromName)) {
                    auto mapping = blendshapeMappings[fromName].toList();
                    if (mapping.count() >= 2) {
                        toName = mapping.at(0).toString();
                        weight = mapping.at(1).toFloat();
                    }
                } else {
                    toName = fromName;
                }

                if (weight == 0.0f && fromName == toName) {
                    continue;
                }

                aiAnimMesh* animMesh = getBlendshapeByName(toName, mMesh);
                if (animMesh == nullptr) {
                    continue;
                }
                standardShapes.insert(toName);
                fillBlendshape(*blendshapePtr, mMesh, animMesh, weight);
            }

            if (keepOwnBlendshapes) {
                for (size_t i = 0; i < mMesh->mNumAnimMeshes; i++) {
                    const aiAnimMesh* animMesh = mMesh->mAnimMeshes[i];
                    const QString name = animMesh->mName.C_Str();
                    if (name.isEmpty() || standardShapes.contains(name)) {
                        continue;
                    }
                    auto slot = ownBlendshapeSlots.find(name);
                    if (slot == ownBlendshapeSlots.end()) {
                        slot = ownBlendshapeSlots.insert(name, hfmModel->ownBlendshapeNames.size());
                        hfmModel->ownBlendshapeNames << name;
                    }
                    const int index = (int)Blendshapes::BlendshapeCount + *slot;
                    if (meshPtr->blendshapes.size() <= index) {
                        meshPtr->blendshapes.resize(index + 1);
                    }
                    fillBlendshape(meshPtr->blendshapes[index], mMesh, animMesh, 1.0f);
                }
            }
        }
    }

    // the own blend shapes' starting weights
    hfmModel->ownBlendshapeDefaults.fill(0.0f, hfmModel->ownBlendshapeNames.size());
    for (auto it = ownBlendshapeDefaults.begin(); it != ownBlendshapeDefaults.end(); ++it) {
        const int slot = hfmModel->ownBlendshapeNames.indexOf(it.key());
        if (slot >= 0) {
            hfmModel->ownBlendshapeDefaults[slot] = glm::clamp(it.value().toFloat(), 0.0f, 1.0f);
        }
    }
}

QList<hfm::Shape*> AssimpSerializer::getHfmShapesByMeshIndex(uint32_t meshIndex) {
    QList<hfm::Shape*> shapes;
    for (size_t i = 0; i < hfmModel->shapes.size(); i++) {
        auto shape = &hfmModel->shapes.at(i);
        if (shape->mesh == meshIndex) {
            shapes.push_back(shape);
        }
    }
    return shapes;
}

void AssimpSerializer::processBones() {
    bool hasBones = false;
    for (size_t meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++) {
        if (scene->mMeshes[meshIndex]->mNumBones > 0) {
            hasBones = true;
            break;
        }
    };
    if (!hasBones) return;

    QHash<size_t, Extents> skinnedMeshExtents; // by mesh index, at the bind pose

    for (size_t meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++) {
        aiMesh* mMesh = scene->mMeshes[meshIndex];
        if (mMesh->mNumBones == 0) continue;

        hfm::Mesh* meshPtr = &hfmModel->meshes.at(meshIndex);
        meshPtr->clusterWeightsPerVertex = 4;
        hfmModel->hasSkeletonJoints = true;

        // A skin for each mesh, its clusters numbered from 0: the skinning shader takes 128 clusters a mesh
        // (Skinning.slh), and one skin for the whole model gave the later meshes of a VRM numbers past that, which drew
        // its hair and skirt as spikes
        uint32_t skinDeformerIndex = hfmModel->skinDeformers.size();
        hfmModel->skinDeformers.emplace_back();
        hfm::SkinDeformer* skinDeformer = &hfmModel->skinDeformers.back();
        if (mMesh->mNumBones > 128) {
            qCWarning(modelformat) << "AssimpSerializer: mesh" << mMesh->mName.C_Str() << "has" << mMesh->mNumBones
                                   << "bones; the skinning shader takes 128";
        }

        QList<hfm::Shape*> shapes = getHfmShapesByMeshIndex(meshIndex);
        for (auto shape : shapes) {
            shape->skinDeformer = skinDeformerIndex;
        }

        QHash<size_t, // vertex index
            QList<std::pair<
                size_t, // cluster index (to joint)
                float // weight
            >>
        > clusterMap;

        for (size_t boneIndex = 0; boneIndex < mMesh->mNumBones; boneIndex++) {
            aiBone* bone = mMesh->mBones[boneIndex];

            int nodeIndex = hfmModel->jointIndices[bone->mName.C_Str()];
            hfm::Joint* nodePtr = &hfmModel->joints[nodeIndex];
            if (nodePtr == nullptr) continue;

            glm::mat4 inverseBindMatrix = asMat4(bone->mOffsetMatrix);

            glm::vec3 bindTranslation = extractTranslation(hfmModel->offset * inverseBindMatrix);
            hfmModel->bindExtents.addPoint(bindTranslation);

            nodePtr->isSkeletonJoint = true;
            nodePtr->bindTransform = inverseBindMatrix;
            nodePtr->bindTransformFoundInCluster = true;
            nodePtr->inverseBindRotation = glmExtractRotation(inverseBindMatrix);
            nodePtr->inverseDefaultRotation = glm::inverse(nodePtr->rotation);

            size_t clusterIndex = skinDeformer->clusters.size();
            skinDeformer->clusters.emplace_back();
            hfm::Cluster* cluster = &skinDeformer->clusters.back();     

            cluster->jointIndex = nodeIndex;
            cluster->inverseBindMatrix = inverseBindMatrix;
            cluster->inverseBindTransform = Transform(inverseBindMatrix);

            for (size_t weightIndex = 0; weightIndex < bone->mNumWeights; weightIndex++) {
                aiVertexWeight weight = bone->mWeights[weightIndex];
                clusterMap[weight.mVertexId].push_back(
                    std::pair<size_t, float>(clusterIndex, weight.mWeight)
                );
            }
        }

        // apply mesh cluster indices (to joints) and weights

        for (size_t vertexIndex = 0; vertexIndex < mMesh->mNumVertices; vertexIndex++) {
            auto cluster = clusterMap[vertexIndex];
            // the four strongest, summing to one: some exporters' weights don't (older VRoid's give an eye highlight
            // Head 1 and LeftEye 1, which drew it at twice its height)
            std::sort(cluster.begin(), cluster.end(), [](const std::pair<size_t, float>& a, const std::pair<size_t, float>& b) {
                return a.second > b.second;
            });
            int clusterSize = cluster.size() > 4 ? 4 : cluster.size();
            int clusterEmptySize = 4 - clusterSize;
            float weightSum = 0.0f;
            for (int i = 0; i < clusterSize; i++) {
                weightSum += cluster[i].second;
            }
            const float weightScale = weightSum > 0.0f ? 1.0f / weightSum : 1.0f;

            for (int i = 0; i < clusterSize; i++) {
                meshPtr->clusterIndices.push_back(cluster[i].first);
                meshPtr->clusterWeights.push_back(
                    std::round(cluster[i].second * weightScale * 65535.0f)
                );
            }
            for (int i = 0; i < clusterEmptySize; i++) {
                meshPtr->clusterIndices.push_back(0);
                meshPtr->clusterWeights.push_back(0);
            }
        }

        // the mesh at its bind pose, where its clusters put it; a vertex with no bone weights stays by its node
        const glm::mat4 nodeTransform = shapes.isEmpty() ? hfmModel->offset : hfmModel->joints[shapes.first()->joint].globalTransform;
        Extents bindPoseExtents;
        for (size_t vertexIndex = 0; vertexIndex < mMesh->mNumVertices && vertexIndex < (size_t)meshPtr->vertices.size(); vertexIndex++) {
            const glm::vec4 vertex(meshPtr->vertices[(int)vertexIndex], 1.0f);
            glm::vec3 bindPosition(0.0f);
            float totalWeight = 0.0f;
            auto influences = clusterMap.constFind(vertexIndex);
            if (influences != clusterMap.constEnd()) {
                for (const auto& influence : influences.value()) {
                    const hfm::Cluster& influenceCluster = skinDeformer->clusters[influence.first];
                    const glm::mat4 bindMatrix =
                        hfmModel->joints[influenceCluster.jointIndex].globalTransform * influenceCluster.inverseBindMatrix;
                    bindPosition += influence.second * glm::vec3(bindMatrix * vertex);
                    totalWeight += influence.second;
                }
            }
            bindPoseExtents.addPoint(totalWeight > 0.0f ? bindPosition / totalWeight : glm::vec3(nodeTransform * vertex));
        }
        if (bindPoseExtents.isValid()) {
            skinnedMeshExtents[meshIndex] = bindPoseExtents;
        }
    }

    // The model's extents (its natural dimensions, the head's clip distance) take a skinned mesh at its bind pose.
    // processNode took every mesh through its node, which doesn't place a skinned one: a glTF armature at 1/100 scale
    // left the model a hundredth of its body, so it read about 2 cm. The meshes' and shapes' own extents keep the
    // node's frame, as hfm::Shape::transformedExtents documents.
    hfmModel->meshExtents.reset();
    for (int meshIndex = 0; meshIndex < (int)hfmModel->meshes.size(); meshIndex++) {
        auto skinned = skinnedMeshExtents.constFind((size_t)meshIndex);
        const Extents& extents = skinned != skinnedMeshExtents.constEnd() ? skinned.value() : hfmModel->meshes[meshIndex].meshExtents;
        if (extents.isValid()) {
            hfmModel->meshExtents.addExtents(extents);
        }
    }
}

void AssimpSerializer::processAnimations() {
    if (scene->mNumAnimations == 0) return;
    
    // TODO: finish adding translation

    aiAnimation* animation = scene->mAnimations[0];
    size_t totalFrames = 0;

    for (size_t jointIndex = 0; jointIndex < animation->mNumChannels; jointIndex++) {
        auto animJoint = animation->mChannels[jointIndex];
        // if (animJoint->mNumPositionKeys > totalFrames) totalFrames = animJoint->mNumPositionKeys;
        if (animJoint->mNumRotationKeys > totalFrames) totalFrames = animJoint->mNumRotationKeys;
    }

    for (size_t frame = 0; frame < totalFrames; frame++) {
        hfmModel->animationFrames.append(HFMAnimationFrame());
        auto animationFrame = &hfmModel->animationFrames.back();

        // fill up the frame first
        for (size_t jointIndex = 0; jointIndex < hfmModel->joints.size(); jointIndex++) {
            auto joint = &hfmModel->joints[jointIndex];
            animationFrame->translations.push_back(joint->translation);
            animationFrame->rotations.push_back(joint->rotation);
        }

        for (size_t channelIndex = 0; channelIndex < animation->mNumChannels; channelIndex++) {
            auto channel = animation->mChannels[channelIndex];

            QString jointName = channel->mNodeName.C_Str(); 
            if (!hfmModel->jointIndices.contains(jointName)) {
                continue;
            }

            auto jointIndex = hfmModel->jointIndices[jointName];
            // auto joint = &hfmModel->joints[jointIndex];

            // auto positionKey = channel->mPositionKeys[frame];
            // animationFrame->translations[jointIndex] = 
            //     extractTranslation(joint->globalTransform) -
            //     asVec3(positionKey.mValue);
            
            auto rotationKey = channel->mRotationKeys[frame];
            animationFrame->rotations[jointIndex] = asQuat(rotationKey.mValue);            
        }
    }
}

void AssimpSerializer::processNode(const aiNode* aiNode, int parentIndex) {
    auto nodeIndex = hfmModel->joints.size();
    hfmModel->joints.emplace_back();
    hfmModel->jointIndices["x"]++;
    hfm::Joint* nodePtr = &hfmModel->joints.back();

    // set up node (joint)
    nodePtr->name = aiNode->mName.C_Str();
    hfmModel->jointIndices[nodePtr->name] = nodeIndex;

    nodePtr->parentIndex = parentIndex;
    nodePtr->isSkeletonJoint = false;

    nodePtr->localTransform = asMat4(aiNode->mTransformation);
    nodePtr->translation = extractTranslation(nodePtr->localTransform);
    nodePtr->rotation = glmExtractRotation(nodePtr->localTransform);
    glm::vec3 scale = extractScale(nodePtr->localTransform);
    nodePtr->postTransform = glm::scale(glm::mat4(), scale);

    if (nodePtr->parentIndex == -1) {
        nodePtr->transform = nodePtr->localTransform;
        nodePtr->globalTransform = hfmModel->offset * nodePtr->localTransform;
        nodePtr->inverseDefaultRotation = glm::inverse(nodePtr->rotation);
        nodePtr->distanceToParent = 0.0f;
    } else {
        const auto& parentNode = hfmModel->joints[nodePtr->parentIndex];
        nodePtr->transform = parentNode.transform * nodePtr->localTransform;
        nodePtr->globalTransform = parentNode.globalTransform * nodePtr->localTransform;
        nodePtr->inverseDefaultRotation = glm::inverse(nodePtr->rotation) * parentNode.inverseDefaultRotation;
        nodePtr->distanceToParent = glm::distance(
            extractTranslation(parentNode.transform),
            extractTranslation(nodePtr->transform)
        );
    }

    // for all meshes in node, create shape
    for (size_t i = 0; i < aiNode->mNumMeshes; i++) {
        auto meshIndex = aiNode->mMeshes[i];
        hfm::Mesh* mesh = &hfmModel->meshes[meshIndex];

        // no verticies and indices causes segfault
        if (mesh->vertices.size() == 0) continue;
        bool hasIndicies = false;
        for (auto part : mesh->parts) {
            if (part.triangleIndices.size() > 0) {
                hasIndicies = true;
                break;
            }
        }
        if (!hasIndicies) continue;

        hfmModel->shapes.emplace_back();
        hfm::Shape* shapePtr = &hfmModel->shapes.back();

        shapePtr->joint = nodeIndex;
        shapePtr->mesh = meshIndex;
        shapePtr->meshPart = 0;

        // TODO: fixes bounding box? seems not for https://files.tivolicloud.com/caitlyn/fun/cu-cat/cu-cat.glb
        // mesh->modelTransform = nodePtr->globalTransform;
        hfmModel->meshIndicesToModelNames[meshIndex] = nodePtr->name;

        for (auto vertex : mesh->vertices) {
            glm::vec3 transformedVertex = glm::vec3(nodePtr->globalTransform * glm::vec4(vertex, 1.0f));
            mesh->meshExtents.addPoint(transformedVertex);
        }
        shapePtr->transformedExtents = mesh->meshExtents;
        hfmModel->meshExtents.addExtents(mesh->meshExtents);

        auto materialIndex = scene->mMeshes[meshIndex]->mMaterialIndex;
        if (materialIndex < hfmModel->materials.size()) {
            shapePtr->material = materialIndex;
        }
    }

    // process children
    for (size_t i = 0; i < aiNode->mNumChildren; i++) {
        processNode(aiNode->mChildren[i], nodeIndex);
    }
}

void AssimpSerializer::processScene(const hifi::VariantHash& mapping) {
    hfmModel = std::make_shared<HFMModel>();
    hfmModel->originalURL = url.toString();

    // apply fst offset
    glm::vec3 offsetTranslation = glm::vec3(
        mapping.value("tx").toFloat(), mapping.value("ty").toFloat(), mapping.value("tz").toFloat()
    );
    glm::quat offsetRotation = glm::quat(glm::radians(glm::vec3(
        mapping.value("rx").toFloat(), mapping.value("ry").toFloat(), mapping.value("rz").toFloat()
    )));
    glm::vec3 offsetScale = glm::vec3(mapping.value("scale", 1.0f).toFloat());
    hfmModel->offset = (
        glm::translate(offsetTranslation) *
        glm::mat4_cast(offsetRotation) *
        glm::scale(offsetScale)
    );

    processMaterials();
    processMeshes(mapping);

    hfmModel->jointIndices["x"] = 0;
    processNode(scene->mRootNode);

    processBones();

    processAnimations();
}

// Assimp drops glTF "extras" (e.g., Blender custom properties), so read the nodes' and materials' extras from the file's
// JSON and match them to the joints and materials by name.
void AssimpSerializer::processGltfExtras(const hifi::ByteArray& data) {
    hifi::ByteArray json = data;
    if (data.startsWith("glTF")) {
        // A .glb is a 12-byte header, then the JSON chunk: its length, its type, and the JSON itself.
        const int JSON_LENGTH_OFFSET = 12;
        const int JSON_OFFSET = 20;
        if (data.size() < JSON_OFFSET) {
            return;
        }
        auto jsonLength = qFromLittleEndian<quint32>(data.constData() + JSON_LENGTH_OFFSET);
        json = data.mid(JSON_OFFSET, jsonLength);
    }
    QJsonObject gltf = QJsonDocument::fromJson(json).object();

    auto getExtrasByName = [](const QJsonArray& items) {
        QHash<QString, QVariantMap> extrasByName;
        for (const auto& item : items) {
            QJsonObject object = item.toObject();
            QString name = object["name"].toString();
            QJsonObject extras = object["extras"].toObject();
            if (!name.isEmpty() && !extras.isEmpty()) {
                extrasByName[name] = extras.toVariantMap();
            }
        }
        return extrasByName;
    };

    auto nodeExtras = getExtrasByName(gltf["nodes"].toArray());
    for (auto& joint : hfmModel->joints) {
        joint.extras = nodeExtras.value(joint.name);
    }
    auto materialExtras = getExtrasByName(gltf["materials"].toArray());
    for (auto& material : hfmModel->materials) {
        material.extras = materialExtras.value(material.name);
        // our own extra, as in material JSON: "keep" lights back faces with the normal as it comes (foliage cards)
        if (material._material && material.extras.value("doubleSidedNormals").toString() == "keep") {
            material._material->setKeepBackFaceNormal(true);
        }
    }
}

#ifdef ENABLE_ASSIMP_LOGGING
class TivoliLogStream : public Assimp::LogStream {
public:
    QString filename;
    TivoliLogStream(QString filename) : filename(filename) {}
    void write(const char* message) override {
        qCDebug(modelformat) << filename << QString(message).trimmed().toLatin1().data();
    }
};
#endif

static const unsigned int UNUSED_VERTEX = 0xffffffff;

// Keeps of a mesh's per-vertex `array` the vertices `remap` gives a place to, at that place
template <typename T>
static void keepVertices(T*& array, const std::vector<unsigned int>& remap, unsigned int count) {
    if (!array) {
        return;
    }
    T* kept = new T[count];
    for (size_t vertexIndex = 0; vertexIndex < remap.size(); vertexIndex++) {
        if (remap[vertexIndex] != UNUSED_VERTEX) {
            kept[remap[vertexIndex]] = array[vertexIndex];
        }
    }
    delete[] array;
    array = kept;
}

// Drops the vertices no face uses from each skinned mesh, with their morph targets and bone weights, and the bones then
// left weighting nothing. Where a glTF's primitives share one vertex buffer (VRoid's do), Assimp gives each primitive's
// mesh the whole buffer, and every bone that weights any of it. Joining identical vertices then left a bone whose weights
// were all on the other primitives' vertices with its old vertex numbers (JoinVerticesProcess keeps a bone's weights when
// none of them carries over), and those numbers now pointed at this mesh's own vertices: three-vrm-girl's hair came out
// weighted to her hand. The vertices keep their order, so the join that follows gives what it gave before for the rest.
static void compactSkinnedMeshes(aiScene* scene) {
    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++) {
        aiMesh* mesh = scene->mMeshes[meshIndex];
        if (mesh->mNumBones == 0) {
            continue;
        }

        std::vector<unsigned int> remap(mesh->mNumVertices, UNUSED_VERTEX);
        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; faceIndex++) {
            const aiFace& face = mesh->mFaces[faceIndex];
            for (unsigned int i = 0; i < face.mNumIndices; i++) {
                if (face.mIndices[i] < mesh->mNumVertices) {
                    remap[face.mIndices[i]] = 0;
                }
            }
        }
        unsigned int count = 0;
        for (auto& place : remap) {
            if (place != UNUSED_VERTEX) {
                place = count++;
            }
        }
        if (count == 0 || count == mesh->mNumVertices) {
            continue;
        }

        keepVertices(mesh->mVertices, remap, count);
        keepVertices(mesh->mNormals, remap, count);
        keepVertices(mesh->mTangents, remap, count);
        keepVertices(mesh->mBitangents, remap, count);
        for (auto& colors : mesh->mColors) {
            keepVertices(colors, remap, count);
        }
        for (auto& textureCoords : mesh->mTextureCoords) {
            keepVertices(textureCoords, remap, count);
        }
        for (unsigned int animMeshIndex = 0; animMeshIndex < mesh->mNumAnimMeshes; animMeshIndex++) {
            aiAnimMesh* animMesh = mesh->mAnimMeshes[animMeshIndex];
            if (animMesh->mNumVertices != mesh->mNumVertices) {
                continue;
            }
            keepVertices(animMesh->mVertices, remap, count);
            keepVertices(animMesh->mNormals, remap, count);
            keepVertices(animMesh->mTangents, remap, count);
            keepVertices(animMesh->mBitangents, remap, count);
            for (auto& colors : animMesh->mColors) {
                keepVertices(colors, remap, count);
            }
            for (auto& textureCoords : animMesh->mTextureCoords) {
                keepVertices(textureCoords, remap, count);
            }
            animMesh->mNumVertices = count;
        }
        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; faceIndex++) {
            aiFace& face = mesh->mFaces[faceIndex];
            for (unsigned int i = 0; i < face.mNumIndices; i++) {
                face.mIndices[i] = face.mIndices[i] < mesh->mNumVertices ? remap[face.mIndices[i]] : 0;
            }
        }

        unsigned int keptBones = 0;
        for (unsigned int boneIndex = 0; boneIndex < mesh->mNumBones; boneIndex++) {
            aiBone* bone = mesh->mBones[boneIndex];
            const bool hadWeights = bone->mNumWeights > 0;
            unsigned int keptWeights = 0;
            for (unsigned int weightIndex = 0; weightIndex < bone->mNumWeights; weightIndex++) {
                const aiVertexWeight& weight = bone->mWeights[weightIndex];
                if (weight.mVertexId < mesh->mNumVertices && remap[weight.mVertexId] != UNUSED_VERTEX) {
                    bone->mWeights[keptWeights++] = aiVertexWeight(remap[weight.mVertexId], weight.mWeight);
                }
            }
            bone->mNumWeights = keptWeights;
            if (hadWeights && keptWeights == 0) {
                delete bone;
            } else {
                mesh->mBones[keptBones++] = bone;
            }
        }
        mesh->mNumBones = keptBones;
        if (keptBones == 0) {
            delete[] mesh->mBones;
            mesh->mBones = nullptr;
        }
        mesh->mNumVertices = count;
    }
}

// A glTF binary's chunks start on 4-byte boundaries: the JSON chunk is padded with spaces to a multiple of 4 (glTF 2.0,
// "Binary glTF Layout"). Some exporters leave the padding out (16 of 45 VRMs in one collection, 10-27), and Assimp then
// looks for the BIN chunk 1-3 bytes too far on and gives up ("GLTF: BIN chunk missing"). With the BIN chunk straight
// after the unpadded JSON, this puts the padding in, in a copy; anything else (a padded file, one whose padding is there
// but not counted, which Assimp reads, or not a glTF binary) comes back as it is.
static hifi::ByteArray padGlbJsonChunk(const hifi::ByteArray& data) {
    const int HEADER = 12;
    const int CHUNK_HEADER = 8;
    const quint32 MAGIC_GLTF = 0x46546C67;
    const quint32 CHUNK_JSON = 0x4E4F534A;
    const quint32 CHUNK_BIN = 0x004E4942;
    if (data.size() < HEADER + 2 * CHUNK_HEADER || qFromLittleEndian<quint32>(data.constData()) != MAGIC_GLTF ||
        qFromLittleEndian<quint32>(data.constData() + HEADER + 4) != CHUNK_JSON) {
        return data;
    }
    const quint32 jsonLength = qFromLittleEndian<quint32>(data.constData() + HEADER);
    const int padding = (4 - jsonLength % 4) % 4;
    if (padding == 0 || jsonLength > (quint32)(data.size() - HEADER - 2 * CHUNK_HEADER)) {
        return data;
    }
    const int jsonEnd = HEADER + CHUNK_HEADER + (int)jsonLength;
    if (qFromLittleEndian<quint32>(data.constData() + jsonEnd + 4) != CHUNK_BIN) {
        return data;
    }
    hifi::ByteArray padded;
    padded.reserve(data.size() + padding);
    padded.append(data.constData(), jsonEnd);
    padded.append(padding, ' ');
    padded.append(data.constData() + jsonEnd, data.size() - jsonEnd);
    qToLittleEndian<quint32>(qFromLittleEndian<quint32>(data.constData() + 8) + padding, padded.data() + 8);
    qToLittleEndian<quint32>(jsonLength + padding, padded.data() + HEADER);
    return padded;
}

HFMModel::Pointer AssimpSerializer::read(const hifi::ByteArray& data, const hifi::VariantHash& mapping, const hifi::URL& inputUrl) {
	url = inputUrl;

    // normalize url for local files
    hifi::URL normalizeUrl = DependencyManager::get<ResourceManager>()->normalizeURL(url);
    if (normalizeUrl.scheme().isEmpty() || (normalizeUrl.scheme() == "file")) {
        QString localFileName = PathUtils::expandToLocalDataAbsolutePath(normalizeUrl).toLocalFile();
        url = hifi::URL(QFileInfo(localFileName).absoluteFilePath());
    }

    // what follows the last dot, as the format registry chose this reader by: Blender's Tree.001.glb is a "glb"
    ext = QFileInfo(url.path()).suffix();

    // qCDebug(modelformat) << "AssimpSerializer::read url"<<url<<ext;

#ifdef ENABLE_ASSIMP_LOGGING
    TivoliLogStream tivoliLogStream = TivoliLogStream(url.fileName());
    auto errorSeverity = 
        // Assimp::Logger::ErrorSeverity::Debugging |
        // Assimp::Logger::ErrorSeverity::Info |
        Assimp::Logger::ErrorSeverity::Warn |
        Assimp::Logger::ErrorSeverity::Err;

    if (Assimp::DefaultLogger::isNullLogger()) {
        Assimp::DefaultLogger::create("");
    }

    Assimp::DefaultLogger::get()->attachStream(&tivoliLogStream, errorSeverity);
#endif

    Assimp::Importer importer;
    importer.SetIOHandler(new TivoliIOSystem(url));
    
    // we don't support lights and cameras over models
    importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, aiComponent_LIGHTS | aiComponent_CAMERAS);

    // vertex cache locality
    // https://arbook.icg.tugraz.at/schmalstieg/Schmalstieg_351.pdf
    importer.SetPropertyInteger(AI_CONFIG_PP_ICL_PTCACHE_SIZE, 64);

    // Joining identical vertices, and the steps Assimp runs after it, wait until each skinned mesh holds only the
    // vertices its faces use (compactSkinnedMeshes)
    const unsigned int AFTER_COMPACTING = aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights | aiProcess_ImproveCacheLocality;

    const hifi::ByteArray glb = padGlbJsonChunk(data);
    if (glb.size() != data.size()) {
        qCDebug(modelformat) << "AssimpSerializer::read" << url.fileName() << "has its JSON chunk unpadded: padded it";
    }
    scene = importer.ReadFileFromMemory(
        glb.constData(), glb.size(),
        // aiProcess_CalcTangentSpace |
        aiProcess_Triangulate |
        aiProcess_RemoveComponent |
        aiProcess_GenNormals | 
        aiProcess_SplitLargeMeshes | // split large meshes every 1 million verticies and triangles (now before joining)
        // aiProcess_RemoveRedundantMaterials | // ends up removing necessary materials...
        aiProcess_PopulateArmatureData | 
        aiProcess_FindInstances | // can be slow but instances duplicate meshes
        aiProcess_OptimizeMeshes | // joins meshes together
        // aiProcess_OptimizeGraph |
        aiProcess_FlipUVs,
        ext.toLocal8Bit()
    );
    if (scene) {
        compactSkinnedMeshes(const_cast<aiScene*>(scene));
        scene = importer.ApplyPostProcessing(AFTER_COMPACTING);
    }

    if (!scene) {
        qCDebug(modelformat) << "AssimpSerializer::read Error parsing model file"<<importer.GetErrorString();
        return nullptr;
    }

    processScene(mapping);
    if (ext == "gltf" || ext == "glb") {
        processGltfExtras(data);
    }

#ifdef ENABLE_ASSIMP_LOGGING
    Assimp::DefaultLogger::get()->detachStream(&tivoliLogStream, errorSeverity);
#endif

    return hfmModel;
}
