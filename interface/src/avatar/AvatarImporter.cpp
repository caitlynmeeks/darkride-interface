//
//  AvatarImporter.cpp
//  interface/src/avatar
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AvatarImporter.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <AccountManager.h>
#include <AssimpSerializer.h>
#include <AutoRig.h>
#include <AvatarWriter.h>
#include <FBXSerializer.h>
#include <GLMHelpers.h>
#include <MainWindow.h>
#include <NetworkAccessManager.h>
#include <OBJSerializer.h>
#include <PathUtils.h>
#include <SharedUtil.h>
#include <StandardSkeleton.h>
#include <UnityPackage.h>
#include <UnityAvatar.h>

#include "Application.h"
#include "AvatarManager.h"
#include "AvatarThumbnails.h"
#include "InterfaceLogging.h"
#include "MyAvatar.h"
#include "editor/EditorCreate.h"
#include "ui/Popup.h"

namespace {

const float USUAL_HEIGHT = 1.70f;

// The joints a rig needs before Darkride's animations can drive it.
const char* const NEEDED_JOINTS[] = { "Hips", "Spine", "Spine1", "Neck", "Head",
                                      "LeftArm", "LeftForeArm", "LeftHand", "RightArm", "RightForeArm", "RightHand",
                                      "LeftUpLeg", "LeftLeg", "LeftFoot", "RightUpLeg", "RightLeg", "RightFoot" };

enum class RigKind { None, Standard, Other };

struct Import {
    QString path;
    QString name;
    HFMModel::Pointer model;
    autorig::Body body;
    RigKind rig { RigKind::None };
    QString why;  // for RigKind::Other: what keeps the animations from driving it
};
using ImportPointer = std::shared_ptr<Import>;

HFMModel::Pointer readModel(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return nullptr;
    }
    const QByteArray data = file.readAll();
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    const QUrl url = QUrl::fromLocalFile(info.absoluteFilePath());
    const hifi::VariantMultiHash mapping;
    if (suffix == "fbx") {
        return FBXSerializer().read(data, mapping, url);
    }
    if (suffix == "obj") {
        return OBJSerializer().read(data, mapping, url);
    }
    // glTF too, as the model cache reads it (GLTFSerializer ignores interleaved vertex buffers, as Meshy's are)
    return AssimpSerializer().read(data, mapping, url);
}

// None: no skin. Standard: the needed joints, each with Mixamo's axes (its Y axis along its bone), so Darkride's
// animations drive it as it is. Other: a skeleton of its own that the animations would drive wrongly (AnimClip.cpp
// copies each joint's absolute rotation onto the avatar).
RigKind classify(const HFMModel& model, QString& why) {
    if (model.skinDeformers.empty()) {
        return RigKind::None;
    }
    std::map<QString, int> byName;
    for (int j = 0; j < (int)model.joints.size(); j++) {
        byName.emplace(model.joints[j].name.section(':', -1).toLower(), j);
    }
    const autorig::StandardSkeleton& standard = autorig::StandardSkeleton::get();
    for (const char* name : NEEDED_JOINTS) {
        auto found = byName.find(QString(name).toLower());
        if (found == byName.end()) {
            why = QString("it has no %1 joint").arg(name);
            return RigKind::Other;
        }
        const int child = standard.boneChild(standard.indexOf(name));
        auto childFound = child >= 0 ? byName.find(QString(standard.joint(child).name).toLower()) : byName.end();
        if (childFound == byName.end()) {
            continue;
        }
        const glm::mat4& joint = model.joints[found->second].globalTransform;
        const glm::vec3 bone = extractTranslation(model.joints[childFound->second].globalTransform) - extractTranslation(joint);
        const glm::vec3 axis = glm::mat3(joint) * glm::vec3(0.0f, 1.0f, 0.0f);
        if (glm::length(bone) > 0.0f && glm::length(axis) > 0.0f &&
            glm::dot(glm::normalize(axis), glm::normalize(bone)) < 0.9f) {
            why = QString("its %1 joint is not turned the way Darkride's animations expect").arg(name);
            return RigKind::Other;
        }
    }
    return RigKind::Standard;
}

