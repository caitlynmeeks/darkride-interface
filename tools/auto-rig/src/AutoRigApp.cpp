//
//  AutoRigApp.cpp
//  tools/auto-rig/src
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AutoRigApp.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QRegExp>
#include <QUrl>

#include <AssimpSerializer.h>
#include <FBX.h>
#include <FBXSerializer.h>
#include <GLMHelpers.h>
#include <GLTFSerializer.h>
#include <OBJSerializer.h>

#include <AutoRig.h>
#include <AvatarWriter.h>
#include <FstBake.h>
#include <VRMReader.h>
#include <BlendshapeConstants.h>
#include <StandardSkeleton.h>

#include "RigCheck.h"
#include "RigPreview.h"

static HFMModel::Pointer readModel(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return nullptr;
    }
    const QByteArray data = file.readAll();
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    const QUrl url = QUrl::fromLocalFile(info.absoluteFilePath());
    const QVariantHash mapping;
    if (suffix == "fbx") {
        return FBXSerializer().read(data, mapping, url);
    }
    if (suffix == "gltf" || suffix == "glb") {
        return GLTFSerializer().read(data, mapping, url);
    }
    if (suffix == "obj") {
        return OBJSerializer().read(data, mapping, url);
    }
    return AssimpSerializer().read(data, mapping, url);
}

// The answer key's joints under their standard names: matched without case, after any prefix ("mixamorig:Hips").
static std::map<QString, glm::vec3> keyJoints(const HFMModel& key) {
    const autorig::StandardSkeleton& standard = autorig::StandardSkeleton::get();
    std::map<QString, glm::vec3> joints;
    for (const HFMJoint& joint : key.joints) {
        const QString name = joint.name.section(':', -1).toLower();
        for (int i = 0; i < standard.size(); i++) {
            const QString standardName = standard.joint(i).name;
            if (standardName.toLower() == name && !joints.count(standardName)) {
                joints[standardName] = extractTranslation(joint.globalTransform);
            }
        }
    }
    return joints;
}


// Prints an FBX file's node tree as the engine's parser sees it: each node's name, properties and child count.
static void dumpNode(const FBXNode& node, int depth, int maxDepth, int maxChildren) {
    QStringList properties;
    for (int i = 0; i < node.properties.size() && i < 8; i++) {
        const QVariant& p = node.properties.at(i);
        const QString type = p.typeName();
        if (p.type() == QVariant::ByteArray) {
            QString text = QString::fromLatin1(p.toByteArray().left(60));
            text.replace(QChar('\0'), "\\0").replace(QChar('\1'), "\\1");
            properties << "\"" + text + "\"";
        } else if (type.startsWith("QVector")) {
            int size = 0;
            if (type == "QVector<double>") size = p.value<QVector<double>>().size();
            else if (type == "QVector<float>") size = p.value<QVector<float>>().size();
            else if (type == "QVector<int>") size = p.value<QVector<int>>().size();
            else if (type == "QVector<qlonglong>") size = p.value<QVector<qlonglong>>().size();
            properties << QString("%1[%2]").arg(type).arg(size);
        } else {
            properties << QString("%1:%2").arg(type).arg(p.toString().left(40));
        }
    }
    fprintf(stdout, "%*s%s %s%s\n", depth * 2, "", node.name.constData(), qPrintable(properties.join(", ")),
            node.children.isEmpty() ? "" : qPrintable(QString(" {%1}").arg(node.children.size())));
    if (depth >= maxDepth) {
        return;
    }
    int shown = 0;
    QMap<QByteArray, int> skipped;
    for (const FBXNode& child : node.children) {
        if (shown < maxChildren) {
            dumpNode(child, depth + 1, maxDepth, maxChildren);
            shown++;
        } else {
            skipped[child.name]++;
        }
    }
    for (auto it = skipped.begin(); it != skipped.end(); ++it) {
        fprintf(stdout, "%*s... %d more %s\n", (depth + 1) * 2, "", it.value(), it.key().constData());
    }
}


