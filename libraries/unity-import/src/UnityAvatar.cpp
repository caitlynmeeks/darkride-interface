//
//  UnityAvatar.cpp
//  libraries/unity-import/src
//
//  Created on 2026-10-14 for Darkride's import of avatars from Unity packages.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "UnityAvatar.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <BlendshapeConstants.h>
#include <FBXSerializer.h>
#include <Humanoid.h>

#include "AvatarMenuImport.h"
#include "FaceShapes.h"
#include "PrefabOverrides.h"
#include "UnityHumanoid.h"
#include "UnityIds.h"
#include "UnityPackage.h"
#include "UnitySpringBones.h"
#include "UnityTransforms.h"
#include "UnityYaml.h"

namespace avatarimport {

namespace {

const unity::YamlObject* firstObject(const unity::Package& package, const QString& guid, int classID) {
    for (const auto& object : package.objects(guid)) {
        if (object.classID == classID && !object.stripped) {
            return &object;
        }
    }
    return nullptr;
}

int toInt(const QVariant& value) {
    return (int)unity::toNumber(value, 0.0);
}

// The model a prefab's object comes from, following stripped placeholders through nested prefabs.
QString modelOf(const unity::Package& package, const QString& guid, qint64 objectID, int depth = 0) {
    const auto* asset = package.byGuid(guid);
    if (!asset || depth > 8) {
        return QString();
    }
    if (asset->path.endsWith(".fbx", Qt::CaseInsensitive)) {
        return guid;
    }
    for (const auto& object : package.objects(guid)) {
        if (object.fileID == objectID) {
            if (!object.stripped) {
                return QString();
            }
            const QVariant source = object.fields.value("m_CorrespondingSourceObject");
            return modelOf(package, unity::guidOf(source), unity::fileIDOf(source), depth + 1);
        }
    }
    return QString();
}

// Copies textures from the package into the avatar's folder once each, beside the .fst, and names them as the .fst
// does (File > Put Avatar in My Files sends up every file the .fst names, into the same folder).
class Textures {
public:
    Textures(const unity::Package& package, const QDir& folder) : _package(package), _folder(folder) {}

    QString copy(const QString& guid, QStringList& notes) {
        if (guid.isEmpty()) {
            return QString();
        }
        auto found = _copied.find(guid);
        if (found != _copied.end()) {
            return *found;
        }
        QString relative;
        const auto* asset = _package.byGuid(guid);
        if (!asset || !asset->hasData) {
            notes << QString("a material names a texture that is not in the package (%1)").arg(guid.left(8));
        } else {
            const QFileInfo info(asset->path);
            const QString suffix = info.suffix().toLower();
            static const QStringList READABLE { "png", "jpg", "jpeg", "tga", "bmp" };
            if (!READABLE.contains(suffix)) {
                notes << QString("%1: the engine cannot read .%2 textures yet").arg(info.fileName(), suffix);
            } else {
                QString fileName = info.fileName();
                for (int n = 2; _used.contains(fileName.toLower()); n++) {
                    fileName = QString("%1-%2.%3").arg(info.completeBaseName()).arg(n).arg(suffix);
                }
                _used.insert(fileName.toLower());
                QFile file(_folder.filePath(fileName));
                if (file.open(QIODevice::WriteOnly) && file.write(asset->data) == asset->data.size()) {
                    relative = fileName;
                } else {
                    notes << "could not write " + file.fileName();
                }
            }
        }
        _copied.insert(guid, relative);
        return relative;
    }