QString avatarName(const QString& path) {
    QString name = QFileInfo(path).completeBaseName();
    name.replace(QRegularExpression("[^A-Za-z0-9_-]+"), "_");
    return name.isEmpty() ? QString("avatar") : name;
}

// A new folder for each import (name, name-2, name-3...), so the client never shows a cached copy of an earlier one.
QString avatarFolder(const QString& name) {
    const QString base = PathUtils::getAppDataPath() + "Avatars/" + name;
    QString folder = base;
    for (int n = 2; QDir(folder).exists(); n++) {
        folder = base + "-" + QString::number(n);
    }
    return folder;
}

// The importer's alerts and banner are Darkride's popups (ui/Popup.h), in the design language Caity chose for all of
// them (10-18); appIcon is popup's.
using popup::appIcon;

// An error from below as a sentence of its own: "it was cut" becomes "It was cut."
QString sentence(QString text) {
    text = text.trimmed();
    if (!text.isEmpty()) {
        text[0] = text[0].toUpper();
        if (!text.endsWith('.')) {
            text += '.';
        }
    }
    return text;
}

// One of the importer's questions. Returns the chosen button's place in the list, or -1. With rightsLine, the rights
// line sits under the buttons: one of them puts the avatar where others see it.
int ask(const QString& title, const QString& text, const std::vector<popup::Button>& buttons,
        const QString& details = QString(), bool rightsLine = false) {
    qCDebug(interfaceapp) << "Import Avatar: asking" << title;
    popup::Alert alert(appIcon(), title, text, details, buttons, qApp->getWindow());
    if (rightsLine) {
        alert.addRightsLine();
    }
    alert.exec();
    const int answer = alert.answer();
    qCDebug(interfaceapp) << "Import Avatar: answered" << (answer < 0 ? QString("nothing") : buttons[answer].label);
    return answer;
}

void warn(const QString& title, const QString& text = QString()) {
    qCDebug(interfaceapp) << "Import Avatar: warning" << title << text;
    popup::Alert alert(appIcon(), title, text, QString(), { { "OK", popup::Role::Default } }, qApp->getWindow());
    alert.exec();
}

// What went right needs no answer: a banner says it and goes.
void announce(const QString& title, const QString& text) {
    qCDebug(interfaceapp) << "Import Avatar: telling" << title << text;
    new popup::Banner(appIcon(), title, text, qApp->getWindow());
}

// A line before and after each step, so a client that stops here shows in its log the step it stopped in (a freeze
// right after "Wear it", 10-13, left no trace).
void wearUrl(const QString& url, const QString& name) {
    qCDebug(interfaceapp) << "Import Avatar: putting on" << url;
    DependencyManager::get<AvatarManager>()->getMyAvatar()->useFullAvatarURL(url, name);
    qCDebug(interfaceapp) << "Import Avatar: avatar change started";
    emit qApp->fullAvatarURLChanged(url, name);
    qCDebug(interfaceapp) << "Import Avatar: wear done";
}

void wear(const QString& fstPath, const QString& name) {
    wearUrl(QUrl::fromLocalFile(fstPath).toString(), name);
}

// An avatar on this computer goes out to others as a blank address (AvatarData::getWireSafeSkeletonModelURL), so they
// see the default. In My Files it has a web address others' apps load: the model goes up with the files it names
// beside it (game rips name their textures; anything else in the folder stays here), then the .fst with its filename
// line pointing beside it; the folder is made private, and the client wears that copy. One file at a time, through
// the editor's upload. Its picture (AvatarThumbnails: a snap of it, worn, from the front) goes up before the .fst,
// which names it on a `preview` line.
struct Share {
    QString name;
    QString fstName;
    QString remoteFolder;
    QStringList fstLines;
    int modelLine { -1 };
    bool modelIsLocal { false };
    QStringList files;  // local paths, the model first
    int next { 0 };
    QByteArray picture;  // a JPEG, or empty when there is none
    QString pictureName;
    bool pictureSent { false };
};
using SharePointer = std::shared_ptr<Share>;

