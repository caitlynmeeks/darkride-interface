//
//  AvatarWriter.cpp
//  libraries/auto-rigging/src
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AvatarWriter.h"

#include <algorithm>
#include <array>
#include <map>
#include <tuple>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>
#include <QTextStream>
#include <QVector>

#include <BlendshapeConstants.h>
#include <FBX.h>
#include <FBXWriter.h>
#include <GLMHelpers.h>
#include <hfm/HFM.h>

using namespace autorig;

namespace {

const float CENTIMETRES_PER_METRE = 100.0f;
// The names of the two UV layers: the reader treats "map1" as the first set and any other name as the second.
const QByteArray FIRST_UV_SET = "map1";
const QByteArray SECOND_UV_SET = "UVSet1";

// FBX object names are "name\0\1Class".
QByteArray objectName(const QString& name, const char* kind) {
    return name.toUtf8() + QByteArray("\x00\x01", 2) + QByteArray(kind);
}

FBXNode makeNode(const QByteArray& name, const QVariantList& properties = QVariantList(),
                 const FBXNodeList& children = FBXNodeList()) {
    FBXNode node;
    node.name = name;
    node.properties = properties;
    node.children = children;
    return node;
}

// One "P" entry of a Properties70 block: name, type, label, flags, values.
FBXNode property(const char* name, const char* type, const char* label, const char* flags, const QVariantList& values) {
    QVariantList properties { QByteArray(name), QByteArray(type), QByteArray(label), QByteArray(flags) };
    properties.append(values);
    return makeNode("P", properties);
}

QVariantList vector3(const glm::vec3& v) {
    return { (double)v.x, (double)v.y, (double)v.z };
}

QVariant doubles(const QVector<double>& values) {
    return QVariant::fromValue(values);
}

QVariant ints(const QVector<qint32>& values) {
    return QVariant::fromValue(values);
}

// glm matrices are stored column by column, as FBX expects.
QVector<double> matrixValues(const glm::mat4& m) {
    QVector<double> values;
    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) {
            values.append(m[column][row]);
        }
    }
    return values;
}

FBXNode transformModel(qint64 id, const QString& name, const char* kind, const glm::vec3& translation,
                       const glm::vec3& rotationDegrees) {
    FBXNodeList properties {
        property("RotationActive", "bool", "", "", { 1 }),
        property("InheritType", "enum", "", "", { 1 }),
        property("ScalingMax", "Vector3D", "Vector", "", { 0.0, 0.0, 0.0 }),
        property("DefaultAttributeIndex", "int", "Integer", "", { 0 }),
        property("Lcl Translation", "Lcl Translation", "", "A+", vector3(translation)),
        property("Lcl Rotation", "Lcl Rotation", "", "A+", vector3(rotationDegrees)),
        property("Lcl Scaling", "Lcl Scaling", "", "A", { 1.0, 1.0, 1.0 }),
    };
    return makeNode("Model", { id, objectName(name, "Model"), QByteArray(kind) },
                    { makeNode("Version", { 232 }), makeNode("Properties70", {}, properties),
                      makeNode("Shading", { true }), makeNode("Culling", { QByteArray("CullingOff") }) });
}

FBXNode layerElement(const char* type) {
    return makeNode("LayerElement", {}, { makeNode("Type", { QByteArray(type) }), makeNode("TypedIndex", { 0 }) });
}

}

// What a colour map's pixels hold: the shares its alpha leaves nearly clear and nearly solid, and the shares that are
// pure black and nearly black. False when the image cannot be read.
struct ColourMap {
    bool hasAlpha { false };
    float clear { 0.0f };
    float solid { 0.0f };
    float black { 0.0f };
    float nearBlack { 0.0f };
};

static bool measureColourMap(const QString& path, ColourMap& measured) {
    QImage image(path);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return false;
    }
    measured.hasAlpha = image.hasAlphaChannel();
    image = image.convertToFormat(QImage::Format_ARGB32);
    qint64 clear = 0;
    qint64 solid = 0;
    qint64 black = 0;
    qint64 nearBlack = 0;
    for (int y = 0; y < image.height(); y++) {
        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x++) {
            const int alpha = qAlpha(line[x]);
            const int brightest = std::max({ qRed(line[x]), qGreen(line[x]), qBlue(line[x]) });
            clear += alpha < 32 ? 1 : 0;
            solid += alpha > 223 ? 1 : 0;
            black += brightest < 4 ? 1 : 0;
            nearBlack += brightest >= 4 && brightest < 16 ? 1 : 0;
        }
    }
    const double total = (double)image.width() * (double)image.height();
    measured.clear = (float)(clear / total);
    measured.solid = (float)(solid / total);
    measured.black = (float)(black / total);
    measured.nearBlack = (float)(nearBlack / total);
    return true;
}