    // A texture multiplied by a mask, as lilToon multiplies its emission by _EmissionBlendMask; written once.
    QString bakeMasked(const QString& guid, const QString& maskGuid, QStringList& notes) {
        const QString key = guid + "*" + maskGuid;
        auto found = _copied.find(key);
        if (found != _copied.end()) {
            return *found;
        }
        QString relative;
        const auto* asset = _package.byGuid(guid);
        const auto* maskAsset = _package.byGuid(maskGuid);
        QImage image = asset ? QImage::fromData(asset->data).convertToFormat(QImage::Format_ARGB32) : QImage();
        QImage mask = maskAsset ? QImage::fromData(maskAsset->data) : QImage();
        if (image.isNull() || mask.isNull()) {
            notes << "could not read an emission texture or its mask; emission left unmasked";
            relative = copy(guid, notes);
        } else {
            mask = mask.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < image.height(); y++) {
                QRgb* row = reinterpret_cast<QRgb*>(image.scanLine(y));
                const QRgb* maskRow = reinterpret_cast<const QRgb*>(mask.constScanLine(y));
                for (int x = 0; x < image.width(); x++) {
                    row[x] = qRgba(qRed(row[x]) * qRed(maskRow[x]) / 255, qGreen(row[x]) * qGreen(maskRow[x]) / 255,
                                   qBlue(row[x]) * qBlue(maskRow[x]) / 255, 255);
                }
            }
            QString fileName = QFileInfo(asset->path).completeBaseName() + "_emission.png";
            for (int n = 2; _used.contains(fileName.toLower()); n++) {
                fileName = QString("%1_emission-%2.png").arg(QFileInfo(asset->path).completeBaseName()).arg(n);
            }
            _used.insert(fileName.toLower());
            if (image.save(_folder.filePath(fileName), "PNG")) {
                relative = fileName;
            } else {
                notes << "could not write " + fileName;
            }
        }
        _copied.insert(key, relative);
        return relative;
    }

private:
    const unity::Package& _package;
    QDir _folder;
    QHash<QString, QString> _copied;
    QSet<QString> _used;
};

QJsonArray rgb(const QVariant& color) {
    const QVariantMap c = color.toMap();
    return QJsonArray { unity::toNumber(c.value("r"), 1.0), unity::toNumber(c.value("g"), 1.0),
                        unity::toNumber(c.value("b"), 1.0) };
}

// A stand-in for a Unity material in the engine's own material: its colour texture and colour, how its alpha is
// used, which faces are drawn, emission and normal map. Toon shaders (lilToon, Poiyomi) are drawn matte for now.
QJsonObject standIn(const unity::Package& package, const QString& materialGuid, Textures& textures, QStringList& notes,
                    QString& family) {
    const unity::YamlObject* material = firstObject(package, materialGuid, 21);
    if (!material) {
        return QJsonObject();
    }
    const QVariantMap saved = material->fields.value("m_SavedProperties").toMap();
    const QVariantMap floats = unity::mergeKeyList(saved.value("m_Floats"));
    const QVariantMap colors = unity::mergeKeyList(saved.value("m_Colors"));
    const QVariantMap maps = unity::mergeKeyList(saved.value("m_TexEnvs"));
    auto number = [&](const char* key, double fallback) {
        return floats.contains(key) ? unity::toNumber(floats.value(key), fallback) : fallback;
    };
    auto map = [&](const char* key) {
        return textures.copy(unity::guidOf(maps.value(key).toMap().value("m_Texture")), notes);
    };

    const bool lilToon = floats.contains("_lilToonVersion");
    bool poiyomi = floats.contains("_ShaderOptimizerEnabled");
    for (auto it = floats.begin(); !poiyomi && it != floats.end(); ++it) {
        poiyomi = it.key().startsWith("shader_");
    }
    family = lilToon ? "lilToon" : poiyomi ? "Poiyomi" : "other";

    QJsonObject m;
    m["model"] = "hifi_pbr";
    const QString albedoMap = map("_MainTex");
    if (!albedoMap.isEmpty()) {
        m["albedoMap"] = albedoMap;
    }
    if (colors.contains("_Color")) {
        const QJsonArray color = rgb(colors.value("_Color"));
        if (color[0].toDouble() < 0.999 || color[1].toDouble() < 0.999 || color[2].toDouble() < 0.999) {
            m["albedo"] = color;
        }
        const double alpha = unity::toNumber(colors.value("_Color").toMap().value("a"), 1.0);
        if (alpha < 0.999) {
            m["opacity"] = alpha;
        }
    }
    const int queue = toInt(material->fields.value("m_CustomRenderQueue"));  // -1: the shader's own
    if (number("_DstBlend", 0.0) == 10.0) {  // OneMinusSrcAlpha: blended transparency
        m["opacityMapMode"] = "OPACITY_MAP_BLEND";
    } else if (queue >= 2450 && queue < 3000) {  // the alpha-test queue: cut out
        m["opacityMapMode"] = "OPACITY_MAP_MASK";
        m["opacityCutoff"] = number("_Cutoff", 0.5);
    } else {
        m["opacityMapMode"] = "OPACITY_MAP_OPAQUE";
    }
    if (!albedoMap.isEmpty() && m["opacityMapMode"].toString() != "OPACITY_MAP_OPAQUE") {
        m["opacityMap"] = albedoMap;  // the engine takes the colour map's alpha only when it is also the opacity map
    }
    const int cull = (int)number("_Cull", 2.0);
    m["cullFaceMode"] = cull == 0 ? "CULL_NONE" : cull == 1 ? "CULL_FRONT" : "CULL_BACK";
    if (number("_UseEmission", 0.0) == 1.0 || number("_EnableEmission", 0.0) == 1.0) {
        // lilToon multiplies the emission by a blend mask; the engine has one emissive map, so the two are baked.
        const QString emissionGuid = unity::guidOf(maps.value("_EmissionMap").toMap().value("m_Texture"));
        const QString maskGuid = unity::guidOf(maps.value("_EmissionBlendMask").toMap().value("m_Texture"));
        const QString emissiveMap = !emissionGuid.isEmpty() && !maskGuid.isEmpty()
            ? textures.bakeMasked(emissionGuid, maskGuid, notes) : map("_EmissionMap");
        if (!emissiveMap.isEmpty()) {
            m["emissiveMap"] = emissiveMap;
        }
        if (colors.contains("_EmissionColor")) {
            m["emissive"] = rgb(colors.value("_EmissionColor"));
        }
    }
    if (number("_UseBumpMap", lilToon ? 0.0 : 1.0) == 1.0) {
        const QString normalMap = map("_BumpMap");
        if (!normalMap.isEmpty()) {
            m["normalMap"] = normalMap;
        }
    }
    if (lilToon || poiyomi) {
        m["roughness"] = 1.0;
        m["metallic"] = 0.0;
    } else {
        m["roughness"] = 1.0 - number("_Glossiness", 0.0);
        m["metallic"] = number("_Metallic", 0.0);
    }
    return QJsonObject { { "materialVersion", 1 }, { "materials", m } };
}

// What the effects layer shows and hides before anyone touches the menu: each layer from its default state, following
// transitions whose conditions the parameters' starting values meet, then that state's clip at its first frame.
// A first slice of the animator; the full one runs live in the avatar later.
struct StartingState {
    QHash<QString, bool> active;   // object path -> shown
    QHash<QString, float> shapes;  // "path:shape key" -> weight (0-100)
    // object path -> local transform in Unity's frame (meters), where a clip moves, turns or scales it
    QHash<QString, glm::vec3> positions;
    QHash<QString, glm::quat> rotations;
    QHash<QString, glm::vec3> scales;
};

StartingState startingState(const unity::Package& package, const QVariantMap& descriptor) {
    StartingState state;
    QString effectsController;
    for (const QVariant& entry : descriptor.value("baseAnimationLayers").toList()) {
        const QVariantMap layer = entry.toMap();
        if (toInt(layer.value("type")) == 5 && toInt(layer.value("isDefault")) == 0) {
            effectsController = unity::guidOf(layer.value("animatorController"));
        }
    }
    const unity::YamlObject* controller = effectsController.isEmpty() ? nullptr : firstObject(package, effectsController, 91);
    if (!controller) {
        return state;
    }

    // The built-in parameters as they stand for someone standing still, then the controller's and the menu's.
    QHash<QString, double> values { { "IsLocal", 1 }, { "Grounded", 1 }, { "Upright", 1 }, { "TrackingType", 1 },
                                    { "AvatarVersion", 3 }, { "IsAnimatorEnabled", 1 } };
    for (const QVariant& entry : controller->fields.value("m_AnimatorParameters").toList()) {
        const QVariantMap parameter = entry.toMap();
        const QString name = parameter.value("m_Name").toString();
        if (values.contains(name)) {
            continue;
        }
        const int type = toInt(parameter.value("m_Type"));
        values[name] = type == 1 ? unity::toNumber(parameter.value("m_DefaultFloat"))
                     : type == 3 ? unity::toNumber(parameter.value("m_DefaultInt"))
                     : type == 4 ? unity::toNumber(parameter.value("m_DefaultBool")) : 0.0;
    }
    const QString parametersGuid = unity::guidOf(descriptor.value("expressionParameters"));
    if (const unity::YamlObject* parameters = parametersGuid.isEmpty() ? nullptr : firstObject(package, parametersGuid, 114)) {
        for (const QVariant& entry : parameters->fields.value("parameters").toList()) {
            const QVariantMap parameter = entry.toMap();
            values[parameter.value("name").toString()] = unity::toNumber(parameter.value("defaultValue"));
        }
    }

    QHash<qint64, const unity::YamlObject*> byID;
    for (const auto& object : package.objects(effectsController)) {
        byID.insert(object.fileID, &object);
    }
    auto conditionsHold = [&](const QVariantList& conditions) {
        for (const QVariant& entry : conditions) {
            const QVariantMap condition = entry.toMap();
            const double value = values.value(condition.value("m_ConditionEvent").toString(), 0.0);
            const double threshold = unity::toNumber(condition.value("m_EventTreshold"));
            bool holds = false;
            switch (toInt(condition.value("m_ConditionMode"))) {
                case 1: holds = value != 0.0; break;        // If
                case 2: holds = value == 0.0; break;        // IfNot
                case 3: holds = value > threshold; break;   // Greater
                case 4: holds = value < threshold; break;   // Less
                case 6: holds = value == threshold; break;  // Equals
                case 7: holds = value != threshold; break;  // NotEqual
                default: break;
            }
            if (!holds) {
                return false;
            }
        }
        return true;
    };

    for (const QVariant& entry : controller->fields.value("m_AnimatorLayers").toList()) {
        const QVariantMap layer = entry.toMap();
        if (unity::toNumber(layer.value("m_DefaultWeight"), 1.0) <= 0.0) {
            continue;
        }
        const unity::YamlObject* machine = byID.value(unity::fileIDOf(layer.value("m_StateMachine")));
        if (!machine) {
            continue;
        }
        qint64 current = unity::fileIDOf(machine->fields.value("m_DefaultState"));
        QSet<qint64> visited { current };
        for (int hop = 0; hop < 4 && current; hop++) {
            QVariantList candidates = machine->fields.value("m_AnyStateTransitions").toList();
            if (const unity::YamlObject* here = byID.value(current)) {
                candidates += here->fields.value("m_Transitions").toList();
            }
            qint64 next = 0;
            for (const QVariant& reference : candidates) {
                const unity::YamlObject* transition = byID.value(unity::fileIDOf(reference));
                if (!transition) {
                    continue;
                }
                const QVariantList conditions = transition->fields.value("m_Conditions").toList();
                const bool exitTime = toInt(transition->fields.value("m_HasExitTime")) != 0;
                if ((conditions.isEmpty() && !exitTime) || !conditionsHold(conditions)) {
                    continue;
                }
                next = unity::fileIDOf(transition->fields.value("m_DstState"));
                break;
            }
            if (!next || visited.contains(next)) {
                break;
            }
            visited.insert(next);
            current = next;
        }
        const unity::YamlObject* chosen = byID.value(current);
        const QString clip = chosen ? unity::guidOf(chosen->fields.value("m_Motion")) : QString();
        const unity::YamlObject* animation = clip.isEmpty() ? nullptr : firstObject(package, clip, 74);
        if (!animation) {
            continue;  // a blend tree, or no motion
        }
        for (const QVariant& curveEntry : animation->fields.value("m_FloatCurves").toList()) {
            const QVariantMap curve = curveEntry.toMap();
            const QVariantList keys = curve.value("curve").toMap().value("m_Curve").toList();
            if (keys.isEmpty()) {
                continue;
            }
            const QString attribute = curve.value("attribute").toString();
            const QString path = curve.value("path").toString();
            const double value = unity::toNumber(keys.first().toMap().value("value"));
            if (attribute == QLatin1String("m_IsActive")) {
                state.active[path] = value > 0.5;
            } else if (attribute.startsWith(QLatin1String("blendShape."))) {
                state.shapes[path + ":" + attribute.mid(11)] = (float)value;
            }
        }
        // transforms: whole vectors per object
        auto firstValue = [](const QVariant& curveEntry, QString& path) {
            const QVariantMap curve = curveEntry.toMap();
            path = curve.value("path").toString();
            const QVariantList keys = curve.value("curve").toMap().value("m_Curve").toList();
            return keys.isEmpty() ? QVariantMap() : keys.first().toMap().value("value").toMap();
        };
        auto vec3 = [](const QVariantMap& v) {
            return glm::vec3((float)unity::toNumber(v.value("x")), (float)unity::toNumber(v.value("y")),
                             (float)unity::toNumber(v.value("z")));
        };
        QString path;
        for (const QVariant& curveEntry : animation->fields.value("m_PositionCurves").toList()) {
            const QVariantMap v = firstValue(curveEntry, path);
            if (!v.isEmpty()) {
                state.positions[path] = vec3(v);
            }
        }
        for (const QVariant& curveEntry : animation->fields.value("m_ScaleCurves").toList()) {
            const QVariantMap v = firstValue(curveEntry, path);
            if (!v.isEmpty()) {
                state.scales[path] = vec3(v);
            }
        }
        for (const QVariant& curveEntry : animation->fields.value("m_EulerCurves").toList()) {
            const QVariantMap v = firstValue(curveEntry, path);
            if (!v.isEmpty()) {
                state.rotations[path] = unity::unityEulerRotation(vec3(v));
            }
        }
        for (const QVariant& curveEntry : animation->fields.value("m_RotationCurves").toList()) {
            const QVariantMap v = firstValue(curveEntry, path);
            if (!v.isEmpty()) {
                state.rotations[path] = glm::normalize(glm::quat((float)unity::toNumber(v.value("w"), 1.0), vec3(v)));
            }
        }
    }
    return state;
}

// The avatar's own shape key for one of the engine's face shapes, by common names.
QString findShape(const QSet<QString>& shapes, const QStringList& candidates) {
    QHash<QString, QString> folded;
    for (const QString& shape : shapes) {
        QString key = shape.toLower();
        key.remove(QRegularExpression("[^a-z0-9]"));
        folded.insert(key, shape);
    }
    for (const QString& candidate : candidates) {
        auto found = folded.find(candidate);
        if (found != folded.end()) {
            return *found;
        }
    }
    return QString();
}

// The avatar script that plays the hand gestures (%1 name, %2 and %3 per gesture 0 to 7: [file, frames, rate] or null).
const char* GESTURE_SCRIPT = R"JS(//
//  gestures.js: %1's hand gestures, from its hand-gesture layer (written by Darkride's avatar import).
//
//  Shift+F1 to F8 set the left hand's gesture and Ctrl+F1 to F8 the right's, the usual desktop keys:
//  F1 neutral, F2 fist, F3 open hand, F4 point, F5 victory, F6 rock'n'roll, F7 hand gun, F8 thumbs up.
//  On a Mac, Option+F1 to F8 set the right hand too, because macOS keeps Control+F1 to F8 for its own
//  keyboard shortcuts. On a Mac keyboard hold fn as well, or the F keys change brightness and volume.
//  Each gesture is one of the avatar's own muscle clips, played on that hand. The gesture also sets the
//  avatar's GestureLeft or GestureRight parameter (0 to 7), which its effects layer may answer with a face.
//
(function () {
    var LEFT = %2;
    var RIGHT = %3;
    var KEYS = ["F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8"];
    var IS_MAC = typeof PlatformInfo !== "undefined" && PlatformInfo.getOperatingSystemType() === "MACOS";

    function gesture(isLeft, index) {
        if (MyAvatar.setAvatarParameter) {
            MyAvatar.setAvatarParameter(isLeft ? "GestureLeft" : "GestureRight", index);
            MyAvatar.setAvatarParameter(isLeft ? "GestureLeftWeight" : "GestureRightWeight", index === 1 ? 1 : 0);
        }
        var clip = (isLeft ? LEFT : RIGHT)[index];
        if (clip) {
            MyAvatar.overrideHandAnimation(isLeft, Script.resolvePath(clip[0]), clip[2], clip[1] > 1, 0, clip[1] - 1);
        } else {
            MyAvatar.restoreHandAnimation(isLeft);
        }
    }

    function onKeyPress(event) {
        var index = KEYS.indexOf(event.text);
        if (index < 0 || event.isAutoRepeat || (event.isAlt && !IS_MAC)) {  // Alt+F4 closes the window elsewhere
            return;
        }
        if (event.isShifted) {
            gesture(true, index);
        } else if (event.isControl || event.isMeta || event.isAlt) {  // on a Mac, Command reads as Control, Control as Meta
            gesture(false, index);
        }
    }

    Controller.keyPressEvent.connect(onKeyPress);
    Script.scriptEnding.connect(function () {
        Controller.keyPressEvent.disconnect(onKeyPress);
        MyAvatar.restoreHandAnimation(true);
        MyAvatar.restoreHandAnimation(false);
    });
    gesture(true, 0);
    gesture(false, 0);
}());
)JS";

}  // namespace