// Prints a model's meshes (vertices, UV sets, parts and their materials), materials and textures (with UV set).
static void inspectModel(const HFMModel& model) {
    auto textureLine = [](const char* slot, const hfm::Texture& texture) {
        if (texture.filename.isEmpty() && texture.content.isEmpty()) {
            return QString();
        }
        QString transform;
        if (!texture.transform.isIdentity()) {
            const glm::vec3 t = texture.transform.getTranslation();
            const glm::vec3 s = texture.transform.getScale();
            const glm::vec3 r = glm::degrees(glm::eulerAngles(texture.transform.getRotation()));
            transform = QString(" moved %1 %2, scaled %3 %4, turned %5").arg(t.x).arg(t.y).arg(s.x).arg(s.y).arg(r.z);
        }
        return QString(" %1=%2(set %3 %4%5)").arg(slot).arg(QString::fromUtf8(texture.filename).section('/', -1).section('\\', -1))
            .arg(texture.texcoordSet).arg(texture.texcoordSetName).arg(transform);
    };
    for (size_t m = 0; m < model.materials.size(); m++) {
        const hfm::Material& material = model.materials[m];
        fprintf(stdout, "material %zu %s: opacity %.2f%s%s%s%s\n", m, qPrintable(material.name), material.opacity,
                qPrintable(textureLine("albedo", material.albedoTexture)), qPrintable(textureLine("normal", material.normalTexture)),
                qPrintable(textureLine("opacity", material.opacityTexture)), qPrintable(textureLine("specular", material.specularTexture)));
        // the engine makes a phong material as metallic as its specular colour's brightest channel (FBXSerializer_Material)
        fprintf(stdout, "    diffuse (%.2f %.2f %.2f) x %.2f, specular (%.2f %.2f %.2f), shininess %.1f, %s, metallic %.2f roughness %.2f\n",
                material.diffuseColor.x, material.diffuseColor.y, material.diffuseColor.z, material.diffuseFactor,
                material.specularColor.x, material.specularColor.y, material.specularColor.z, material.shininess,
                material.isPBSMaterial ? "PBS" : "phong", material.metallic, material.roughness);
    }
    std::map<uint32_t, QStringList> partsOf;
    for (const hfm::Shape& shape : model.shapes) {
        if (shape.mesh < model.meshes.size() && shape.meshPart < model.meshes[shape.mesh].parts.size()) {
            const hfm::MeshPart& part = model.meshes[shape.mesh].parts[shape.meshPart];
            partsOf[shape.mesh] << QString("part %1: %2 triangles, material %3, joint %4")
                                       .arg(shape.meshPart).arg((part.triangleIndices.size() + part.quadTrianglesIndices.size()) / 3)
                                       .arg(shape.material).arg(shape.joint);
        }
    }
    for (size_t m = 0; m < model.meshes.size(); m++) {
        const hfm::Mesh& mesh = model.meshes[m];
        Extents extents;
        for (const glm::vec3& v : mesh.vertices) {
            extents.addPoint(v);
        }
        fprintf(stdout, "mesh %zu %s: %d vertices, normals %d, uv0 %d, uv1 %d, from (%.1f %.1f %.1f) to (%.1f %.1f %.1f)\n", m,
                qPrintable(model.getModelNameOfMesh((int)m)), mesh.vertices.size(), mesh.normals.size(), mesh.texCoords.size(),
                mesh.texCoords1.size(), extents.minimum.x, extents.minimum.y, extents.minimum.z, extents.maximum.x,
                extents.maximum.y, extents.maximum.z);
        for (const QString& line : partsOf[(uint32_t)m]) {
            fprintf(stdout, "    %s\n", qPrintable(line));
        }
        // the blendshapes it has, by Darkride's names, with how many vertices each moves
        QStringList blendshapes;
        for (int b = 0; b < (int)mesh.blendshapes.size() && b < (int)Blendshapes::BlendshapeCount; b++) {
            if (!mesh.blendshapes[b].indices.isEmpty()) {
                blendshapes << QString("%1 %2").arg(BLENDSHAPE_NAMES[b]).arg(mesh.blendshapes[b].indices.size());
            }
        }
        if (!blendshapes.isEmpty()) {
            fprintf(stdout, "    %d blendshapes: %s\n", blendshapes.size(), qPrintable(blendshapes.join(", ")));
        }
        // how far each UV layer spreads: a layer collapsed to a point (an unused lightmap channel) draws one texel
        for (int layer = 0; layer < 2; layer++) {
            const QVector<glm::vec2>& uvs = layer == 0 ? mesh.texCoords : mesh.texCoords1;
            if (uvs.isEmpty()) {
                continue;
            }
            glm::vec2 low = uvs[0];
            glm::vec2 high = uvs[0];
            for (const glm::vec2& uv : uvs) {
                low = glm::min(low, uv);
                high = glm::max(high, uv);
            }
            fprintf(stdout, "    uv%d spans u %.3f to %.3f, v %.3f to %.3f\n", layer, low.x, high.x, low.y, high.y);
        }
        // flipped normals: a triangle whose vertex normals point against its winding (its front, which is drawn)
        if (mesh.normals.size() == mesh.vertices.size()) {
            int against = 0;
            int total = 0;
            for (const hfm::MeshPart& part : mesh.parts) {
                for (const QVector<int>* indices : { &part.triangleIndices, &part.quadTrianglesIndices }) {
                    for (int i = 0; i + 2 < indices->size(); i += 3) {
                        const int a = indices->at(i), b = indices->at(i + 1), c = indices->at(i + 2);
                        if (a < 0 || b < 0 || c < 0 || a >= mesh.vertices.size() || b >= mesh.vertices.size() ||
                            c >= mesh.vertices.size()) {
                            continue;
                        }
                        const glm::vec3 front = glm::cross(mesh.vertices[b] - mesh.vertices[a], mesh.vertices[c] - mesh.vertices[a]);
                        const glm::vec3 normal = mesh.normals[a] + mesh.normals[b] + mesh.normals[c];
                        against += glm::dot(front, normal) < 0.0f ? 1 : 0;
                        total++;
                    }
                }
            }
            fprintf(stdout, "    normals against the winding: %d of %d triangles\n", against, total);
        }
    }
}