// The web copy is worn only once its .fst reads back from the web: a failed avatar load falls back to the default
// without a word, and the files host sent every .fst labelled gzip over plain text until metaverse 19f3c7d (10-13).
// The folder is private by now, so the check carries the player's login, as the avatar's own load does
// (HTTPResourceRequest); the files host passes a private file through itself, never redirecting it to the CDN, so the
// check follows no redirect and the login goes nowhere else.
void wearOnceItReadsBack(const SharePointer& share, const QString& url) {
    QNetworkRequest request { QUrl(url) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    const auto& token = DependencyManager::get<AccountManager>()->getAccountInfo().getAccessToken();
    if (!token.token.isEmpty() && !token.isExpired()) {
        request.setRawHeader(ACCESS_TOKEN_AUTHORIZATION_HEADER, token.authorizationHeaderValue());
    }
    // A check, so it neither reads nor writes the disk cache: Qt unpacks the gzip itself here and would cache the
    // unpacked body under the gzip label, which the avatar's own load (HTTPResourceRequest) then fails to unpack.
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    QNetworkReply* reply = NetworkAccessManager::getInstance().get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, share, url]() {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || !body.contains("filename")) {
            const QString why = reply->error() != QNetworkReply::NoError ? reply->errorString()
                                                                         : QString("it came back different");
            warn("Couldn't wear " + share->name + " from My Files",
                 "Its file doesn't read back from the web (" + why + "). You're still wearing the copy on this computer.");
            return;
        }
        qCDebug(interfaceapp) << "Import Avatar:" << share->name << "reads back from" << url;
        wearUrl(url, share->name);
        announce(share->name + " is in My Files",
                 "You're wearing it. It's private: people see it on you in Darkride, but it has no public link.");
    });
}

// Wear & Keep's folder is private (the server's tea-only flag): its files go only to signed-in Darkride apps, so
// people in a world see the avatar on its owner, but it has no public link (Caity 10-28, "safe wear"). The server
// sets a whole folder at once, and its call is a switch: a new folder's files are all public, so one call makes them
// private, and a second goes only if the answer says the folder went the other way.
void makePrivateThenWear(const SharePointer& share, const QString& url, bool again = false) {
    QNetworkRequest request = DependencyManager::get<AccountManager>()->createRequest("/api/user/files/folder/tea-only",
                                                                                      AccountManagerAuth::Required);
    const QString leftPublic = "It's in My Files, but anyone with its link could download it: make it private there, "
                               "or delete it. You're still wearing the copy on this computer.";
    if (request.url().isEmpty()) {
        warn("Couldn't make " + share->name + " private", "You're signed out. " + leftPublic);
        return;
    }
    QUrl requestUrl = request.url();
    requestUrl.setQuery("path=" + QString::fromLatin1(QUrl::toPercentEncoding(share->remoteFolder)), QUrl::StrictMode);
    request.setUrl(requestUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = NetworkAccessManager::getInstance().post(request, QByteArray());
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, share, url, again, leftPublic]() {
        reply->deleteLater();
        const bool answered = reply->error() == QNetworkReply::NoError;
        const bool isPrivate = answered && QJsonDocument::fromJson(reply->readAll()).object().value("tea").toBool();
        qCDebug(interfaceapp) << "Import Avatar:" << share->name << "folder" << share->remoteFolder
                              << (!answered ? "not made private: " + reply->errorString()
                                            : isPrivate ? QString("is private") : QString("came back public"));
        if (isPrivate) {
            wearOnceItReadsBack(share, url);
        } else if (answered && !again) {
            makePrivateThenWear(share, url, true);
        } else {
            warn("Couldn't make " + share->name + " private", leftPublic);
        }
    });
}