QVector<AvatarSource> findAvatars(const unity::Package& package) {
    QVector<AvatarSource> found;
    for (const auto* prefab : package.withExtension(".prefab")) {
        for (const auto& object : package.objects(prefab->guid)) {
            if (object.classID != 114 || object.stripped ||
                unity::componentClassName(unity::fileIDOf(object.fields.value("m_Script"))) != QLatin1String("VRCAvatarDescriptor")) {
                continue;
            }
            AvatarSource source;
            source.name = QFileInfo(prefab->path).completeBaseName();
            source.prefabGuid = prefab->guid;
            source.prefabPath = prefab->path;
            source.descriptor = object.fields;
            source.modelGuid = modelOf(package, prefab->guid, unity::fileIDOf(object.fields.value("m_GameObject")));
            if (const auto* model = package.byGuid(source.modelGuid)) {
                source.modelPath = model->path;
            }
            found.push_back(source);
            break;
        }
    }
    auto forPhones = [](const AvatarSource& avatar) {
        const QString name = avatar.name.toLower();
        return name.contains("mobile") || name.contains("quest") || name.contains("android");
    };
    std::stable_sort(found.begin(), found.end(), [&](const AvatarSource& a, const AvatarSource& b) {
        return !forPhones(a) && forPhones(b);
    });
    return found;
}