// Draws each material's UV triangles over its colour texture into <folder>/<material>-<name>.png, the first UV layer
// (red) on the left and the second (cyan) on the right: whether each part's coordinates land on its part of the texture
// (Chloe's jacket showed patches of skin, 10-13). V counts down from the picture's top edge, as the engine draws it
// (its FBX reader negates the file's V).
static void drawUVSheets(const HFMModel& model, const QString& modelPath, const QString& folder) {
    const int SIDE = 1024;
    QDir().mkpath(folder);
    const QDir modelFolder = QFileInfo(modelPath).absoluteDir();
    for (size_t m = 0; m < model.materials.size(); m++) {
        const hfm::Texture& texture = model.materials[m].albedoTexture;
        QImage picture;
        if (!texture.content.isEmpty()) {
            picture = QImage::fromData(texture.content);
        }
        const QString name = QString::fromUtf8(texture.filename);
        if (picture.isNull() && !name.isEmpty()) {
            const QUrl url(name);
            picture.load(url.isLocalFile() ? url.toLocalFile() : modelFolder.filePath(name));
            if (picture.isNull()) {
                picture.load(modelFolder.filePath(name.section('/', -1).section('\\', -1)));
            }
        }
        QImage panel(SIDE, SIDE, QImage::Format_RGB32);
        panel.fill(Qt::darkGray);
        if (!picture.isNull()) {
            panel = picture.scaled(SIDE, SIDE, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
        }
        QImage sheet(SIDE * 2, SIDE, QImage::Format_RGB32);
        QPainter painter(&sheet);
        painter.drawImage(0, 0, panel);
        painter.drawImage(SIDE, 0, panel);
        painter.setBrush(Qt::NoBrush);
        int drawn = 0;
        for (const hfm::Shape& shape : model.shapes) {
            if (shape.material != m || shape.mesh >= model.meshes.size() || shape.meshPart >= model.meshes[shape.mesh].parts.size()) {
                continue;
            }
            const hfm::Mesh& mesh = model.meshes[shape.mesh];
            const hfm::MeshPart& part = mesh.parts[shape.meshPart];
            for (int layer = 0; layer < 2; layer++) {
                const QVector<glm::vec2>& uvs = layer == 0 ? mesh.texCoords : mesh.texCoords1;
                if (uvs.size() != mesh.vertices.size()) {
                    continue;
                }
                painter.setPen(QPen(layer == 0 ? QColor(255, 40, 40, 150) : QColor(40, 255, 255, 150), 1.0));
                for (const QVector<int>* indices : { &part.triangleIndices, &part.quadTrianglesIndices }) {
                    for (int i = 0; i + 2 < indices->size(); i += 3) {
                        const int a = indices->at(i), b = indices->at(i + 1), c = indices->at(i + 2);
                        if (a < 0 || b < 0 || c < 0 || a >= uvs.size() || b >= uvs.size() || c >= uvs.size()) {
                            continue;
                        }
                        // a triangle on a repeated tile is drawn on the first one
                        const glm::vec2 tile = glm::floor(glm::min(uvs[a], glm::min(uvs[b], uvs[c])));
                        QPointF corners[3];
                        int k = 0;
                        for (const int v : { a, b, c }) {
                            const glm::vec2 uv = uvs[v] - tile;
                            corners[k++] = QPointF(layer * SIDE + uv.x * SIDE, uv.y * SIDE);
                        }
                        painter.drawPolygon(corners, 3);
                        drawn += layer == 0 ? 1 : 0;
                    }
                }
            }
        }
        painter.end();
        const QString path = QDir(folder).filePath(QString("%1-%2.png").arg(m).arg(model.materials[m].name));
        sheet.save(path);
        fprintf(stdout, "uv sheet %s: %d triangles over %s\n", qPrintable(path), drawn,
                picture.isNull() ? "no picture" : qPrintable(name.section('/', -1)));
    }
}

AutoRigApp::AutoRigApp(int argc, char* argv[]) : QCoreApplication(argc, argv) {
    QCommandLineParser parser;
    parser.setApplicationDescription("Darkride auto-rigger: places the standard skeleton in a character");
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption inputOption("i", "the character (FBX, glTF, OBJ, or anything Assimp reads)", "file");
    const QCommandLineOption keyOption("key", "a rigged file to measure against (default: the input's own rig)", "file");
    const QCommandLineOption turnOption("turn-around", "the character faces -Z: turn it to face +Z");
    const QCommandLineOption jsonOption("json", "write the joints and the answer key as JSON", "file");
    const QCommandLineOption outputOption("o", "write the rigged avatar (FBX, textures, .fst) into this folder", "folder");
    const QCommandLineOption nameOption("name", "the avatar's name (default: the input file's)", "name");
    const QCommandLineOption flipVOption("flip-v", "with -o: flip texture V, for a model the engine draws upside down; "
                                         "without this or --keep-v the writer decides from the texture seams");
    const QCommandLineOption keepVOption("keep-v", "with -o: keep texture V as the engine draws the input");
    const QCommandLineOption previewOption("preview", "draw the rigged character into a PNG", "file");
    const QCommandLineOption animationOption("animation", "with --preview: also pose it by an animation", "fbx");
    const QCommandLineOption frameOption("frame", "with --animation: which frame (default 30)", "number");
    const QCommandLineOption focusOption("focus", "with --preview: centre the views on this joint", "joint");
    const QCommandLineOption zoomOption("zoom", "with --preview: magnify by this factor", "factor");
    const QCommandLineOption weightsOption("weights", "with --preview: colour by one joint's weights, ours beside the key's",
                                           "joint");
    const QCommandLineOption sideOption("side", "with --weights: show the weights from the side");
    const QCommandLineOption heightOption("height", "scale the character to this height first (for files in the wrong units)",
                                          "metres");
    const QCommandLineOption profilesOption("profiles", "print the measurements along each arm");
    const QCommandLineOption scaleOption("scale", "scale the character by a factor first (0.3937 for inches read as cm)",
                                         "factor");
    const QCommandLineOption jointsOnlyOption("joints-only", "place the joints and skip the skin weights");
    const QCommandLineOption dumpOption("dump", "print an FBX file's node tree and stop", "file");
    const QCommandLineOption inspectOption("inspect", "print a model's meshes, materials and textures and stop", "file");
    const QCommandLineOption bakeOption("bake", "print the .fst lines Import Avatar writes when it keeps a model's rig, and stop", "file");
    const QCommandLineOption uvSheetsOption("uv-sheets", "with --inspect: draw each material's UV triangles over its texture into PNGs here",
                                            "folder");
    const QCommandLineOption depthOption("depth", "with --dump: how deep (default 3)", "levels");
    const QCommandLineOption childrenOption("children", "with --dump: children shown per node (default 12)", "count");
    const QCommandLineOption falloffOption("falloff", "skin weights: how fast a bone's influence fades (0 to 1)", "a");
    const QCommandLineOption smoothingOption("smoothing", "skin weights: passes of smoothing over the mesh", "passes");
    const QCommandLineOption weightingOption("weighting", "skin weights: 1 joint planes (default), 0 geodesic falloff", "kind");
    const QCommandLineOption sharpnessOption("sharpness", "joint planes: how sharply the nearest bone wins", "k");
    const QCommandLineOption blendOption("blend", "joint planes: half-width of the change at a joint, in thicknesses", "b");
    parser.addOption(inputOption);
    parser.addOption(keyOption);
    parser.addOption(turnOption);
    parser.addOption(jsonOption);
    parser.addOption(outputOption);
    parser.addOption(nameOption);
    parser.addOption(flipVOption);
    parser.addOption(keepVOption);
    parser.addOption(previewOption);
    parser.addOption(animationOption);
    parser.addOption(frameOption);
    parser.addOption(focusOption);
    parser.addOption(zoomOption);
    parser.addOption(weightsOption);
    parser.addOption(sideOption);
    parser.addOption(heightOption);
    parser.addOption(profilesOption);
    parser.addOption(scaleOption);
    parser.addOption(jointsOnlyOption);
    parser.addOption(dumpOption);
    parser.addOption(inspectOption);
    parser.addOption(bakeOption);
    parser.addOption(uvSheetsOption);
    parser.addOption(depthOption);
    parser.addOption(childrenOption);
    parser.addOption(falloffOption);
    parser.addOption(smoothingOption);
    parser.addOption(weightingOption);
    parser.addOption(sharpnessOption);
    parser.addOption(blendOption);
    if (parser.parse(QCoreApplication::arguments()) && parser.isSet(bakeOption)) {
        // read as the client reads a model it wears: glTF and VRM by the Assimp loader
        const QString path = parser.value(bakeOption);
        const QString suffix = QFileInfo(path).suffix().toLower();
        const bool isGltf = suffix == "vrm" || suffix == "glb" || suffix == "gltf";
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            fprintf(stderr, "could not read %s\n", qPrintable(path));
            _returnCode = 2;
            return;
        }
        const QByteArray data = file.readAll();
        HFMModel::Pointer model = isGltf ? AssimpSerializer().read(data, QVariantHash(), QUrl::fromLocalFile(path)) : readModel(path);
        if (!model) {
            fprintf(stderr, "could not read %s\n", qPrintable(path));
            _returnCode = 2;
            return;
        }
        const VRMInfo vrm = isGltf ? readVRM(data) : VRMInfo();
        const QStringList morphTargetNames = isGltf ? autorig::gltfMorphTargetNames(data) : QStringList(model->blendshapeChannelNames);
        QStringList lines, found;
        autorig::bakeFst(*model, vrm, morphTargetNames, lines, found);
        fprintf(stdout, "# found: %s\n%s\n", qPrintable(found.join(", ")), qPrintable(lines.join("\n")));
        return;
    }
    if (parser.parse(QCoreApplication::arguments()) && parser.isSet(inspectOption)) {
        HFMModel::Pointer model = readModel(parser.value(inspectOption));
        if (!model) {
            fprintf(stderr, "could not read %s\n", qPrintable(parser.value(inspectOption)));
            _returnCode = 2;
            return;
        }
        inspectModel(*model);
        if (parser.isSet(uvSheetsOption)) {
            drawUVSheets(*model, parser.value(inspectOption), parser.value(uvSheetsOption));
        }
        return;
    }
    if (parser.parse(QCoreApplication::arguments()) && parser.isSet(dumpOption)) {
        QFile file(parser.value(dumpOption));
        if (!file.open(QIODevice::ReadOnly)) {
            fprintf(stderr, "could not read %s\n", qPrintable(parser.value(dumpOption)));
            _returnCode = 2;
            return;
        }
        const FBXNode root = FBXSerializer::parseFBX(&file);
        dumpNode(root, 0, parser.isSet(depthOption) ? parser.value(depthOption).toInt() : 3,
                 parser.isSet(childrenOption) ? parser.value(childrenOption).toInt() : 12);
        return;
    }
    if (!parser.parse(QCoreApplication::arguments()) || !parser.isSet(inputOption)) {
        parser.showHelp(1);
    }
    if (parser.isSet(helpOption)) {
        parser.showHelp();
        return;
    }

    const QString inputPath = parser.value(inputOption);
    HFMModel::Pointer model = readModel(inputPath);
    if (!model) {
        fprintf(stderr, "could not read %s\n", qPrintable(inputPath));
        _returnCode = 2;
        return;
    }
    autorig::Body body = autorig::gatherBody(*model);
    const QString eyelids = autorig::openEyelids(body, *model);
    if (!eyelids.isEmpty()) {
        fprintf(stdout, "%s\n", qPrintable(eyelids));
    }
    float scale = parser.isSet(scaleOption) ? parser.value(scaleOption).toFloat() : 1.0f;
    if (parser.isSet(heightOption) && body.maximum.y > body.minimum.y) {
        scale = parser.value(heightOption).toFloat() / (body.maximum.y - body.minimum.y);
    }
    if (scale != 1.0f) {
        body.scale(scale);
        fprintf(stdout, "scaled by %.4f\n", scale);
    }
    fprintf(stdout, "%s: %zu vertices, %zu triangles, from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f) cm\n",
            qPrintable(QFileInfo(inputPath).fileName()), body.vertices.size(), body.triangles.size(),
            body.minimum.x * 100.0f, body.minimum.y * 100.0f, body.minimum.z * 100.0f, body.maximum.x * 100.0f,
            body.maximum.y * 100.0f, body.maximum.z * 100.0f);

    const bool turned = parser.isSet(turnOption);
    if (turned) {
        body.turnAround();
    }
    autorig::Options options;
    options.profiles = parser.isSet(profilesOption);
    options.skin = !parser.isSet(jointsOnlyOption);
    if (parser.isSet(falloffOption)) {
        options.falloff = parser.value(falloffOption).toFloat();
    }
    if (parser.isSet(smoothingOption)) {
        options.smoothing = parser.value(smoothingOption).toInt();
    }
    if (parser.isSet(weightingOption)) {
        options.weighting = parser.value(weightingOption).toInt();
    }
    if (parser.isSet(sharpnessOption)) {
        options.sharpness = parser.value(sharpnessOption).toFloat();
    }
    if (parser.isSet(blendOption)) {
        options.blend = parser.value(blendOption).toFloat();
    }
    const autorig::Result result = autorig::rig(body, options);
    for (const QString& note : result.notes) {
        fprintf(stdout, "  %s\n", qPrintable(note));
    }
    for (const QString& line : result.profiles) {
        fprintf(stdout, "    %s\n", qPrintable(line));
    }
    if (!result.ok) {
        fprintf(stderr, "not rigged: %s\n", qPrintable(result.error));
        _returnCode = 3;
        return;
    }

    HFMModel::Pointer key = parser.isSet(keyOption) ? readModel(parser.value(keyOption)) : model;
    std::map<QString, glm::vec3> answers = key ? keyJoints(*key) : std::map<QString, glm::vec3>();
    for (auto& answer : answers) {
        answer.second *= scale;
    }

    // the joints, and how far each is from the answer key's
    fprintf(stdout, "\n%-20s %26s %26s %8s\n", "joint", "placed (cm)", "answer key (cm)", "off (cm)");
    std::vector<std::pair<float, QString>> body_errors, finger_errors;
    QJsonArray jsonJoints;
    for (const autorig::RiggedJoint& joint : result.joints) {
        const glm::vec3 p = joint.position * 100.0f;
        QJsonObject entry {
            { "name", joint.name },
            { "parent", joint.parent },
            { "position", QJsonArray { p.x, p.y, p.z } },
            { "rotation", QJsonArray { joint.rotation.x, joint.rotation.y, joint.rotation.z, joint.rotation.w } },
        };
        auto found = answers.find(joint.name);
        if (found != answers.end()) {
            const glm::vec3 k = found->second * 100.0f;
            const float off = glm::distance(p, k);
            fprintf(stdout, "%-20s %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f\n", qPrintable(joint.name), p.x, p.y, p.z,
                    k.x, k.y, k.z, off);
            entry["key"] = QJsonArray { k.x, k.y, k.z };
            const bool finger = joint.name.contains("Hand") && joint.name != "LeftHand" && joint.name != "RightHand";
            (finger ? finger_errors : body_errors).push_back({ off, joint.name });
        } else {
            fprintf(stdout, "%-20s %8.1f %8.1f %8.1f %26s\n", qPrintable(joint.name), p.x, p.y, p.z, "-");
        }
        jsonJoints.append(entry);
    }
    auto summarize = [](const char* label, std::vector<std::pair<float, QString>> errors) {
        if (errors.empty()) {
            return;
        }
        float sum = 0.0f;
        for (const auto& e : errors) {
            sum += e.first;
        }
        std::sort(errors.begin(), errors.end(), [](const std::pair<float, QString>& a, const std::pair<float, QString>& b) {
            return a.first > b.first;
        });
        QStringList worst;
        for (size_t i = 0; i < std::min<size_t>(5, errors.size()); i++) {
            worst << QString("%1 %2").arg(errors[i].second).arg(errors[i].first, 0, 'f', 1);
        }
        fprintf(stdout, "%s: %zu joints against the key, mean %.1f cm off; worst %s\n", label, errors.size(),
                sum / errors.size(), qPrintable(worst.join(", ")));
    };
    fprintf(stdout, "\n");
    summarize("body", body_errors);
    summarize("fingers", finger_errors);
    if (key && options.skin && !turned) {
        const KeyRig keyRig = readKeyRig(*key, scale);
        compareWeights(result, keyRig);
        compareDeformation(body, result, keyRig);
    }

    if (parser.isSet(outputOption)) {
        if (!options.skin) {
            fprintf(stderr, "writing an avatar needs the skin weights: drop --joints-only\n");
            _returnCode = 4;
            return;
        }
        QString name = parser.isSet(nameOption) ? parser.value(nameOption) : QFileInfo(inputPath).completeBaseName();
        name.replace(QRegExp("[^A-Za-z0-9_-]+"), "_");
        const autorig::WrittenAvatar written =
            autorig::writeAvatar(*model, body, result, inputPath, parser.value(outputOption), name,
                                 parser.isSet(flipVOption)   ? autorig::FlipV::Always
                                 : parser.isSet(keepVOption) ? autorig::FlipV::Never
                                                             : autorig::FlipV::Detect);
        for (const QString& note : written.notes) {
            fprintf(stdout, "%s\n", qPrintable(note));
        }
        if (!written.ok) {
            fprintf(stderr, "not written: %s\n", qPrintable(written.error));
            _returnCode = 5;
            return;
        }
        if (!checkWritten(written.fbxPath, body, result)) {
            _returnCode = 6;
        }
    }
    if (parser.isSet(previewOption) && options.skin) {
        HFMModel::Pointer animation = parser.isSet(animationOption) ? readModel(parser.value(animationOption)) : nullptr;
        const int frame = parser.isSet(frameOption) ? parser.value(frameOption).toInt() : 30;
        std::vector<float> heat, keyHeat;
        if (parser.isSet(weightsOption)) {
            const int joint = autorig::StandardSkeleton::get().indexOf(parser.value(weightsOption));
            heat.assign(body.vertices.size(), 0.0f);
            for (size_t v = 0; v < body.vertices.size(); v++) {
                for (int k = 0; k < 4; k++) {
                    if (result.weights[v].joints[k] == joint) {
                        heat[v] += result.weights[v].weights[k];
                    }
                }
            }
            if (key && !turned) {
                const KeyRig keyRig = readKeyRig(*key, scale);
                if (keyRig.weights.size() == body.vertices.size()) {
                    keyHeat.assign(body.vertices.size(), 0.0f);
                    for (size_t v = 0; v < body.vertices.size(); v++) {
                        float sum = 0.0f;
                        for (const auto& w : keyRig.weights[v]) {
                            sum += w.second;
                            if (keyRig.standard[w.first] == joint) {
                                keyHeat[v] += w.second;
                            }
                        }
                        keyHeat[v] /= std::max(sum, 1.0e-6f);
                    }
                }
            }
        }
        if (renderPreview(body, result, animation.get(), frame, parser.value(previewOption), parser.value(focusOption),
                          parser.isSet(zoomOption) ? parser.value(zoomOption).toFloat() : 1.0f,
                          heat.empty() ? nullptr : &heat, keyHeat.empty() ? nullptr : &keyHeat, parser.isSet(sideOption))) {
            fprintf(stdout, "preview: %s%s\n", qPrintable(parser.value(previewOption)),
                    animation ? qPrintable(QString(" (frame %1 of %2)").arg(frame).arg(animation->animationFrames.size())) : "");
        } else {
            fprintf(stdout, "preview: not drawn\n");
        }
    }
    if (parser.isSet(jsonOption)) {
        QFile out(parser.value(jsonOption));
        if (out.open(QIODevice::WriteOnly)) {
            out.write(QJsonDocument(QJsonObject { { "joints", jsonJoints } }).toJson());
        }
    }
}