// A copy of a colour map, as PNG, whose black (brightest channel under 8) is clear and everything else solid. The clear
// pixels then take the colour of the strands beside them, a few pixels out: the engine blends neighbouring pixels at a
// strand's edge, and black there showed as dark fringes around the hair (10-13).
static bool blackCutOut(const QString& path, QByteArray& png) {
    QImage image(path);
    if (image.isNull()) {
        return false;
    }
    image = image.convertToFormat(QImage::Format_ARGB32);
    const int width = image.width();
    const int height = image.height();
    std::vector<uint8_t> filled((size_t)width * height, 0);
    for (int y = 0; y < height; y++) {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; x++) {
            const bool solid = std::max({ qRed(line[x]), qGreen(line[x]), qBlue(line[x]) }) >= 8;
            line[x] = qRgba(qRed(line[x]), qGreen(line[x]), qBlue(line[x]), solid ? 255 : 0);
            filled[(size_t)y * width + x] = solid ? 1 : 0;
        }
    }
    // each pass gives every clear pixel next to a coloured one the average colour of its coloured neighbours
    for (int pass = 0; pass < 8; pass++) {
        std::vector<QRgb> colours;
        std::vector<size_t> where;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                if (filled[(size_t)y * width + x]) {
                    continue;
                }
                int red = 0, green = 0, blue = 0, count = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height || !filled[(size_t)ny * width + nx]) {
                            continue;
                        }
                        const QRgb c = reinterpret_cast<const QRgb*>(image.constScanLine(ny))[nx];
                        red += qRed(c);
                        green += qGreen(c);
                        blue += qBlue(c);
                        count++;
                    }
                }
                if (count > 0) {
                    colours.push_back(qRgba(red / count, green / count, blue / count, 0));
                    where.push_back((size_t)y * width + x);
                }
            }
        }
        if (where.empty()) {
            break;
        }
        for (size_t i = 0; i < where.size(); i++) {
            reinterpret_cast<QRgb*>(image.scanLine((int)(where[i] / width)))[where[i] % width] = colours[i];
            filled[where[i]] = 1;
        }
    }
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, "PNG");
}

// Where a texture's picture is, found the way game rips need: as written, relative to the model, by file name, then by
// file name in any case. Empty when it is not there.
static QString findTextureFile(const QString& written, const QDir& sourceFolder) {
    QString path = written;
    path.replace('\\', '/');
    const QString fileName = QFileInfo(path).fileName();
    for (const QString& candidate : { path, sourceFolder.filePath(path), sourceFolder.filePath(fileName) }) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isFile()) {
            return candidate;
        }
    }
    if (!fileName.isEmpty()) {
        for (const QString& entry : sourceFolder.entryList(QDir::Files)) {
            if (entry.compare(fileName, Qt::CaseInsensitive) == 0) {
                return sourceFolder.filePath(entry);
            }
        }
    }
    return QString();
}