WrittenAvatar writeAvatar(const unity::Package& package, const AvatarSource& avatar, const QString& folderPath,
                          const QString& name) {
    WrittenAvatar result;
    const auto* model = package.byGuid(avatar.modelGuid);
    if (!model || !model->hasData) {
        result.error = "the avatar's model is not in the package";
        return result;
    }
    QDir folder(folderPath);
    if (!folder.mkpath(".")) {
        result.error = "cannot make " + folderPath;
        return result;
    }
    QStringList& notes = result.notes;

    // The model, unchanged, and read as the engine will read it.
    const QString modelFile = name + "." + QFileInfo(model->path).suffix().toLower();
    result.modelPath = folder.filePath(modelFile);
    {
        QFile file(result.modelPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(model->data) != model->data.size()) {
            result.error = "cannot write " + result.modelPath;
            return result;
        }
    }
    const HFMModel::Pointer hfmModel = FBXSerializer().read(model->data, hifi::VariantMultiHash(), QUrl::fromLocalFile(result.modelPath));
    if (!hfmModel) {
        result.error = "the engine cannot read " + model->path;
        return result;
    }
    if (hfmModel->joints.size() > 255) {
        notes << QString("%1 joints: the avatar packet carries 255, so some will not reach other people")
                     .arg((int)hfmModel->joints.size());
    }
    for (const auto& shape : hfmModel->shapes) {
        if (shape.skinDeformer < hfmModel->skinDeformers.size() &&
            hfmModel->skinDeformers[shape.skinDeformer].clusters.size() > 128) {
            notes << QString("mesh %1 uses %2 bones; the skinning shader draws 128 per mesh")
                         .arg(hfmModel->meshIndicesToModelNames.value(shape.mesh))
                         .arg((int)hfmModel->skinDeformers[shape.skinDeformer].clusters.size());
        }
    }

    // Humanoid joints: Unity's humanoid map names them; the engine knows them by Darkride's standard names.
    const QVariantMap importer = package.meta(avatar.modelGuid).value("ModelImporter").toMap();
    const QVariantMap human = importer.value("humanDescription").toMap();
    QStringList jointMap;
    QHash<QString, QString> skeletonNames;  // the file's joint name -> the avatar's, where jointMap renames it
    for (const QVariant& entry : human.value("human").toList()) {
        const QVariantMap bone = entry.toMap();
        const int index = humanoid::boneFromUnityName(bone.value("humanName").toString());
        if (index < 0) {
            notes << "unknown humanoid bone " + bone.value("humanName").toString();
            continue;
        }
        jointMap << QString("jointMap = %1 = %2").arg(humanoid::standardName(index), bone.value("boneName").toString());
        skeletonNames.insert(bone.value("boneName").toString(), humanoid::standardName(index));
    }
    if (jointMap.isEmpty()) {
        notes << "the model has no humanoid map; it will not animate";
    }

    // Unity's humanoid frames, through which the avatar's own muscle clips play; the clips; and its hand gestures.
    QStringList humanoidLines;
    QStringList avatarFiles;
    const QVariantMap frames = humanoidFrames(human, notes);
    if (!frames.isEmpty()) {
        humanoidLines << "humanoid = " + QString::fromUtf8(QJsonDocument::fromVariant(frames).toJson(QJsonDocument::Compact));
        struct Clip {
            QString file;
            int frames { 1 };
            double rate { 30.0 };
        };
        QHash<QString, Clip> clips;  // by the Unity clip's GUID
        for (const auto* asset : package.withExtension(".anim")) {
            QString base = QFileInfo(asset->path).completeBaseName();
            base.replace(QRegularExpression("[^A-Za-z0-9_-]+"), "_");
            QString fileName = base + ".muscles.json";
            for (int n = 2; avatarFiles.contains(fileName); n++) {
                fileName = QString("%1-%2.muscles.json").arg(base).arg(n);
            }
            Clip clip { fileName };
            if (writeMuscleClip(package, asset->guid, folder.filePath(fileName), nullptr, &clip.frames, &clip.rate)) {
                clips.insert(asset->guid, clip);
                avatarFiles << fileName;
            }
        }
        if (!clips.isEmpty()) {
            notes << QString("%1 muscle clips").arg(clips.size());
        }
        const GestureClips gestures = gestureClips(package, avatar.descriptor);
        auto gestureList = [&](const QString* guids) {
            QJsonArray list;
            int found = 0;
            for (int g = 0; g < 8; g++) {
                auto clip = clips.find(guids[g]);
                if (clip == clips.end()) {
                    list.append(QJsonValue());
                } else {
                    list.append(QJsonArray { clip->file, clip->frames, clip->rate });
                    found++;
                }
            }
            return std::make_pair(list, found);
        };
        const auto left = gestureList(gestures.left);
        const auto right = gestureList(gestures.right);
        if (left.second + right.second > 0) {
            QFile script(folder.filePath("gestures.js"));
            const QString text = QString(GESTURE_SCRIPT).arg(name)
                .arg(QString::fromUtf8(QJsonDocument(left.first).toJson(QJsonDocument::Compact)))
                .arg(QString::fromUtf8(QJsonDocument(right.first).toJson(QJsonDocument::Compact)));
            if (script.open(QIODevice::WriteOnly) && script.write(text.toUtf8()) > 0) {
                humanoidLines << "script = gestures.js";
                avatarFiles << "gestures.js";
                notes << QString("hand gestures: %1 left, %2 right (Shift+F1 to F8; Ctrl+F1 to F8, or Option+F1 to F8 on a Mac)")
                             .arg(left.second).arg(right.second);
            }
        }
    }

    // Materials: Unity's own material for each of the model's, drawn by a stand-in for now.
    Textures textures(package, folder);
    QJsonObject materials;
    QHash<QString, int> families;
    for (const QVariant& entry : importer.value("externalObjects").toList()) {
        const QVariantMap remap = entry.toMap();
        const QVariantMap first = remap.value("first").toMap();
        if (!first.value("type").toString().endsWith("Material")) {
            continue;
        }
        QString family;
        const QJsonObject material = standIn(package, unity::guidOf(remap.value("second")), textures, notes, family);
        if (!material.isEmpty()) {
            materials.insert("mat::" + first.value("name").toString(), material);
            families[family]++;
        }
    }
    for (auto it = families.begin(); it != families.end(); ++it) {
        notes << QString("%1 %2 material%3 drawn by a matte stand-in until the engine has the shader")
                     .arg(it.value()).arg(it.key()).arg(it.value() == 1 ? "" : "s");
    }

    // The model as the avatar's prefab changes it (Unity's prefab overrides), then what the effects layer's starting
    // animations set on top: bones moved, turned or scaled, objects shown or hidden, shape keys and material slots.
    const QStringList paths = unity::jointPaths(*hfmModel);
    const float metersPerUnit = unity::metersPerUnit(importer, *hfmModel);
    ModelState state = prefabModelState(package, avatar.prefabGuid, avatar.modelGuid, *hfmModel, paths, metersPerUnit);
    const ModelState prefabState = state;  // before the effects layer: what a state that writes defaults goes back to
    if (state.stale > 0) {
        notes << QString("%1 prefab overrides name objects the model no longer has").arg(state.stale);
    }
    const StartingState start = startingState(package, avatar.descriptor);
    QSet<QString> unknownPaths;
    auto poseAt = [&](const QString& path) -> ModelState::Pose* {
        if (path.isEmpty()) {
            return &state.root;
        }
        const int joint = paths.indexOf(path);
        if (joint < 0) {
            unknownPaths.insert(path);
            return nullptr;
        }
        return &state.poses[joint];
    };
    for (auto it = start.positions.begin(); it != start.positions.end(); ++it) {
        if (ModelState::Pose* pose = poseAt(it.key())) {
            pose->position = it.value();
            pose->changed = true;
        }
    }
    for (auto it = start.rotations.begin(); it != start.rotations.end(); ++it) {
        if (ModelState::Pose* pose = poseAt(it.key())) {
            pose->rotation = it.value();
            pose->changed = true;
        }
    }
    for (auto it = start.scales.begin(); it != start.scales.end(); ++it) {
        if (ModelState::Pose* pose = poseAt(it.key())) {
            pose->scale = it.value();
            pose->changed = true;
        }
    }
    for (auto it = start.active.begin(); it != start.active.end(); ++it) {
        const int joint = paths.indexOf(it.key());
        if (joint >= 0) {
            state.active[joint] = it.value();
        } else {
            unknownPaths.insert(it.key());
        }
    }
    if (!unknownPaths.isEmpty()) {
        notes << QString("the starting animations name %1 objects the model does not have").arg(unknownPaths.size());
    }

    // Bones whose resting transform differs from the model file's.
    QStringList poseLines;
    const float MIN_SCALE = 1.0e-4f;  // a bone scaled to 0 hides what it carries; the engine needs an invertible pose
    for (int i = 0; i < (int)state.poses.size(); i++) {
        const ModelState::Pose& pose = state.poses[i];
        if (!pose.changed) {
            continue;
        }
        glm::vec3 fileTranslation, fileScale;
        glm::quat fileRotation;
        unity::jointLocalPose(hfmModel->joints[i], fileTranslation, fileRotation, fileScale);
        const glm::vec3 translation = unity::positionFromUnity(pose.position, metersPerUnit);
        const glm::quat rotation = unity::rotationFromUnity(glm::normalize(pose.rotation));
        const glm::vec3 scale = glm::max(pose.scale, glm::vec3(MIN_SCALE));
        const float turned = glm::degrees(2.0f * acosf(std::min(1.0f, fabsf(glm::dot(rotation, fileRotation)))));
        if (glm::length(translation - fileTranslation) * metersPerUnit < 1.0e-6f && turned < 1.0e-3f &&
            glm::length(scale - fileScale) < 1.0e-6f) {
            continue;
        }
        auto numbers = [](std::initializer_list<float> values) {
            QJsonArray array;
            for (float v : values) {
                array.append(QString::number(v, 'g', 7).toDouble());
            }
            return array;
        };
        const QJsonObject json {
            { "translation", numbers({ translation.x, translation.y, translation.z }) },
            { "rotation", numbers({ rotation.x, rotation.y, rotation.z, rotation.w }) },
            { "scale", numbers({ scale.x, scale.y, scale.z }) },
        };
        poseLines << QString("jointDefaultPose = %1 = %2").arg(hfmModel->joints[i].name)
                         .arg(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
    }
    if (!poseLines.isEmpty()) {
        notes << QString("%1 bones rest as the prefab and the starting animations set them").arg(poseLines.size());
    }

    // Rotation constraints (VRCRotationConstraint): twist bones that turn part way with the wrist or the elbow, a strap
    // that follows an arm. Their joints are named as the avatar names them (after jointMap).
    const QHash<qint64, int> modelJoints = modelObjectJoints(paths);
    QHash<qint64, const unity::YamlObject*> prefabObjects;
    for (const auto& object : package.objects(avatar.prefabGuid)) {
        prefabObjects.insert(object.fileID, &object);
    }
    auto jointOfReference = [&](const QVariant& reference) {  // a transform or game object in the prefab -> joint
        const unity::YamlObject* object = prefabObjects.value(unity::fileIDOf(reference));
        if (!object || !object->stripped) {
            return -2;  // not one of the model's objects
        }
        const QVariant source = object->fields.value("m_CorrespondingSourceObject");
        if (unity::guidOf(source) != avatar.modelGuid) {
            return -2;
        }
        return modelJoints.value(unity::fileIDOf(source), -2);
    };
    auto skeletonName = [&](int joint) {
        const QString name = hfmModel->joints[joint].name;
        return skeletonNames.value(name, name);
    };
    QStringList constraintLines;
    QHash<QString, int> otherConstraints;
    for (const auto& object : package.objects(avatar.prefabGuid)) {
        const QString component = object.classID == 114 && !object.stripped
            ? unity::componentClassName(unity::fileIDOf(object.fields.value("m_Script"))) : QString();
        if (!component.endsWith("Constraint")) {
            continue;
        }
        if (component != "VRCRotationConstraint") {
            otherConstraints[component]++;
            continue;
        }
        const QVariantMap& f = object.fields;
        if (unity::toNumber(f.value("IsActive"), 1.0) == 0.0) {
            continue;
        }
        const QVariant target = unity::fileIDOf(f.value("TargetTransform")) ? f.value("TargetTransform") : f.value("m_GameObject");
        const int joint = jointOfReference(target);
        if (joint < 0) {
            otherConstraints["VRCRotationConstraint on an object the model does not have"]++;
            continue;
        }
        QJsonArray sources;
        const QVariantMap sourceMap = f.value("Sources").toMap();
        for (int i = 0; i < 16; i++) {
            const QVariantMap source = sourceMap.value(QString("source%1").arg(i)).toMap();
            const float weight = (float)unity::toNumber(source.value("Weight"));
            const int sourceJoint = source.isEmpty() ? -2 : jointOfReference(source.value("SourceTransform"));
            if (weight > 0.0f && sourceJoint >= 0) {
                sources.append(QJsonArray { skeletonName(sourceJoint), weight });
            }
        }
        if (sources.isEmpty()) {
            continue;
        }
        auto euler = [&](const char* key) {
            const QVariantMap v = f.value(key).toMap();
            return unity::rotationFromUnityEuler(glm::vec3((float)unity::toNumber(v.value("x")),
                                                           (float)unity::toNumber(v.value("y")),
                                                           (float)unity::toNumber(v.value("z"))));
        };
        auto quatJson = [](const glm::quat& q) {
            return QJsonArray { QString::number(q.x, 'g', 7).toDouble(), QString::number(q.y, 'g', 7).toDouble(),
                                QString::number(q.z, 'g', 7).toDouble(), QString::number(q.w, 'g', 7).toDouble() };
        };
        const QJsonObject json {
            { "sources", sources },
            { "weight", unity::toNumber(f.value("GlobalWeight"), 1.0) },
            { "axes", QJsonArray { (int)unity::toNumber(f.value("AffectsRotationX"), 1.0),
                                   (int)unity::toNumber(f.value("AffectsRotationY"), 1.0),
                                   (int)unity::toNumber(f.value("AffectsRotationZ"), 1.0) } },
            { "atRest", quatJson(euler("RotationAtRest")) },
            { "offset", quatJson(euler("RotationOffset")) },
            { "local", unity::toNumber(f.value("SolveInLocalSpace")) != 0.0 },
        };
        if (unity::toNumber(f.value("FreezeToWorld")) != 0.0) {
            notes << "not yet: a rotation constraint frozen to the world (" + skeletonName(joint) + ")";
        }
        constraintLines << QString("rotationConstraint = %1 = %2").arg(skeletonName(joint))
                               .arg(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
    }
    if (!constraintLines.isEmpty()) {
        notes << QString("%1 rotation constraints (twist bones and the like)").arg(constraintLines.size());
    }
    for (auto it = otherConstraints.begin(); it != otherConstraints.end(); ++it) {
        notes << QString("not yet: %1 %2").arg(it.value()).arg(it.key());
    }

    // Spring bones, their colliders and the contacts, from the prefab and the prefabs nested in it, written as data
    // for the engine (UnitySpringBones.h), which simulates them (libraries/animation/src/SpringBones.h).
    const bool sortChildrenByName = unity::toNumber(importer.value("meshes").toMap().value("sortHierarchyByName"), 1.0) != 0.0;
    const SpringBoneData springBones = springBoneData(prefabComponents(package, avatar.prefabGuid), avatar.modelGuid, *hfmModel,
                                                paths, skeletonNames, sortChildrenByName);
    QStringList springBoneLines;
    if (springBones.bones + springBones.contacts > 0) {
        springBoneLines << "springBones = " + QString::fromUtf8(QJsonDocument(springBones.json).toJson(QJsonDocument::Compact));
        notes << QString("%1 spring bones moving %2 joints, %3 colliders and %4 contacts written for the engine to simulate")
                     .arg(springBones.bones).arg(springBones.joints).arg(springBones.colliders).arg(springBones.contacts);
    }
    notes << springBones.notes;
    float avatarScale = 1.0f;
    if (state.root.changed) {
        const glm::vec3 s = state.root.scale;
        avatarScale = (s.x + s.y + s.z) / 3.0f;
        if (fabsf(s.x - s.y) > 1.0e-4f || fabsf(s.y - s.z) > 1.0e-4f) {
            notes << "the prefab scales the avatar unevenly; it is scaled evenly by the average";
        }
        if (glm::length(state.root.position) > 1.0e-4f || fabsf(state.root.rotation.w) < 0.99999f) {
            notes << "the prefab moves or turns the avatar's root; not carried over";
        }
        if (fabsf(avatarScale - 1.0f) > 1.0e-4f) {
            notes << QString("the prefab scales the avatar by %1").arg(avatarScale);
        }
    }

    // The avatar's menu and parameters and the effects layer's state machines, as data the engine runs
    // (AvatarMenuImport.h). What the layers switch on and off is left to the engine, not hidden at load.
    QHash<QString, float> prefabWeights;
    {
        const QHash<QString, QStringList> keysByMesh = shapeKeysByMesh(model->data);
        for (auto renderer = prefabState.shapeWeights.begin(); renderer != prefabState.shapeWeights.end(); ++renderer) {
            const QStringList keys = keysByMesh.value(hfmModel->joints[renderer.key()].name);
            for (auto weight = renderer.value().begin(); weight != renderer.value().end(); ++weight) {
                if (weight.key() >= 0 && weight.key() < keys.size()) {
                    prefabWeights.insert(keys[weight.key()], qBound(0.0f, weight.value() / 100.0f, 1.0f));
                }
            }
        }
    }
    AvatarMenuInputs menuInputs;
    // the contacts in the avatar's own meters: a component's lengths are in its joint's units, which the joint's scale
    // (the product of the model's node scales down to it) and the avatar's turn into meters at the avatar's size
    for (const QJsonValue& value : springBones.json.value("contacts").toArray()) {
        QJsonObject contact = value.toObject();
        float scale = avatarScale;
        for (int i = 0; i < hfmModel->joints.size(); i++) {
            const QString& name = hfmModel->joints[i].name;
            if (skeletonNames.value(name, name) == contact.value("joint").toString()) {
                scale *= glm::length(glm::vec3(hfmModel->joints[i].transform[0]));
                break;
            }
        }
        QJsonArray position = contact.value("position").toArray();
        for (int i = 0; i < position.size(); i++) {
            position[i] = position[i].toDouble() * scale;
        }
        contact["position"] = position;
        contact["radius"] = contact.value("radius").toDouble() * scale;
        contact["height"] = contact.value("height").toDouble() * scale;
        menuInputs.contacts.append(contact);
    }
    menuInputs.model = hfmModel.get();
    menuInputs.paths = paths;
    menuInputs.skeletonNames = skeletonNames;
    menuInputs.metersPerUnit = metersPerUnit;
    menuInputs.copyIcon = [&](const QString& guid) { return textures.copy(guid, notes); };
    menuInputs.prefabActive = [&](int joint) { return prefabState.active.value(joint, true); };
    menuInputs.prefabPose = [&](int joint) {
        const ModelState::Pose& pose = prefabState.poses[joint];
        const glm::vec3 t = unity::positionFromUnity(pose.position, metersPerUnit);
        const glm::quat r = unity::rotationFromUnity(glm::normalize(pose.rotation));
        return QJsonObject {
            { "translation", QJsonArray { t.x, t.y, t.z } },
            { "rotation", QJsonArray { r.x, r.y, r.z, r.w } },
            { "scale", QJsonArray { pose.scale.x, pose.scale.y, pose.scale.z } },
        };
    };
    menuInputs.prefabBlendshapes = prefabWeights;
    const AvatarMenuData avatarMenu = avatarMenuData(package, avatar.descriptor, menuInputs);
    QStringList menuLines;
    if (avatarMenu.layers + avatarMenu.controls > 0) {
        menuLines << "avatarMenu = " + QString::fromUtf8(QJsonDocument(avatarMenu.json).toJson(QJsonDocument::Compact));
        notes << QString("avatar menu: %1 controls driving %2 animator layers (%3 states); the engine runs them")
                     .arg(avatarMenu.controls).arg(avatarMenu.layers).arg(avatarMenu.states);
    }
    notes << avatarMenu.notes;

    // What starts hidden: the meshes under an object the prefab or the effects layer switches off, or whose renderer the
    // prefab disables, are drawn invisible.
    auto shown = [&](int joint) {
        for (int j = joint, hops = 0; j >= 0 && j < (int)hfmModel->joints.size() && hops < 256;
             j = hfmModel->joints[j].parentIndex, hops++) {
            if (!state.active.value(j, true)) {
                return false;
            }
        }
        return state.drawn.value(joint, true);
    };
    QJsonObject hidden;
    QStringList hiddenMeshes;
    for (int i = 0; i < (int)hfmModel->shapes.size(); i++) {
        const auto& shape = hfmModel->shapes[i];
        if (shape.joint >= hfmModel->joints.size() || shown((int)shape.joint) || avatarMenu.controlledParts.contains(i)) {
            continue;
        }
        hidden.insert(QString::number(i), QJsonObject { { "materialVersion", 1 },
            { "materials", QJsonObject { { "model", "hifi_pbr" }, { "opacity", 0.0 },
                                        { "opacityMapMode", "OPACITY_MAP_BLEND" } } } });
        const QString meshName = hfmModel->meshIndicesToModelNames.value(shape.mesh);
        if (!hiddenMeshes.contains(meshName)) {
            hiddenMeshes << meshName;
        }
    }
    if (!hiddenMeshes.isEmpty()) {
        notes << "starts hidden: " + hiddenMeshes.join(", ");
    }

    // Materials the prefab puts in a renderer's slots, on that renderer's parts only.
    QJsonObject slotMaterials;
    for (auto renderer = state.materials.begin(); renderer != state.materials.end(); ++renderer) {
        for (auto slot = renderer.value().begin(); slot != renderer.value().end(); ++slot) {
            bool placed = false;
            QString family;
            for (int i = 0; i < (int)hfmModel->shapes.size(); i++) {
                const auto& shape = hfmModel->shapes[i];
                if ((int)shape.joint == renderer.key() && (int)shape.meshPart == slot.key()) {
                    const QJsonObject material = standIn(package, slot.value(), textures, notes, family);
                    if (!material.isEmpty()) {
                        slotMaterials.insert(QString::number(i), material);
                        placed = true;
                    }
                }
            }
            if (!placed) {
                notes << QString("not yet: the prefab's extra material on %1 (slot %2 draws a part again, a second pass)")
                             .arg(hfmModel->joints[renderer.key()].name).arg(slot.key());
            }
        }
    }
    // The engine's reader keeps a shape key's name whole only when the .fst names it; otherwise it keeps what follows
    // the last dot ("vrc.v.aa" reads "aa").
    const QSet<QString> shapes(hfmModel->blendshapeChannelNames.begin(), hfmModel->blendshapeChannelNames.end());
    auto hasShape = [&](const QString& shape) {
        return !shape.isEmpty() && (shapes.contains(shape) || shapes.contains(shape.section('.', -1)));
    };

    // The shape keys the prefab sets on its renderers (by index) and then the effects layer at the start (an outfit's fit,
    // the resting mouth) become the starting weights of the model's own blend shapes, which the engine keeps beside its
    // standard ones (by name, for every mesh that has the shape key).
    QMap<QString, float> startingWeights;
    const QHash<QString, QStringList> shapeKeys = shapeKeysByMesh(model->data);
    int unmappedWeights = 0;
    for (auto renderer = state.shapeWeights.begin(); renderer != state.shapeWeights.end(); ++renderer) {
        const QStringList keys = shapeKeys.value(hfmModel->joints[renderer.key()].name);
        for (auto weight = renderer.value().begin(); weight != renderer.value().end(); ++weight) {
            const QString shape = weight.key() >= 0 && weight.key() < keys.size() ? keys[weight.key()] : QString();
            if (hasShape(shape)) {
                startingWeights[shape] = qBound(0.0f, weight.value() / 100.0f, 1.0f);
            } else {
                unmappedWeights++;
            }
        }
    }
    if (unmappedWeights > 0) {
        notes << QString("%1 of the prefab's shape-key weights name no shape key of the model").arg(unmappedWeights);
    }
    for (auto it = start.shapes.begin(); it != start.shapes.end(); ++it) {
        const QString shape = it.key().section(':', 1);
        if (hasShape(shape)) {
            startingWeights[shape] = qBound(0.0f, it.value() / 100.0f, 1.0f);
        }
    }
    QStringList shapeLines { "ownBlendshapes = true" };
    QStringList startingNames;
    for (auto it = startingWeights.begin(); it != startingWeights.end(); ++it) {
        if (it.value() > 0.0f) {
            shapeLines << QString("blendshapeDefault = %1 = %2").arg(it.key()).arg(it.value());
            startingNames << it.key();
        }
    }
    if (!startingNames.isEmpty()) {
        notes << QString("starting shape keys: %1").arg(startingNames.join(", "));
    }

    // The engine's talking mouth and blink, on the avatar's own shape keys.
    QStringList faceLines;
    const QVariantList visemes = avatar.descriptor.value("VisemeBlendShapes").toList();
    auto viseme = [&](int index) {
        const QString shape = index < visemes.size() ? visemes[index].toString() : QString();
        return hasShape(shape) ? shape : QString();
    };
    if (!viseme(10).isEmpty()) {
        faceLines << QString("bs = JawOpen = %1 = 1").arg(viseme(10));      // aa
    }
    if (!viseme(14).isEmpty()) {
        faceLines << QString("bs = LipsFunnel = %1 = 0.7").arg(viseme(14));  // ou
    }
    // The blink only as the avatar's own platform gives it: with eye look on and eyelids that are shape keys, the shape
    // keys found by name; with eye look off, only the older avatars' vrc.blink_left and vrc.blink_right. An avatar whose
    // creator gave it no blink gets none: its own face animations close its eyes, and a blink on top would undo them.
    const bool eyeLook = unity::toNumber(avatar.descriptor.value("enableEyeLook")) != 0.0;
    const int eyelidType = (int)unity::toNumber(avatar.descriptor.value("customEyeLookSettings").toMap().value("eyelidType"));
    QString blinkLeft, blinkRight, blinkBoth;
    if (eyeLook && eyelidType == 1) {
        blinkLeft = findShape(shapes, { "vrcblinkleft", "blinkleft", "blinkl", "eyeblinkleft", "eyeblinkl", "eyecloseleft",
                                        "eyeclosel", "closeleft" });
        blinkRight = findShape(shapes, { "vrcblinkright", "blinkright", "blinkr", "eyeblinkright", "eyeblinkr",
                                         "eyecloseright", "eyecloser", "closeright" });
        blinkBoth = findShape(shapes, { "blink", "eyeblink", "eyeclose", "blinkboth" });
    } else if (!eyeLook) {
        blinkLeft = findShape(shapes, { "vrcblinkleft" });
        blinkRight = findShape(shapes, { "vrcblinkright" });
    }
    if (!blinkLeft.isEmpty() && !blinkRight.isEmpty()) {
        faceLines << QString("bs = EyeBlink_L = %1 = 1").arg(blinkLeft) << QString("bs = EyeBlink_R = %1 = 1").arg(blinkRight);
    } else if (!blinkBoth.isEmpty()) {
        faceLines << QString("bs = EyeBlink_L = %1 = 0.5").arg(blinkBoth) << QString("bs = EyeBlink_R = %1 = 0.5").arg(blinkBoth);
    } else if (eyeLook && eyelidType == 2) {
        notes << "not yet: eyelids that blink with bones";
    } else {
        notes << "no automatic blink: its creator gave it none";
    }
    // the rest of the face, for face tracking, on the shapes the procedural face leaves alone
    faceLines << unity::faceTrackingShapeLines(shapes, faceLines);
    // and the brows' lift, the smile, the wide eyes (and a blink or jaw the rules above didn't give), which only face
    // tracking moves: the procedural face's talking and blinking leave them alone
    faceLines << unity::faceTrackingOnlyShapeLines(shapes, faceLines);
    // of the face's 52 (ARKit's set): the ten UserBlendshapes after TongueOut are no face
    notes << QString("face tracking: %1 of %2 face shapes").arg(unity::countFaceShapes(faceLines)).arg((int)Blendshapes::UserBlendshape0);

    notes << "not yet: the animator's other layers (walking, hand gestures, sitting, AFK; the emotes the menu picks play the engine's own animations)";

    // The .fst.
    QJsonArray materialMap { materials };  // later entries win
    if (!slotMaterials.isEmpty()) {
        materialMap.append(slotMaterials);
    }
    if (!hidden.isEmpty()) {
        materialMap.append(hidden);
    }
    QStringList fst {
        "name = " + name,
        "type = body+head",
        "scale = " + QString::number(avatarScale, 'g', 6),
        "filename = " + modelFile,
        "retarget = humanoid",
        "joint = jointRoot = Hips",
        "joint = jointLean = Spine",
        "joint = jointNeck = Neck",
        "joint = jointHead = Head",
        "joint = jointEyeLeft = LeftEye",
        "joint = jointEyeRight = RightEye",
        "joint = jointLeftHand = LeftHand",
        "joint = jointRightHand = RightHand",
    };
    fst << jointMap << poseLines << constraintLines << faceLines << shapeLines << humanoidLines << springBoneLines
        << menuLines;
    if (!avatarFiles.isEmpty()) {
        // Named here so File > Put Avatar in My Files sends them up beside the .fst.
        fst << "avatarFiles = " + QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(avatarFiles)).toJson(QJsonDocument::Compact));
    }
    fst << "materialMap = " + QString::fromUtf8(QJsonDocument(materialMap).toJson(QJsonDocument::Compact));
    result.fstPath = folder.filePath(name + ".fst");
    QFile file(result.fstPath);
    if (!file.open(QIODevice::WriteOnly) || file.write((fst.join('\n') + '\n').toUtf8()) < 0) {
        result.error = "cannot write " + result.fstPath;
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace avatarimport