void shareNext(const SharePointer& share) {
    const bool fileNow = share->next < share->files.size();
    const bool pictureNow = !fileNow && !share->picture.isEmpty() && !share->pictureSent;
    const bool fstNow = !fileNow && !pictureNow;
    auto upload = new EditorUpload();
    QString error;
    bool started;
    if (fileNow) {
        const QFileInfo file(share->files[share->next]);
        if (share->next == 0 && share->modelIsLocal && file.suffix().toLower() == "vrm") {
            // a VRM is a binary glTF: it goes up named .glb, the name every version of the app reads (before 1.1.3 a
            // .vrm reached no reader, and people on those versions would see the default avatar)
            QFile vrm(file.absoluteFilePath());
            started = vrm.open(QIODevice::ReadOnly);
            if (started) {
                started = upload->startData(vrm.readAll(), file.completeBaseName() + ".glb", share->remoteFolder,
                                            "model/gltf-binary", error);
            } else {
                error = "cannot read " + file.fileName();
            }
        } else {
            started = upload->start(share->files[share->next], share->remoteFolder, error);
        }
        if (started && share->next == 0 && share->modelIsLocal) {
            share->fstLines[share->modelLine] = "filename = " + upload->fileName();
        }
        share->next++;
    } else if (pictureNow) {
        started = upload->startData(share->picture, share->pictureName, share->remoteFolder, "image/jpeg", error);
        share->pictureSent = true;
    } else {
        started = upload->startData(share->fstLines.join('\n').toUtf8(), share->fstName, share->remoteFolder,
                                    "text/plain", error);
    }
    if (!started && pictureNow) {
        delete upload;
        qCDebug(interfaceapp) << "Import Avatar:" << share->name << "picture not sent:" << error;
        shareNext(share);  // the avatar goes up without its picture
        return;
    }
    if (!started) {
        delete upload;
        warn("Couldn't put " + share->name + " in My Files", sentence(error));
        return;
    }
    QObject::connect(upload, &EditorUpload::failed, upload, [upload, share, pictureNow](const QString& error) {
        upload->deleteLater();
        if (pictureNow) {
            qCDebug(interfaceapp) << "Import Avatar:" << share->name << "picture not sent:" << error;
            shareNext(share);
            return;
        }
        warn("Couldn't put " + share->name + " in My Files", sentence(error));
    });
    QObject::connect(upload, &EditorUpload::finished, upload, [upload, share, fstNow](const QString& url) {
        upload->deleteLater();
        if (!fstNow) {
            shareNext(share);
            return;
        }
        qCDebug(interfaceapp) << "Import Avatar:" << share->name << "is in My Files at" << url;
        makePrivateThenWear(share, url);
    });
}

void shareAvatar(const QString& fstPath, const QString& name) {
    QFile fst(fstPath);
    if (!fst.open(QIODevice::ReadOnly)) {
        warn("Couldn't read " + QFileInfo(fstPath).fileName(), fstPath);
        return;
    }
    auto share = std::make_shared<Share>();
    share->name = name;
    share->fstName = QFileInfo(fstPath).fileName();
    share->remoteFolder = "/avatars/" + name + "-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QString modelPath;
    const QString fstText = QString::fromUtf8(fst.readAll());
    for (const QString& line : fstText.split('\n')) {
        const QString key = line.section('=', 0, 0).trimmed();
        if (key == "texdir") {
            continue;  // the textures go up beside the model, where it looks for them anyway
        }
        if (key == "filename" && share->modelLine < 0) {
            const QString value = line.section('=', 1).trimmed();
            const QUrl url(value);
            if (url.isLocalFile()) {
                modelPath = url.toLocalFile();
            } else if (url.isRelative()) {
                modelPath = QFileInfo(fstPath).dir().absoluteFilePath(value);
            }
            share->modelLine = share->fstLines.size();
        }
        share->fstLines.append(line);
    }
    if (!modelPath.isEmpty()) {
        QFile model(modelPath);
        if (!model.open(QIODevice::ReadOnly)) {
            warn("Couldn't read " + QFileInfo(modelPath).fileName(), modelPath);
            return;
        }
        const QByteArray bytes = model.readAll();
        const QFileInfo modelInfo(modelPath);
        share->files.append(modelInfo.absoluteFilePath());
        share->modelIsLocal = true;
        // The files beside the model that it names (game rips name their textures) or that the .fst names (a
        // converted avatar's material map names its textures).
        for (const QFileInfo& file : modelInfo.dir().entryInfoList(QDir::Files)) {
            if (file != modelInfo && file.suffix().toLower() != "fst" &&
                (bytes.contains(file.fileName().toUtf8()) || fstText.contains("\"" + file.fileName() + "\""))) {
                share->files.append(file.absoluteFilePath());
            }
        }
    }
    qCDebug(interfaceapp) << "Import Avatar: putting" << name << "in My Files," << share->files.size()
                          << "files and the .fst, into" << share->remoteFolder;
    // its picture first, while it is worn (Wear & Keep has only just put it on: the picture waits for it to load)
    share->pictureName = AvatarThumbnails::pictureName(QFileInfo(share->fstName).completeBaseName());
    DependencyManager::get<AvatarThumbnails>()->capture([share](const QByteArray& jpeg, const QString& error) {
        if (jpeg.isEmpty()) {
            qCDebug(interfaceapp) << "Import Avatar:" << share->name << "no picture:" << error;
        } else {
            share->picture = jpeg;
            share->fstLines.erase(std::remove_if(share->fstLines.begin(), share->fstLines.end(), [](const QString& line) {
                return line.section('=', 0, 0).trimmed() == "preview";
            }), share->fstLines.end());
            share->fstLines.append("preview = " + share->pictureName);
        }
        shareNext(share);
    });
}