// Whether the engine draws a model's textures upside down. Inside the engine V counts down from the picture's top edge
// (the FBX reader negates the file's V, which counts up from the bottom). Texture artists paint the two sides of every
// UV seam (an edge the mesh draws from two places in a texture) to match, so drawn the right way up the colours across
// a seam agree, and drawn upside down they mostly do not. Yes when the seams agree clearly better upside down; with too
// few seams to tell, or no clear difference, no. The note says what was measured. A file read right (Chloe's rip:
// differences 11.6 drawn, 33.1 flipped, 10-13) is kept.
static bool drawsUpsideDown(const hfm::Model& model, const QDir& sourceFolder, QString& note) {
    // each colour map once, by the texture's name in the model
    std::map<QString, int> pictureOf;
    std::vector<QImage> pictures;
    auto pictureIndex = [&](const hfm::Texture& texture) {
        if (texture.filename.isEmpty() && texture.content.isEmpty()) {
            return -1;
        }
        const QString key = QString::fromUtf8(texture.filename) + (texture.content.isEmpty() ? "" : "#embedded");
        auto known = pictureOf.find(key);
        if (known != pictureOf.end()) {
            return known->second;
        }
        QImage picture;
        if (!texture.content.isEmpty()) {
            picture = QImage::fromData(texture.content);
        } else {
            const QString path = findTextureFile(QString::fromUtf8(texture.filename), sourceFolder);
            if (!path.isEmpty()) {
                picture.load(path);
            }
        }
        int index = -1;
        if (!picture.isNull() && picture.width() > 8 && picture.height() > 8) {
            pictures.push_back(picture.convertToFormat(QImage::Format_RGB32));
            index = (int)pictures.size() - 1;
        }
        pictureOf[key] = index;
        return index;
    };
    // the average colour of the 3 by 3 pixels around a point, drawn as the engine draws it or upside down
    auto colourAt = [](const QImage& picture, const glm::vec2& uv, bool flipped) {
        const float u = uv.x - std::floor(uv.x);
        const float v = uv.y - std::floor(uv.y);
        const int x = glm::clamp((int)(u * picture.width()), 0, picture.width() - 1);
        const int y = glm::clamp((int)((flipped ? 1.0f - v : v) * picture.height()), 0, picture.height() - 1);
        glm::vec3 sum(0.0f);
        int count = 0;
        for (int dy = -1; dy <= 1; dy++) {
            const int row = y + dy;
            if (row < 0 || row >= picture.height()) {
                continue;
            }
            const QRgb* line = reinterpret_cast<const QRgb*>(picture.constScanLine(row));
            for (int dx = -1; dx <= 1; dx++) {
                const int column = x + dx;
                if (column >= 0 && column < picture.width()) {
                    sum += glm::vec3(qRed(line[column]), qGreen(line[column]), qBlue(line[column]));
                    count++;
                }
            }
        }
        return count > 0 ? sum / (float)count : sum;
    };

    // one side of an edge: its two ends' UVs (the lower-numbered corner first) and the third corner's, to step inside
    struct Side {
        glm::vec2 first;
        glm::vec2 second;
        glm::vec2 opposite;
    };
    const float AGREE = 16.0f;  // colours this close (0-255, averaged over red, green and blue) agree
    double differenceDrawn = 0.0;
    double differenceFlipped = 0.0;
    int agreeDrawn = 0;
    int agreeFlipped = 0;
    int samples = 0;
    int seams = 0;
    for (size_t m = 0; m < model.meshes.size(); m++) {
        const hfm::Mesh& mesh = model.meshes[m];
        const QVector<glm::vec2>& uvs = mesh.texCoords;
        if (uvs.size() != mesh.vertices.size()) {
            continue;
        }
        // the corners a seam splits share a position: one number per position
        std::map<std::array<float, 3>, int> numberOf;
        std::vector<int> corner(mesh.vertices.size());
        for (int v = 0; v < mesh.vertices.size(); v++) {
            const glm::vec3& p = mesh.vertices[v];
            corner[v] = numberOf.emplace(std::array<float, 3>{ { p.x, p.y, p.z } }, (int)numberOf.size()).first->second;
        }
        std::map<std::tuple<int, int, int>, std::vector<Side>> edges;  // (corner, corner, picture) to the sides drawn
        for (const hfm::Shape& shape : model.shapes) {
            if (shape.mesh != m || shape.meshPart >= mesh.parts.size() || shape.material >= model.materials.size()) {
                continue;
            }
            const int picture = pictureIndex(model.materials[shape.material].albedoTexture);
            if (picture < 0) {
                continue;
            }
            const hfm::MeshPart& part = mesh.parts[shape.meshPart];
            for (const QVector<int>* indices : { &part.triangleIndices, &part.quadTrianglesIndices }) {
                for (int i = 0; i + 2 < indices->size(); i += 3) {
                    const int t[3] = { indices->at(i), indices->at(i + 1), indices->at(i + 2) };
                    if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= uvs.size() || t[1] >= uvs.size() || t[2] >= uvs.size()) {
                        continue;
                    }
                    for (int e = 0; e < 3; e++) {
                        int a = t[e];
                        int b = t[(e + 1) % 3];
                        if (corner[a] == corner[b]) {
                            continue;
                        }
                        if (corner[a] > corner[b]) {
                            std::swap(a, b);
                        }
                        edges[std::make_tuple(corner[a], corner[b], picture)].push_back({ uvs[a], uvs[b], uvs[t[(e + 2) % 3]] });
                    }
                }
            }
        }
        for (const auto& entry : edges) {
            const std::vector<Side>& sides = entry.second;
            if (sides.size() != 2 || (glm::distance(sides[0].first, sides[1].first) < 1.0e-4f &&
                                      glm::distance(sides[0].second, sides[1].second) < 1.0e-4f)) {
                continue;  // not a seam: one side, or both drawn from the same place
            }
            const QImage& picture = pictures[std::get<2>(entry.first)];
            const glm::vec2 pixels(picture.width(), picture.height());
            seams++;
            for (const float along : { 0.25f, 0.5f, 0.75f }) {
                glm::vec2 points[2];
                for (int s = 0; s < 2; s++) {
                    // a point on the edge, stepped 2.5 pixels into its triangle, past any padding at the seam
                    const glm::vec2 onEdge = glm::mix(sides[s].first, sides[s].second, along);
                    const glm::vec2 inward = sides[s].opposite - onEdge;
                    const float length = glm::length(inward * pixels);
                    points[s] = length > 1.0e-6f ? onEdge + inward * std::min(2.5f / length, 0.5f) : onEdge;
                }
                const glm::vec3 asDrawn = glm::abs(colourAt(picture, points[0], false) - colourAt(picture, points[1], false));
                const glm::vec3 asFlipped = glm::abs(colourAt(picture, points[0], true) - colourAt(picture, points[1], true));
                const float drawn = (asDrawn.x + asDrawn.y + asDrawn.z) / 3.0f;
                const float flipped = (asFlipped.x + asFlipped.y + asFlipped.z) / 3.0f;
                differenceDrawn += drawn;
                differenceFlipped += flipped;
                agreeDrawn += drawn < AGREE ? 1 : 0;
                agreeFlipped += flipped < AGREE ? 1 : 0;
                samples++;
            }
        }
    }
    if (samples < 90) {
        note = seams > 0 ? QString("texture V kept: only %1 UV seams to read it from").arg(seams) : QString();
        return false;
    }
    const double meanDrawn = differenceDrawn / samples;
    const double meanFlipped = differenceFlipped / samples;
    const bool upsideDown = meanFlipped < 0.6 * meanDrawn && agreeFlipped >= agreeDrawn + samples / 10;
    note = QString("texture V %1: across %2 UV seams the colours differ by %3 as the engine draws them (%4% agree) and "
                   "%5 upside down (%6% agree)")
               .arg(upsideDown ? "flipped" : "kept").arg(seams).arg(meanDrawn, 0, 'f', 1).arg(100 * agreeDrawn / samples)
               .arg(meanFlipped, 0, 'f', 1).arg(100 * agreeFlipped / samples);
    return upsideDown;
}

