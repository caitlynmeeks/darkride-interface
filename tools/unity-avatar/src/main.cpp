//
//  main.cpp
//  tools/unity-avatar/src
//
//  Created on 2026-10-14 for Darkride's import of avatars from Unity packages.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include <cstdio>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMap>

#include <SharedUtil.h>

#include <UnityIds.h>
#include <UnityTransforms.h>
#include <UnityPackage.h>
#include <UnityYaml.h>
#include <UnityAvatar.h>
#include <UnityHumanoid.h>

#include <AnimSkeleton.h>
#include <FBXSerializer.h>
#include <Humanoid.h>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

namespace {

void print(const QString& text) {
    fputs(qPrintable(text + '\n'), stdout);
}

QVariant objectsToVariant(const QVector<unity::YamlObject>& objects) {
    QVariantList list;
    for (const auto& object : objects) {
        QVariantMap entry;
        entry["classID"] = object.classID;
        entry["fileID"] = QString::number(object.fileID);
        entry["stripped"] = object.stripped;
        entry["type"] = object.type;
        entry["fields"] = object.fields;
        list.append(entry);
    }
    return list;
}

bool writeJson(const QString& path, const QVariant& value) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact));
    return true;
}

bool isText(const QString& path) {
    static const QStringList EXTENSIONS { ".prefab", ".unity", ".mat", ".controller", ".anim", ".asset",
                                          ".overrideController", ".mask", ".physicMaterial" };
    for (const QString& extension : EXTENSIONS) {
        if (path.endsWith(extension, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

// What the package holds that makes an avatar.
void inventory(const unity::Package& package) {
    QMap<QString, int> extensions;
    for (const auto& asset : package.assets()) {
        const int dot = asset.path.lastIndexOf('.');
        extensions[asset.hasData && dot > asset.path.lastIndexOf('/') ? asset.path.mid(dot).toLower() : "(folder)"]++;
    }
    QStringList counts;
    for (auto it = extensions.begin(); it != extensions.end(); ++it) {
        counts << QString("%1 %2").arg(it.key()).arg(it.value());
    }
    print(QString("%1 assets: %2").arg(package.assets().size()).arg(counts.join(", ")));

    for (const auto* prefab : package.withExtension(".prefab")) {
        QMap<QString, int> components;
        int descriptors = 0;
        for (const auto& object : package.objects(prefab->guid)) {
            if (object.classID == 114 && !object.stripped) {
                QString name = unity::componentClassName(unity::fileIDOf(object.fields.value("m_Script")));
                if (name.isEmpty()) {
                    name = "script " + unity::guidOf(object.fields.value("m_Script")).left(8);
                }
                components[name]++;
                descriptors += name == QLatin1String("VRCAvatarDescriptor");
            }
        }
        QStringList parts;
        for (auto it = components.begin(); it != components.end(); ++it) {
            parts << QString("%1 %2").arg(it.key()).arg(it.value());
        }
        print(QString("%1%2: %3").arg(prefab->path).arg(descriptors ? " [avatar]" : "")
                  .arg(parts.isEmpty() ? QString("-") : parts.join(", ")));
    }
}

// Checks the engine's muscle math against Unity's own poses of the same model (a JSON exported from Unity's editor): the frames
// worked out from the package's humanoid description, through humanoid::MuscleFrames, against the local rotation Unity
// gave every humanoid bone for each muscle vector the export set. Unity's coordinates mirror the engine's along X.
int checkMuscles(const unity::Package& package, const avatarimport::AvatarSource& avatar, const QString& exportPath) {
    QFile exportFile(exportPath);
    if (!exportFile.open(QIODevice::ReadOnly)) {
        print("error: cannot read " + exportPath);
        return 1;
    }
    const QJsonObject unityExport = QJsonDocument::fromJson(exportFile.readAll()).object();
    const auto* model = package.byGuid(avatar.modelGuid);
    const HFMModel::Pointer hfmModel = FBXSerializer().read(model->data, hifi::VariantMultiHash(), QUrl());
    const QVariantMap importer = package.meta(avatar.modelGuid).value("ModelImporter").toMap();
    const QVariantMap human = importer.value("humanDescription").toMap();
    // the joints renamed as the .fst's jointMap renames them
    QHash<QString, QString> standardOf;
    for (const QVariant& entry : human.value("human").toList()) {
        const QVariantMap bone = entry.toMap();
        const int index = humanoid::boneFromUnityName(bone.value("humanName").toString());
        if (index >= 0) {
            standardOf.insert(bone.value("boneName").toString(), humanoid::standardName(index));
        }
    }
    std::vector<HFMJoint> joints = hfmModel->joints;
    for (auto& joint : joints) {
        joint.name = standardOf.value(joint.name, joint.name);
    }
    AnimSkeleton skeleton(joints, QMap<int, glm::quat>());
    QStringList notes;
    humanoid::MuscleFrames frames;
    if (!frames.setup(skeleton, avatarimport::humanoidFrames(human, notes))) {
        print("error: no humanoid frames");
        return 1;
    }
    const QJsonArray poseBones = unityExport.value("poseBones").toArray();
    const QJsonArray poses = unityExport.value("poses").toArray();
    std::vector<bool> animated(humanoid::MUSCLE_COUNT, true);
    QMap<QString, QVector<float>> errors;
    for (const QJsonValue& poseValue : poses) {
        const QJsonObject pose = poseValue.toObject();
        std::vector<float> muscles;
        for (const QJsonValue& v : pose.value("muscles").toArray()) {
            muscles.push_back((float)v.toDouble());
        }
        std::vector<glm::quat> rotations(skeleton.getNumJoints());
        frames.pose(muscles, animated, rotations);
        const QJsonArray local = pose.value("local").toArray();
        for (int i = 0; i < poseBones.size(); i++) {
            const int bone = poseBones[i].toInt();
            if (bone == humanoid::Hips) {
                continue;
            }
            const int joint = skeleton.nameToJointIndex(humanoid::standardName(bone));
            if (joint < 0) {
                continue;
            }
            const QJsonArray q = local[i].toArray();
            const glm::quat unity((float)q[3].toDouble(), (float)q[0].toDouble(), -(float)q[1].toDouble(), -(float)q[2].toDouble());
            const float dot = fabsf(glm::dot(glm::normalize(unity), glm::normalize(rotations[joint])));
            errors[humanoid::unityName(bone)].append(glm::degrees(2.0f * acosf(std::min(1.0f, dot))));
        }
    }
    QVector<QPair<float, QString>> worst;
    QVector<float> all;
    for (auto it = errors.begin(); it != errors.end(); ++it) {
        float maximum = 0.0f;
        for (float e : it.value()) {
            maximum = std::max(maximum, e);
            all.append(e);
        }
        worst.append({ maximum, it.key() });
    }
    std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::sort(all.begin(), all.end());
    print(QString("%1 poses, %2 bones: median %3 deg, 99%% %4, max %5").arg(poses.size()).arg(errors.size())
              .arg(all[all.size() / 2], 0, 'f', 3).arg(all[all.size() * 99 / 100], 0, 'f', 3).arg(all.last(), 0, 'f', 3));
    for (int i = 0; i < 8 && i < worst.size(); i++) {
        print(QString("  %1 max %2").arg(worst[i].second).arg(worst[i].first, 0, 'f', 3));
    }
    return 0;
}

// Checks the frame conversion (UnityTransforms.h) against Unity's import pose of the avatar's model (exported from Unity's
// editor): every
// object's local position, rotation and scale as Unity holds them, against the engine's joints read from the FBX.
int checkFrames(const unity::Package& package, const avatarimport::AvatarSource& avatar, const QString& exportPath) {
    QFile exportFile(exportPath);
    if (!exportFile.open(QIODevice::ReadOnly)) {
        print("error: cannot read " + exportPath);
        return 1;
    }
    const QJsonObject unityExport = QJsonDocument::fromJson(exportFile.readAll()).object();
    const auto* model = package.byGuid(avatar.modelGuid);
    const HFMModel::Pointer hfmModel = FBXSerializer().read(model->data, hifi::VariantMultiHash(), QUrl());
    const float metersPerUnit = unity::metersPerUnit(package.meta(avatar.modelGuid).value("ModelImporter").toMap(), *hfmModel);
    const QStringList paths = unity::jointPaths(*hfmModel);
    int compared = 0, missing = 0;
    float worstPosition = 0.0f, worstAngle = 0.0f, worstScale = 0.0f;
    QString worstPositionPath, worstAnglePath, worstScalePath;
    for (const QJsonValue& value : unityExport.value("importPose").toArray()) {
        const QJsonObject pose = value.toObject();
        const QString path = pose.value("path").toString();
        if (path.isEmpty()) {
            continue;  // the model's root, which is the file itself
        }
        const int joint = paths.indexOf(path);
        if (joint < 0) {
            missing++;
            continue;
        }
        auto vec = [&](const char* key) {
            const QJsonArray a = pose.value(key).toArray();
            return glm::vec3((float)a[0].toDouble(), (float)a[1].toDouble(), (float)a[2].toDouble());
        };
        const QJsonArray q = pose.value("rotation").toArray();
        const glm::vec3 position = unity::positionFromUnity(vec("position"), metersPerUnit);
        const glm::quat rotation = unity::rotationFromUnity(glm::quat((float)q[3].toDouble(), (float)q[0].toDouble(),
                                                                      (float)q[1].toDouble(), (float)q[2].toDouble()));
        const glm::vec3 scale = vec("scale");
        glm::vec3 t, s;
        glm::quat r;
        unity::jointLocalPose(hfmModel->joints[joint], t, r, s);
        const float positionError = glm::length(position - t) * metersPerUnit;
        const float angle = glm::degrees(2.0f * acosf(std::min(1.0f, fabsf(glm::dot(glm::normalize(rotation), r)))));
        const float scaleError = glm::length(scale - s);
        if (positionError > worstPosition) {
            worstPosition = positionError;
            worstPositionPath = path;
        }
        if (angle > worstAngle) {
            worstAngle = angle;
            worstAnglePath = path;
        }
        if (scaleError > worstScale) {
            worstScale = scaleError;
            worstScalePath = path;
        }
        compared++;
    }
    print(QString("%1 objects compared (%2 not among the engine's joints); meters per file unit %3")
              .arg(compared).arg(missing).arg(metersPerUnit));
    print(QString("  position: worst %1 m (%2)").arg(worstPosition, 0, 'g', 3).arg(worstPositionPath));
    print(QString("  rotation: worst %1 deg (%2)").arg(worstAngle, 0, 'g', 3).arg(worstAnglePath));
    print(QString("  scale: worst %1 (%2)").arg(worstScale, 0, 'g', 3).arg(worstScalePath));
    return compared > 0 && worstPosition < 1e-4f && worstAngle < 0.01f && worstScale < 1e-4f ? 0 : 1;
}

// Checks the model-object ID rule (UnityIds.h) against Unity's own list of a model's objects, as
// a Unity editor script lists them: per line the ID, class, name and hierarchy path.
int checkIds(const QString& listPath) {
    QFile file(listPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        print("error: cannot read " + listPath);
        return 1;
    }
    int matched = 0, total = 0;
    QMap<QString, int> matchedByClass;
    for (const QString& line : QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts)) {
        const QStringList fields = line.split('\t');
        if (fields.size() < 4) {
            continue;
        }
        total++;
        const qint64 id = fields[0].toLongLong();
        const QString& className = fields[1];
        QStringList below = fields[3].split('/', Qt::SkipEmptyParts);
        const bool inHierarchy = !below.isEmpty();
        if (inHierarchy) {
            below.removeFirst();  // the model's root object, "root" in the path
        }
        const QString nodePath = unity::modelNodePath(below);
        qint64 expected;
        if (className == "GameObject") {
            expected = unity::modelGameObjectID(nodePath);
        } else if (inHierarchy) {
            expected = unity::modelComponentID(className, nodePath);
        } else if (className == "Avatar") {
            expected = unity::MODEL_AVATAR_ID;
        } else {
            expected = unity::modelObjectID(className, fields[2]);
        }
        if (expected == id) {
            matched++;
            matchedByClass[className]++;
        } else {
            print(QString("  differs: %1 %2 %3: Unity %4, rule %5").arg(className).arg(fields[2]).arg(fields[3]).arg(id)
                      .arg(expected));
        }
    }
    QStringList counts;
    for (auto it = matchedByClass.begin(); it != matchedByClass.end(); ++it) {
        counts.append(QString("%1 %2").arg(it.value()).arg(it.key()));
    }
    print(QString("%1 of %2 IDs as Unity gives them (%3)").arg(matched).arg(total).arg(counts.join(", ")));
    return matched == total ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    setupHifiApplication("Unity Avatar");
    QCoreApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription("Darkride's avatar import: reads a .unitypackage");
    parser.addHelpOption();
    const QCommandLineOption inputOption("i", "the .unitypackage", "file");
    const QCommandLineOption listOption("list", "list the package's assets");
    const QCommandLineOption objectsOption("objects", "print one text asset's objects as JSON", "asset path");
    const QCommandLineOption metaOption("meta", "print one asset's .meta as JSON", "asset path");
    const QCommandLineOption dumpOption("dump-json", "write every text asset's objects and every .meta as JSON "
                                        "files named by GUID into this folder", "folder");
    const QCommandLineOption inventoryOption("inventory", "summarize the avatars the package holds");
    const QCommandLineOption avatarsOption("avatars", "list the avatars (prefabs with an avatar descriptor)");
    const QCommandLineOption convertOption("convert", "write an avatar as a Darkride avatar (.fst, model, textures) "
                                           "into this folder", "folder");
    const QCommandLineOption avatarOption("avatar", "with --convert: which avatar (default: the first listed)", "name");
    const QCommandLineOption nameOption("name", "with --convert: the Darkride avatar's name (default: the avatar's)", "name");
    const QCommandLineOption checkOption("check-muscles", "check the muscle math against Unity's poses of the "
                                         "avatar's model (a JSON exported from Unity's editor)", "json");
    const QCommandLineOption checkIdsOption("check-ids", "check the model-object ID rule against Unity's list of a "
                                            "model's objects (as a Unity editor script lists them); needs no package", "list");
    const QCommandLineOption checkFramesOption("check-frames", "check the conversion of Unity's local transforms against "
                                               "Unity's import pose of the avatar's model (a JSON exported from Unity's editor)", "json");
    parser.addOptions({ inputOption, listOption, objectsOption, metaOption, dumpOption, inventoryOption, avatarsOption,
                        convertOption, avatarOption, nameOption, checkOption, checkIdsOption, checkFramesOption });
    parser.process(app);

    if (parser.isSet(checkIdsOption)) {
        return checkIds(parser.value(checkIdsOption));
    }

    if (!parser.isSet(inputOption)) {
        parser.showHelp(1);
    }
    unity::Package package;
    QString error;
    if (!package.open(parser.value(inputOption), &error)) {
        print("error: " + error);
        return 1;
    }

    if (parser.isSet(listOption)) {
        for (const auto& asset : package.assets()) {
            print(QString("%1\t%2\t%3").arg(asset.guid).arg(asset.hasData ? QString::number(asset.data.size()) : "-")
                      .arg(asset.path));
        }
    }
    if (parser.isSet(objectsOption) || parser.isSet(metaOption)) {
        const bool objects = parser.isSet(objectsOption);
        const auto* asset = package.byPath(parser.value(objects ? objectsOption : metaOption));
        if (!asset) {
            print("error: no such asset in the package");
            return 1;
        }
        const QVariant value = objects ? objectsToVariant(package.objects(asset->guid))
                                       : QVariant(package.meta(asset->guid));
        fputs(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Indented).constData(), stdout);
    }
    if (parser.isSet(dumpOption)) {
        const QDir folder(parser.value(dumpOption));
        QDir().mkpath(folder.path());
        int written = 0;
        for (const auto& asset : package.assets()) {
            if (!asset.meta.isEmpty()) {
                written += writeJson(folder.filePath(asset.guid + ".meta.json"), package.meta(asset.guid));
            }
            if (asset.hasData && isText(asset.path)) {
                written += writeJson(folder.filePath(asset.guid + ".objects.json"), objectsToVariant(package.objects(asset.guid)));
            }
        }
        print(QString("wrote %1 JSON files").arg(written));
    }
    if (parser.isSet(inventoryOption)) {
        inventory(package);
    }
    const QVector<avatarimport::AvatarSource> avatars = avatarimport::findAvatars(package);
    if (parser.isSet(avatarsOption)) {
        for (const auto& avatar : avatars) {
            print(QString("%1\t%2\t%3").arg(avatar.name, avatar.prefabPath, avatar.modelPath.isEmpty() ? "(no model)" : avatar.modelPath));
        }
    }
    if (parser.isSet(checkOption)) {
        if (avatars.isEmpty()) {
            print("error: no avatar in the package");
            return 1;
        }
        return checkMuscles(package, avatars.first(), parser.value(checkOption));
    }
    if (parser.isSet(checkFramesOption)) {
        if (avatars.isEmpty()) {
            print("error: no avatar in the package");
            return 1;
        }
        return checkFrames(package, avatars.first(), parser.value(checkFramesOption));
    }

    if (parser.isSet(convertOption)) {
        const QString wanted = parser.value(avatarOption);
        const avatarimport::AvatarSource* chosen = nullptr;
        for (const auto& avatar : avatars) {
            if (wanted.isEmpty() || avatar.name == wanted) {
                chosen = &avatar;
                break;
            }
        }
        if (!chosen) {
            print("error: no such avatar in the package");
            return 1;
        }
        const QString name = parser.isSet(nameOption) ? parser.value(nameOption) : chosen->name;
        const avatarimport::WrittenAvatar written = avatarimport::writeAvatar(package, *chosen, parser.value(convertOption), name);
        if (!written.ok) {
            print("error: " + written.error);
            return 1;
        }
        print("wrote " + written.fstPath);
        for (const QString& note : written.notes) {
            print("  " + note);
        }
    }
    if (!package.parseError().isEmpty()) {
        print("parse error: " + package.parseError());
        return 2;
    }
    return 0;
}