// Asked once an avatar is ready. Test Locally (wear it from this computer; nothing leaves it) is the default, so a
// press of Return never uploads a bought avatar (Caity 10-28, "safe wear"); Wear & Keep also puts it in My Files,
// private. The importer's notes go in the details.
void offerToWear(const QString& fstPath, const QString& name, const QString& notes) {
    const int answer = ask(name + " is ready",
                           "Try it on this computer, or wear it and keep it in My Files. A kept avatar is private: "
                           "people see it on you in Darkride, but it has no public link.",
                           { { "Test Locally", popup::Role::Default },
                             { "Wear && Keep", popup::Role::Plain },
                             { "Not Now", popup::Role::Cancel } },
                           notes, true);
    if (answer == 0 || answer == 1) {
        wear(fstPath, name);
    }
    if (answer == 1) {
        shareAvatar(fstPath, name);
    }
}

// The rigging and saving happen off the main thread; the answer comes back to it.
void autoRig(const ImportPointer& import, float scale) {
    QtConcurrent::run([import, scale]() {
        autorig::Body body = import->body;
        if (scale != 1.0f) {
            body.scale(scale);
        }
        const autorig::Result result = autorig::rig(body);
        autorig::WrittenAvatar written;
        if (result.ok) {
            written = autorig::writeAvatar(*import->model, body, result, import->path, avatarFolder(import->name), import->name);
        }
        for (const QString& note : written.notes) {
            qCDebug(interfaceapp) << "Import Avatar:" << import->name << "note:" << note;
        }
        qCDebug(interfaceapp) << "Import Avatar:" << import->name
                              << (!result.ok ? "not rigged:" : !written.ok ? "rigged, not saved:" : "rigged and saved as")
                              << (!result.ok ? result.error : !written.ok ? written.error : written.fstPath);
        QMetaObject::invokeMethod(qApp, [import, result, written]() {
            if (!result.ok) {
                warn("Couldn't rig " + import->name, sentence(result.error));
            } else if (!written.ok) {
                warn("Couldn't save " + import->name, sentence(written.error));
            } else {
                offerToWear(written.fstPath, import->name,
                            "Rigged with Darkride's standard skeleton.\n\nSaved in " +
                                QFileInfo(written.fstPath).absolutePath() + ".");
            }
        }, Qt::QueuedConnection);
    });
}

void checkHeightThenRig(const ImportPointer& import) {
    const float height = import->body.maximum.y - import->body.minimum.y;
    if (height > 2.4f || height < 0.6f) {
        const int answer = ask(QString("%1 is %2 m tall").arg(import->name).arg(height, 0, 'f', 2),
                               "That looks like a units mistake in its file.",
                               { { QString("Make It %1 m").arg(USUAL_HEIGHT, 0, 'f', 2), popup::Role::Default },
                                 { QString("Keep %1 m").arg(height, 0, 'f', 2), popup::Role::Plain },
                                 { "Cancel", popup::Role::Cancel } });
        if (answer == 0) {
            autoRig(import, USUAL_HEIGHT / height);
        } else if (answer == 1) {
            autoRig(import, 1.0f);
        }
    } else {
        autoRig(import, 1.0f);
    }
}