WrittenAvatar autorig::writeAvatar(const hfm::Model& model, const Body& body, const Result& rig, const QString& sourcePath,
                                   const QString& folder, const QString& name, FlipV flipV) {
    WrittenAvatar written;
    if (!rig.ok || rig.joints.empty()) {
        written.error = "the character is not rigged";
        return written;
    }
    if (rig.weights.size() != body.vertices.size()) {
        written.error = "the skin weights do not match the body";
        return written;
    }
    QDir out(folder);
    if (!out.exists() && !QDir().mkpath(folder)) {
        written.error = "cannot make the folder " + folder;
        return written;
    }
    const QDir sourceFolder = QFileInfo(sourcePath).absoluteDir();
    bool flipTextures = flipV == FlipV::Always;
    if (flipV == FlipV::Detect) {
        QString note;
        flipTextures = drawsUpsideDown(model, sourceFolder, note);
        if (!note.isEmpty()) {
            written.notes << note;
        }
    }

    qint64 nextId = 1000000;
    auto newId = [&]() {
        return nextId++;
    };
    FBXNodeList objects;
    FBXNodeList connections;
    std::map<QByteArray, int> counts;  // for the Definitions block
    auto add = [&](const FBXNode& object) {
        objects << object;
        counts[object.name]++;
    };
    auto connect = [&](qint64 child, qint64 parent) {
        connections << makeNode("C", { QByteArray("OO"), child, parent });
    };
    auto connectTo = [&](qint64 child, qint64 parent, const char* slot) {
        connections << makeNode("C", { QByteArray("OP"), child, parent, QByteArray(slot) });
    };

    // the skeleton: each joint's place and axes relative to its parent, in centimetres and degrees
    const int jointCount = (int)rig.joints.size();
    std::vector<qint64> jointIds(jointCount);
    std::vector<glm::mat4> bind(jointCount);
    for (int j = 0; j < jointCount; j++) {
        bind[j] = glm::translate(glm::mat4(), rig.joints[j].position * CENTIMETRES_PER_METRE) *
                  glm::mat4_cast(rig.joints[j].rotation);
        jointIds[j] = newId();
    }
    for (int j = 0; j < jointCount; j++) {
        const RiggedJoint& joint = rig.joints[j];
        const int parent = joint.parent;
        const glm::mat4 local = parent >= 0 ? glm::inverse(bind[parent]) * bind[j] : bind[j];
        const glm::quat rotation =
            glm::normalize(parent >= 0 ? glm::inverse(rig.joints[parent].rotation) * joint.rotation : joint.rotation);
        const qint64 attribute = newId();
        add(makeNode("NodeAttribute", { attribute, objectName(joint.name, "NodeAttribute"), QByteArray("LimbNode") },
                     { makeNode("TypeFlags", { QByteArray("Skeleton") }) }));
        add(transformModel(jointIds[j], joint.name, "LimbNode", extractTranslation(local),
                           glm::degrees(safeEulerAngles(rotation))));
        connect(attribute, jointIds[j]);
        connect(jointIds[j], parent >= 0 ? jointIds[parent] : 0);
    }

    // textures: copied beside the FBX
    std::map<QString, QString> placed;
    auto placeTexture = [&](const hfm::Texture& texture) -> QString {
        QString path = QString::fromUtf8(texture.filename);
        path.replace('\\', '/');
        auto done = placed.find(path);
        if (done != placed.end()) {
            return done->second;
        }
        QString fileName = QFileInfo(path).fileName();
        if (!texture.content.isEmpty()) {
            if (fileName.isEmpty()) {
                fileName = QString("texture%1.png").arg(placed.size());
            }
            QFile file(out.filePath(fileName));
            if (!file.open(QIODevice::WriteOnly) || file.write(texture.content) != texture.content.size()) {
                return QString();
            }
        } else {
            const QString found = findTextureFile(path, sourceFolder);
            if (found.isEmpty()) {
                written.notes << "texture not found: " + path;
                placed[path] = QString();
                return QString();
            }
            fileName = QFileInfo(found).fileName();
            const QString target = out.filePath(fileName);
            if (QFileInfo(found).absoluteFilePath() != QFileInfo(target).absoluteFilePath()) {
                QFile::remove(target);
                QFile::copy(found, target);
            }
        }
        placed[path] = fileName;
        return fileName;
    };
    // The UV layer a texture names: the engine's reader only asks whether the name is "map1" or "UVSet0" and otherwise
    // takes the second layer (FBXSerializer_Material; its name matcher is never called), but 3ds Max names its first
    // layer "UVChannel_1". Chloe's textures all name it, so her normal maps were read as being on the second layer,
    // where the engine builds no tangents, and were left out (10-13). Exporters number their layers: 3ds Max's
    // UVChannel_N and Maya's mapN are layer N-1, UVSetN is layer N.
    auto layerOf = [](const hfm::Texture& texture) {
        const QString name = texture.texcoordSetName;
        static const QRegularExpression numbered("^(UVChannel_|map|UVSet)(\\d+)$", QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = numbered.match(name);
        if (match.hasMatch()) {
            const int n = match.captured(2).toInt();
            const int layer = match.captured(1).compare("UVSet", Qt::CaseInsensitive) == 0 ? n : n - 1;
            if (layer == 0 || layer == 1) {
                return layer;
            }
        }
        return texture.texcoordSet == 1 ? 1 : 0;
    };
    auto addTexture = [&](const hfm::Texture& texture, qint64 material, const char* slot) {
        if (texture.filename.isEmpty() && texture.content.isEmpty()) {
            return;
        }
        const QString fileName = placeTexture(texture);
        if (fileName.isEmpty()) {
            return;
        }
        const qint64 id = newId();
        // the reader gives a texture the mesh's UV layer of the same name (FBXSerializer::matchTextureUVSetToAttributeChannel)
        const QByteArray uvSet = layerOf(texture) == 1 ? SECOND_UV_SET : FIRST_UV_SET;
        add(makeNode("Texture", { id, objectName(fileName, "Texture"), QByteArray("") },
                     { makeNode("Type", { QByteArray("TextureVideoClip") }), makeNode("Version", { 202 }),
                       makeNode("TextureName", { objectName(fileName, "Texture") }),
                       makeNode("Properties70", {}, { property("UVSet", "KString", "", "", { uvSet }) }),
                       makeNode("FileName", { fileName.toUtf8() }), makeNode("RelativeFilename", { fileName.toUtf8() }),
                       makeNode("ModelUVTranslation", { 0.0, 0.0 }), makeNode("ModelUVScaling", { 1.0, 1.0 }),
                       makeNode("Texture_Alpha_Source", { QByteArray("None") }),
                       makeNode("Cropping", { 0, 0, 0, 0 }) }));
        connectTo(id, material, slot);
    };

    // materials, each written once, with the textures the engine reads back
    std::map<uint32_t, qint64> materialIds;
    auto materialFor = [&](uint32_t index) -> qint64 {
        auto done = materialIds.find(index);
        if (done != materialIds.end()) {
            return done->second;
        }
        const hfm::Material fallback;
        const hfm::Material& material = index < model.materials.size() ? model.materials[index] : fallback;
        const QString materialName = material.name.isEmpty() ? QString("material%1").arg(index) : material.name;
        // The engine makes a phong material as metallic as its specular colour's brightest channel, and multiplies the
        // colour map by the diffuse colour (FBXSerializer_Material, MaterialTextures.slh). A game rip's specular 0.9 made
        // Chloe 90% metal, black wherever no bright reflection fell, and its default grey diffuse 0.59 darkened her
        // textures (Caity's screenshot, 10-13). An avatar's skin, hair and cloth are not metal: its specular is black,
        // its diffuse white under a colour map, and its shininess goes under "Shininess", the name the reader parses.
        const bool colourMapped = !material.albedoTexture.filename.isEmpty() || !material.albedoTexture.content.isEmpty();
        const qint64 id = newId();
        add(makeNode("Material", { id, objectName(materialName, "Material"), QByteArray("") },
                     { makeNode("Version", { 102 }), makeNode("ShadingModel", { QByteArray("phong") }),
                       makeNode("MultiLayer", { 0 }),
                       makeNode("Properties70", {},
                                { property("DiffuseColor", "Color", "", "A",
                                           vector3(colourMapped ? glm::vec3(1.0f) : material.diffuseColor)),
                                  property("DiffuseFactor", "Number", "", "A", { 1.0 }),
                                  property("SpecularColor", "Color", "", "A", vector3(glm::vec3(0.0f))),
                                  property("Shininess", "Number", "", "A", { (double)material.shininess }),
                                  property("ShininessExponent", "Number", "", "A", { (double)material.shininess }),
                                  property("EmissiveColor", "Color", "", "A", vector3(material.emissiveColor)),
                                  property("Opacity", "Number", "", "A", { (double)material.opacity }) }) }));
        // A colour map that cuts the shape out is the opacity map too: the engine reads a colour map's alpha only when the
        // same map is linked as the opacity map (ProceduralMaterialCache). Its alpha may cut the shape out (a game's
        // alpha-tested hair kept the mask there); or, for hair whose alpha is solid, a pure black background between the
        // strands does (Chloe's: 55% black, 1% near black), and a copy with that black made clear is written. A skin map
        // keeping a shine mask in its alpha has no large clear part, and only hair has its black cut out.
        hfm::Texture colour = material.albedoTexture;
        hfm::Texture opacity = material.opacityTexture;
        const bool hasColourMap = !colour.filename.isEmpty() || !colour.content.isEmpty();
        if (opacity.filename.isEmpty() && opacity.content.isEmpty() && hasColourMap) {
            const QString colourMap = placeTexture(colour);
            ColourMap measured;
            if (!colourMap.isEmpty() && measureColourMap(out.filePath(colourMap), measured)) {
                const bool hair = materialName.contains("hair", Qt::CaseInsensitive) ||
                                  colourMap.contains("hair", Qt::CaseInsensitive);
                QByteArray png;
                if (measured.hasAlpha && measured.clear >= 0.10f && measured.solid >= 0.10f) {
                    opacity = colour;
                    written.notes << QString("%1: %2's alpha cuts it out (%3% clear), so it is the opacity map too")
                                         .arg(materialName, colourMap).arg(measured.clear * 100.0f, 0, 'f', 0);
                } else if (hair && measured.clear < 0.01f && measured.black >= 0.25f && measured.nearBlack < 0.03f &&
                           blackCutOut(out.filePath(colourMap), png)) {
                    colour.filename = (QFileInfo(colourMap).completeBaseName() + "_cutout.png").toUtf8();
                    colour.content = png;
                    opacity = colour;
                    written.notes << QString("%1: %2 is %3% pure black between the strands, so %4 has that black clear")
                                         .arg(materialName, colourMap).arg(measured.black * 100.0f, 0, 'f', 0)
                                         .arg(QString::fromUtf8(colour.filename));
                } else if (hair) {
                    written.notes << QString("%1: no cut-out in %2 (%3% clear, %4% black), so the hair is drawn solid")
                                         .arg(materialName, colourMap).arg(measured.clear * 100.0f, 0, 'f', 0)
                                         .arg(measured.black * 100.0f, 0, 'f', 0);
                }
            }
        }
        addTexture(colour, id, "DiffuseColor");
        // The engine builds a normal map's tangents from the first UV layer (CalculateMeshTangentsTask), so a normal map
        // on another layer is shaded in the wrong frame: Chloe's (game rips keep every texture on the second layer) made
        // a faceted honeycomb over her face. Such a normal map is left out. Writing the first layer as a copy of the
        // second instead, or swapping them, turned her untextured a moment after her textures appeared (10-13).
        if (layerOf(material.normalTexture) == 0) {
            addTexture(material.normalTexture, id, "NormalMap");
        } else if (!material.normalTexture.filename.isEmpty() || !material.normalTexture.content.isEmpty()) {
            written.notes << QString("%1: its normal map is on UV layer %2, whose tangents the engine does not build: left out")
                                 .arg(materialName).arg(layerOf(material.normalTexture) + 1);
        }
        addTexture(opacity, id, "TransparentColor");
        addTexture(material.specularTexture, id, "SpecularColor");
        addTexture(material.glossTexture, id, "ShininessExponent");
        addTexture(material.emissiveTexture, id, "EmissiveColor");
        addTexture(material.roughnessTexture, id, "tex_roughness_map");
        addTexture(material.metallicTexture, id, "tex_metallic_map");
        materialIds[index] = id;
        return id;
    };

    // the meshes, in centimetres, each with its materials and its skin
    std::vector<qint64> meshIds;
    int pieceCount = 0;
    int blendshapeCount = 0;
    for (const Body::Piece& piece : body.pieces) {
        const hfm::Mesh& mesh = model.meshes[piece.mesh];
        const glm::mat3 normalTransform = glm::mat3(body.adjustment * piece.transform);
        QVector<double> vertices;
        vertices.reserve((int)piece.vertexCount * 3);
        for (uint32_t v = 0; v < piece.vertexCount; v++) {
            const glm::vec3 p = body.vertices[piece.firstVertex + v] * CENTIMETRES_PER_METRE;
            vertices << p.x << p.y << p.z;
        }
        QVector<qint32> polygons;
        QVector<qint32> polygonMaterials;
        QVector<qint32> corners;  // the vertex at each polygon corner
        std::vector<uint32_t> pieceMaterials;
        for (uint32_t shapeIndex : piece.shapes) {
            const hfm::Shape& shape = model.shapes[shapeIndex];
            const hfm::MeshPart& part = mesh.parts[shape.meshPart];
            auto known = std::find(pieceMaterials.begin(), pieceMaterials.end(), shape.material);
            const int localMaterial = (int)(known - pieceMaterials.begin());
            if (known == pieceMaterials.end()) {
                pieceMaterials.push_back(shape.material);
            }
            for (const QVector<int>* indices : { &part.triangleIndices, &part.quadTrianglesIndices }) {
                for (int i = 0; i + 2 < indices->size(); i += 3) {
                    const int a = indices->at(i), b = indices->at(i + 1), c = indices->at(i + 2);
                    if (a < 0 || b < 0 || c < 0 || a >= (int)piece.vertexCount || b >= (int)piece.vertexCount ||
                        c >= (int)piece.vertexCount) {
                        continue;
                    }
                    polygons << a << b << (-c - 1);
                    corners << a << b << c;
                    polygonMaterials << localMaterial;
                }
            }
        }
        if (polygons.isEmpty()) {
            continue;
        }
        FBXNodeList geometry { makeNode("Vertices", { doubles(vertices) }), makeNode("PolygonVertexIndex", { ints(polygons) }),
                               makeNode("GeometryVersion", { 124 }) };
        FBXNodeList layer { makeNode("Version", { 100 }) };
        if (mesh.normals.size() == mesh.vertices.size()) {
            QVector<double> normals;
            normals.reserve(corners.size() * 3);
            for (qint32 corner : corners) {
                const glm::vec3 n = glm::normalize(normalTransform * mesh.normals[corner]);
                normals << n.x << n.y << n.z;
            }
            geometry << makeNode("LayerElementNormal", { 0 },
                                 { makeNode("Version", { 102 }), makeNode("Name", { QByteArray("") }),
                                   makeNode("MappingInformationType", { QByteArray("ByPolygonVertex") }),
                                   makeNode("ReferenceInformationType", { QByteArray("Direct") }),
                                   makeNode("Normals", { doubles(normals) }) });
            layer << layerElement("LayerElementNormal");
        }
        // The reader negates V as it reads (FBXSerializer::createVec2Vector), so V is written negated back. The writer
        // wrote it as read until 10-13, which drew every rigged avatar's textures upside down: Chloe's eyes from her
        // painted chin, her jacket from her skin.
        const QVector<glm::vec2>& firstUVs = mesh.texCoords;
        const QVector<glm::vec2>& secondUVs = mesh.texCoords1;
        if (firstUVs.size() == mesh.vertices.size()) {
            QVector<double> uvs;
            uvs.reserve(firstUVs.size() * 2);
            for (const glm::vec2& uv : firstUVs) {
                uvs << uv.x << -(flipTextures ? 1.0f - uv.y : uv.y);
            }
            geometry << makeNode("LayerElementUV", { 0 },
                                 { makeNode("Version", { 101 }), makeNode("Name", { FIRST_UV_SET }),
                                   makeNode("MappingInformationType", { QByteArray("ByPolygonVertex") }),
                                   makeNode("ReferenceInformationType", { QByteArray("IndexToDirect") }),
                                   makeNode("UV", { doubles(uvs) }), makeNode("UVIndex", { ints(corners) }) });
            layer << layerElement("LayerElementUV");
        }
        // a second UV layer: game rips often keep their textures on it (Chloe's all use "UVChannel_1")
        FBXNodeList secondLayer;
        if (secondUVs.size() == mesh.vertices.size()) {
            QVector<double> uvs;
            uvs.reserve(secondUVs.size() * 2);
            for (const glm::vec2& uv : secondUVs) {
                uvs << uv.x << -(flipTextures ? 1.0f - uv.y : uv.y);
            }
            geometry << makeNode("LayerElementUV", { 1 },
                                 { makeNode("Version", { 101 }), makeNode("Name", { SECOND_UV_SET }),
                                   makeNode("MappingInformationType", { QByteArray("ByPolygonVertex") }),
                                   makeNode("ReferenceInformationType", { QByteArray("IndexToDirect") }),
                                   makeNode("UV", { doubles(uvs) }), makeNode("UVIndex", { ints(corners) }) });
            secondLayer << makeNode("Version", { 100 })
                        << makeNode("LayerElement", {}, { makeNode("Type", { QByteArray("LayerElementUV") }),
                                                         makeNode("TypedIndex", { 1 }) });
        }
        geometry << makeNode("LayerElementMaterial", { 0 },
                             { makeNode("Version", { 101 }), makeNode("Name", { QByteArray("") }),
                               makeNode("MappingInformationType", { QByteArray("ByPolygon") }),
                               makeNode("ReferenceInformationType", { QByteArray("IndexToDirect") }),
                               makeNode("Materials", { ints(polygonMaterials) }) });
        layer << layerElement("LayerElementMaterial");
        geometry << makeNode("Layer", { 0 }, layer);
        if (!secondLayer.isEmpty()) {
            geometry << makeNode("Layer", { 1 }, secondLayer);
        }

        QString meshName = model.getModelNameOfMesh((int)piece.mesh);
        if (meshName.isEmpty()) {
            meshName = QString("body%1").arg(pieceCount);
        }
        const qint64 geometryId = newId();
        const qint64 meshId = newId();
        add(makeNode("Geometry", { geometryId, objectName(meshName, "Geometry"), QByteArray("Mesh") }, geometry));
        add(transformModel(meshId, meshName, "Mesh", glm::vec3(0.0f), glm::vec3(0.0f)));
        connect(geometryId, meshId);
        connect(meshId, 0);
        for (uint32_t material : pieceMaterials) {
            connect(materialFor(material), meshId);  // the polygons' material numbers count these in this order
        }
        meshIds.push_back(meshId);

        // the skin: a cluster for each joint that moves part of this mesh
        std::map<int, std::pair<QVector<qint32>, QVector<double>>> clusters;
        for (uint32_t v = 0; v < piece.vertexCount; v++) {
            const VertexWeights& weights = rig.weights[piece.firstVertex + v];
            for (int k = 0; k < 4; k++) {
                if (weights.weights[k] > 0.0f) {
                    auto& cluster = clusters[weights.joints[k]];
                    cluster.first << (qint32)v;
                    cluster.second << (double)weights.weights[k];
                }
            }
        }
        const qint64 skinId = newId();
        add(makeNode("Deformer", { skinId, objectName("Skin " + meshName, "Deformer"), QByteArray("Skin") },
                     { makeNode("Version", { 101 }), makeNode("Link_DeformAcuracy", { 50.0 }) }));
        connect(skinId, geometryId);
        for (const auto& cluster : clusters) {
            const int j = cluster.first;
            const qint64 clusterId = newId();
            add(makeNode("Deformer",
                         { clusterId, objectName("Cluster " + rig.joints[j].name, "SubDeformer"), QByteArray("Cluster") },
                         { makeNode("Version", { 100 }), makeNode("UserData", { QByteArray(""), QByteArray("") }),
                           makeNode("Indexes", { ints(cluster.second.first) }),
                           makeNode("Weights", { doubles(cluster.second.second) }),
                           makeNode("Transform", { doubles(matrixValues(glm::mat4())) }),
                           makeNode("TransformLink", { doubles(matrixValues(bind[j])) }) }));
            connect(clusterId, skinId);
            connect(jointIds[j], clusterId);
        }

        // its face shapes and other blendshapes, as the model was read (BLENDSHAPE_NAMES' order), each under Darkride's
        // name for it, which the reader takes as it is: a Shape for each, through a channel, to the mesh's BlendShape
        const glm::mat4 meshToBody = body.adjustment * piece.transform;
        const glm::mat3 offsetTransform = glm::mat3(meshToBody) * CENTIMETRES_PER_METRE;
        const glm::mat3 normalRotation = glm::mat3_cast(glmExtractRotation(meshToBody));
        qint64 blendShapeId = 0;
        for (int b = 0; b < (int)mesh.blendshapes.size() && b < (int)Blendshapes::BlendshapeCount; b++) {
            const hfm::Blendshape& blendshape = mesh.blendshapes[b];
            QVector<qint32> indexes;
            QVector<double> offsets;
            QVector<double> normalOffsets;  // the reader needs one for every index
            for (int k = 0; k < blendshape.indices.size() && k < blendshape.vertices.size(); k++) {
                const int v = blendshape.indices[k];
                if (v < 0 || v >= (int)piece.vertexCount) {
                    continue;
                }
                const glm::vec3 offset = offsetTransform * blendshape.vertices[k];
                const glm::vec3 normal = k < blendshape.normals.size() ? normalRotation * blendshape.normals[k] : glm::vec3(0.0f);
                indexes << v;
                offsets << offset.x << offset.y << offset.z;
                normalOffsets << normal.x << normal.y << normal.z;
            }
            if (indexes.isEmpty()) {
                continue;
            }
            if (blendShapeId == 0) {
                blendShapeId = newId();
                add(makeNode("Deformer", { blendShapeId, objectName("Morpher " + meshName, "Deformer"), QByteArray("BlendShape") },
                             { makeNode("Version", { 100 }) }));
                connect(blendShapeId, geometryId);
            }
            const QString name = BLENDSHAPE_NAMES[b];
            const qint64 channelId = newId();
            const qint64 shapeId = newId();
            add(makeNode("Deformer", { channelId, objectName(name, "SubDeformer"), QByteArray("BlendShapeChannel") },
                         { makeNode("Version", { 100 }), makeNode("DeformPercent", { 0.0 }),
                           makeNode("FullWeights", { doubles({ 100.0 }) }) }));
            add(makeNode("Geometry", { shapeId, objectName(name, "Geometry"), QByteArray("Shape") },
                         { makeNode("Version", { 100 }), makeNode("Indexes", { ints(indexes) }),
                           makeNode("Vertices", { doubles(offsets) }), makeNode("Normals", { doubles(normalOffsets) }) }));
            connect(channelId, blendShapeId);
            connect(shapeId, channelId);
            blendshapeCount++;
        }
        pieceCount++;
    }

    // the bind pose: every joint and mesh as placed
    FBXNodeList pose { makeNode("Type", { QByteArray("BindPose") }), makeNode("Version", { 100 }),
                       makeNode("NbPoseNodes", { jointCount + (int)meshIds.size() }) };
    for (int j = 0; j < jointCount; j++) {
        pose << makeNode("PoseNode", {}, { makeNode("Node", { jointIds[j] }), makeNode("Matrix", { doubles(matrixValues(bind[j])) }) });
    }
    for (qint64 meshId : meshIds) {
        pose << makeNode("PoseNode", {}, { makeNode("Node", { meshId }), makeNode("Matrix", { doubles(matrixValues(glm::mat4())) }) });
    }
    add(makeNode("Pose", { newId(), objectName("BindPose", "Pose"), QByteArray("BindPose") }, pose));

    FBXNodeList definitions { makeNode("Version", { 100 }), makeNode("Count", { (int)objects.size() + 1 }),
                              makeNode("ObjectType", { QByteArray("GlobalSettings") }, { makeNode("Count", { 1 }) }) };
    for (const auto& count : counts) {
        definitions << makeNode("ObjectType", { count.first }, { makeNode("Count", { count.second }) });
    }
    FBXNode root;
    root.children << makeNode("FBXHeaderExtension", {},
                              { makeNode("FBXHeaderVersion", { 1003 }), makeNode("FBXVersion", { 7400 }),
                                makeNode("Creator", { QByteArray("Darkride auto-rigger") }) });
    root.children << makeNode("GlobalSettings", {},
                              { makeNode("Version", { 1000 }),
                                makeNode("Properties70", {},
                                         { property("UpAxis", "int", "Integer", "", { 1 }),
                                           property("UpAxisSign", "int", "Integer", "", { 1 }),
                                           property("FrontAxis", "int", "Integer", "", { 2 }),
                                           property("FrontAxisSign", "int", "Integer", "", { 1 }),
                                           property("CoordAxis", "int", "Integer", "", { 0 }),
                                           property("CoordAxisSign", "int", "Integer", "", { 1 }),
                                           property("OriginalUpAxis", "int", "Integer", "", { 1 }),
                                           property("OriginalUpAxisSign", "int", "Integer", "", { 1 }),
                                           property("UnitScaleFactor", "double", "Number", "", { 1.0 }),
                                           property("OriginalUnitScaleFactor", "double", "Number", "", { 1.0 }) }) });
    root.children << makeNode("Definitions", {}, definitions);
    root.children << makeNode("Objects", {}, objects);
    root.children << makeNode("Connections", {}, connections);

    written.fbxPath = out.filePath(name + ".fbx");
    QFile fbx(written.fbxPath);
    const QByteArray data = FBXWriter::encodeFBX(root);
    if (!fbx.open(QIODevice::WriteOnly) || fbx.write(data) != data.size()) {
        written.error = "cannot write " + written.fbxPath;
        return written;
    }
    fbx.close();

    written.fstPath = out.filePath(name + ".fst");
    if (!writeFst(written.fstPath, name, name + ".fbx")) {
        written.error = "cannot write " + written.fstPath;
        return written;
    }

    written.notes << QString("wrote %1: %2 joints, %3 meshes, %4 materials, %5 textures, %6 blendshapes, %7 KB")
                         .arg(QFileInfo(written.fbxPath).fileName()).arg(jointCount).arg(meshIds.size())
                         .arg(materialIds.size()).arg(counts["Texture"]).arg(blendshapeCount).arg(data.size() / 1024);
    written.ok = true;
    return written;
}

bool autorig::writeFst(const QString& fstPath, const QString& name, const QString& modelFilename, float scale) {
    QFile fst(fstPath);
    if (!fst.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream text(&fst);
    text << "name = " << name << "\n"
         << "type = body+head\n"
         << "scale = " << QString::number(scale, 'g', 6) << "\n"
         << "filename = " << modelFilename << "\n"
         << "joint = jointRoot = Hips\n"
         << "joint = jointLean = Spine\n"
         << "joint = jointNeck = Neck\n"
         << "joint = jointHead = Head\n"
         << "joint = jointEyeLeft = LeftEye\n"
         << "joint = jointEyeRight = RightEye\n"
         << "joint = jointLeftHand = LeftHand\n"
         << "joint = jointRightHand = RightHand\n";
    return text.status() == QTextStream::Ok;
}