void decide(const ImportPointer& import) {
    if (!import->model || import->body.triangles.empty()) {
        warn("Couldn't read " + QFileInfo(import->path).fileName(), "Darkride found no character in it.");
        return;
    }
    switch (import->rig) {
        case RigKind::Standard: {
            const int answer = ask(import->name + " already has Darkride's skeleton",
                                   "Keep its own rig, or replace it with an automatic one. The file itself doesn't change.",
                                   { { "Keep Rig", popup::Role::Default },
                                     { "Replace Rig", popup::Role::Plain },
                                     { "Cancel", popup::Role::Cancel } });
            if (answer == 0) {
                const QString folder = avatarFolder(import->name);
                QDir().mkpath(folder);
                const QString fstPath = QDir(folder).filePath(import->name + ".fst");
                if (autorig::writeFst(fstPath, import->name, QUrl::fromLocalFile(import->path).toString())) {
                    wear(fstPath, import->name);
                } else {
                    warn("Couldn't write " + QFileInfo(fstPath).fileName(), fstPath);
                }
            } else if (answer == 1) {
                checkHeightThenRig(import);
            }
            break;
        }
        case RigKind::Other:
            if (ask(import->name + "'s skeleton won't move right",
                    sentence("because " + import->why) + " Darkride can replace its rig; the file itself doesn't change.",
                    { { "Replace Rig", popup::Role::Default }, { "Cancel", popup::Role::Cancel } }) == 0) {
                checkHeightThenRig(import);
            }
            break;
        case RigKind::None:
            if (ask(import->name + " has no skeleton yet", "Darkride can rig it with its standard skeleton.",
                    { { "Auto Rig", popup::Role::Default }, { "Cancel", popup::Role::Cancel } }) == 0) {
                checkHeightThenRig(import);
            }
            break;
    }
}

// A .unitypackage (as avatar shops sell avatars): the avatars in it, converted with libraries/unity-import. The package is
// read and the avatar written off the main thread; the questions come back to it.
void convertPackageAvatar(const std::shared_ptr<unity::Package>& package, const avatarimport::AvatarSource& avatar,
                         const QString& packageName) {
    QtConcurrent::run([package, avatar, packageName]() {
        const QString name = avatarName(avatar.name);
        const avatarimport::WrittenAvatar written = avatarimport::writeAvatar(*package, avatar, avatarFolder(name), name);
        for (const QString& note : written.notes) {
            qCDebug(interfaceapp) << "Import Avatar:" << name << "note:" << note;
        }
        qCDebug(interfaceapp) << "Import Avatar:" << name << (written.ok ? "converted and saved as" : "not converted:")
                              << (written.ok ? written.fstPath : written.error);
        QMetaObject::invokeMethod(qApp, [written, name, packageName]() {
            if (!written.ok) {
                warn("Couldn't convert " + name, sentence(written.error));
                return;
            }
            QString notes = "From " + packageName + ".\n\n";
            QStringList notYet;
            for (const QString& note : written.notes) {
                if (note.startsWith("not yet: ")) {
                    notYet << "- " + note.mid(9);
                }
            }
            if (!notYet.isEmpty()) {
                notes += "Not carried over yet:\n" + notYet.join("\n") + "\n\n";
            }
            offerToWear(written.fstPath, name, notes + "Saved in " + QFileInfo(written.fstPath).absolutePath() + ".");
        }, Qt::QueuedConnection);
    });
}

void importUnityPackage(const QString& path) {
    QtConcurrent::run([path]() {
        auto package = std::make_shared<unity::Package>();
        QString error;
        const bool opened = package->open(path, &error);
        const QVector<avatarimport::AvatarSource> avatars = opened ? avatarimport::findAvatars(*package) : QVector<avatarimport::AvatarSource>();
        qCDebug(interfaceapp) << "Import Avatar: read" << path << (opened ? QString("%1 avatars").arg(avatars.size()) : error);
        QMetaObject::invokeMethod(qApp, [package, avatars, path, opened, error]() {
            const QString packageName = QFileInfo(path).fileName();
            if (!opened) {
                warn("Couldn't read " + packageName, sentence(error));
                return;
            }
            if (avatars.isEmpty()) {
                warn(packageName + " holds no avatar",
                     "An outfit or hair package goes onto its avatar, which Darkride can't do yet.");
                return;
            }
            int chosen = 0;
            if (avatars.size() > 1) {
                // findAvatars puts a PC version before the phone ones: two such are versions of one avatar
                bool firstIsPC = true;
                for (int i = 0; i < avatars.size(); i++) {
                    const QString lower = avatars[i].name.toLower();
                    const bool forPhones = lower.contains("mobile") || lower.contains("quest") || lower.contains("android");
                    if (forPhones == (i == 0)) {
                        firstIsPC = false;  // the first is for phones, or another one isn't
                    }
                }
                QString title;
                std::vector<popup::Button> buttons;
                if (avatars.size() == 2 && firstIsPC) {
                    title = avatars[0].name + " comes in two versions";
                    buttons = { { "PC Version", popup::Role::Default }, { "Mobile Version", popup::Role::Plain } };
                } else {
                    title = QString("%1 holds %2 avatars").arg(packageName).arg(avatars.size());
                    for (int i = 0; i < avatars.size() && i < 3; i++) {
                        buttons.push_back({ QString(avatars[i].name).replace("&", "&&"),
                                            i == 0 ? popup::Role::Default : popup::Role::Plain });
                    }
                }
                buttons.push_back({ "Cancel", popup::Role::Cancel });
                const int answer = ask(title, "Which one do you want to import?", buttons);
                if (answer < 0 || answer >= (int)buttons.size() - 1) {
                    return;
                }
                chosen = answer;
            }
            convertPackageAvatar(package, avatars[chosen], packageName);
        }, Qt::QueuedConnection);
    });
}

}

void AvatarImporter::importFile(const QString& path) {
    const QString name = avatarName(path);
    if (QFileInfo(path).suffix().toLower() == "fst") {
        offerToWear(path, name, "From " + path + ".");
        return;
    }
    if (QFileInfo(path).suffix().toLower() == "unitypackage") {
        importUnityPackage(path);
        return;
    }
    auto import = std::make_shared<Import>();
    import->path = path;
    import->name = name;
    QtConcurrent::run([import]() {
        import->model = readModel(import->path);
        if (import->model) {
            import->body = autorig::gatherBody(*import->model);
            const QString eyelids = autorig::openEyelids(import->body, *import->model);
            if (!eyelids.isEmpty()) {
                qCDebug(interfaceapp) << "Import Avatar:" << eyelids;
            }
            import->rig = classify(*import->model, import->why);
        }
        qCDebug(interfaceapp) << "Import Avatar: read" << import->path << (import->model ? "" : "(not readable)");
        QMetaObject::invokeMethod(qApp, [import]() {
            decide(import);
        }, Qt::QueuedConnection);
    });
}

void AvatarImporter::importLink(const QUrl& url) {
    if (url.isLocalFile()) {
        importFile(url.toLocalFile());
        return;
    }
    const QString fileName = QFileInfo(url.path()).fileName();
    announce("Downloading " + fileName, "It's imported once it's here.");
    QNetworkRequest request { url };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, TIVOLI_CLOUD_VR_USER_AGENT);
    QNetworkReply* reply = NetworkAccessManager::getInstance().get(request);
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, url, fileName]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qCDebug(interfaceapp) << "Import Avatar: download failed" << url << reply->errorString();
            warn("Couldn't download " + fileName, sentence(reply->errorString()));
            return;
        }
        // kept beside the imported avatars, a new name each time, as avatarFolder does
        const QString folder = PathUtils::getAppDataPath() + "Avatars/Downloads/";
        QDir().mkpath(folder);
        const QString base = avatarName(fileName);
        const QString suffix = "." + QFileInfo(fileName).suffix();
        QString path = folder + base + suffix;
        for (int n = 2; QFileInfo::exists(path); n++) {
            path = folder + base + "-" + QString::number(n) + suffix;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(reply->readAll()) < 0) {
            warn("Couldn't save " + fileName, sentence(file.errorString()));
            return;
        }
        file.close();
        qCDebug(interfaceapp) << "Import Avatar: downloaded" << url << "to" << path;
        importFile(path);
    });
}

void AvatarImporter::putInMyFiles(const QUrl& fstUrl) {
    if (!fstUrl.isLocalFile()) {
        if (fstUrl.scheme() == "qrc") {
            announce("Nothing to put in My Files", "The default avatar is in every viewer already.");
        } else {
            announce("Your avatar is on the web already", "Everyone can see it.");
        }
        return;
    }
    shareAvatar(fstUrl.toLocalFile(), avatarName(fstUrl.toLocalFile()));
}
