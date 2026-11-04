//
//  FileScriptingInterface.cpp
//  libraries/script-engine/src
//
//  Created by Elisa Lupin-Jimenez on 6/28/16.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "FileScriptingInterface.h"

#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QDebug>
#include <QtCore/QBuffer>
#include <QtCore/QIODevice>
#include <QtCore/QUrl>
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QFileInfo>

// FIXME quazip hasn't been built on the android toolchain
#if !defined(Q_OS_ANDROID)
#include <quazip5/quazip.h>
#include <quazip5/JlCompress.h>
#endif

#include "ResourceManager.h"
#include "ScriptEngineLogging.h"


FileScriptingInterface::FileScriptingInterface(QObject* parent) : QObject(parent) {
    // nothing for now
}

void FileScriptingInterface::runUnzip(QString path, QUrl url, bool autoAdd, bool isZip, bool isBlocks) {
    QString fileName = "/" + path.section("/", -1);
    QString tempDir = path;
    if (!isZip) {
        tempDir.remove(fileName);
    } else {
        QTemporaryDir zipTemp;
        tempDir = zipTemp.path();
        path.remove("file:///");
    }
    
    qCDebug(scriptengine) << "Temporary directory at: " + tempDir;
    if (!isTempDir(tempDir)) {
        qCDebug(scriptengine) << "Temporary directory mismatch; risk of losing files";
        return;
    }

    QStringList fileList = unzipFile(path, tempDir);
    
    if(fileList.isEmpty()) {
        qCDebug(scriptengine) << "Unzip failed";
    }

    if (path.contains("vr.google.com/downloads")) {
        isZip = true;
    }
    if (!hasModel(fileList)) {
        isZip = false;
    }

    emit unzipResult(path, fileList, autoAdd, isZip, isBlocks);

}

QStringList FileScriptingInterface::unzipFile(QString path, QString tempDir) {
#if defined(Q_OS_ANDROID)
    // FIXME quazip hasn't been built on the android toolchain
    return QStringList();
#else
    QDir dir(path);
    QString dirName = dir.path();
    qCDebug(scriptengine) << "Directory to unzip: " << dirName;
    QString target = tempDir + "/model_repo";

    // every entry has to land inside the target folder: an archive whose entry names climb out of it (../, an absolute
    // path, a drive letter) would write wherever they point, so such an archive is refused whole
    const QString targetPath = QDir::cleanPath(QDir(target).absolutePath());
    const QString targetRoot = targetPath + "/";
    const QStringList entries = JlCompress::getFileList(dirName);
    for (const QString& entry : entries) {
        const QString landing = QDir::cleanPath(QDir(target).absoluteFilePath(entry));
        if (entry.contains('\\') || entry.contains(':') || (landing != targetPath && !landing.startsWith(targetRoot))) {
            qCWarning(scriptengine) << "Refused to unzip" << dirName << ": the entry" << entry.left(200)
                                    << "would land outside the target folder";
            return QStringList();
        }
    }

    QStringList list = JlCompress::extractDir(dirName, target);

    qCDebug(scriptengine) << list;

    if (!list.isEmpty()) {
        return list;
    } else {
        qCDebug(scriptengine) << "Extraction failed";
        return list;
    }
#endif
}

// fix to check that we are only referring to a temporary directory
bool FileScriptingInterface::isTempDir(QString tempDir) {
    QString folderName = "/" + tempDir.section("/", -1);
    QString tempContainer = tempDir;
    tempContainer.remove(folderName);
    QTemporaryDir test;
    QString testDir = test.path();
    folderName = "/" + testDir.section("/", -1);
    QString testContainer = testDir;
    testContainer.remove(folderName);
    return (testContainer == tempContainer);
}

bool FileScriptingInterface::hasModel(QStringList fileList) {
    for (int i = 0; i < fileList.size(); i++) {
        if (fileList.at(i).toLower().contains(".fbx") || fileList.at(i).toLower().contains(".obj")) {
            return true;
        }
    }
    return false;
}

QString FileScriptingInterface::getTempDir() {
    QTemporaryDir dir;
    dir.setAutoRemove(false);
    return dir.path();
    // do something to delete this temp dir later
}

QString FileScriptingInterface::convertUrlToPath(QUrl url) {
    QString newUrl;
    QString oldUrl = url.toString();
    newUrl = oldUrl.section("filename=", 1, 1);
    return newUrl;
}

// this function is not in use
void FileScriptingInterface::downloadZip(QString path, const QString link) {
    QUrl url = QUrl(link);
    auto request = DependencyManager::get<ResourceManager>()->createResourceRequest(
        nullptr, url, true, -1, "FileScriptingInterface::downloadZip");
    connect(request, &ResourceRequest::finished, this, [this, path]{
        unzipFile(path, ""); // so intellisense isn't mad
    });
    request->send();
}

// this function is not in use
void FileScriptingInterface::recursiveFileScan(QFileInfo file, QString* dirName) {
    /*if (!file.isDir()) {
        return;
    }*/
    QFileInfoList files;
    // FIXME quazip hasn't been built on the android toolchain
#if !defined(Q_OS_ANDROID)
    if (file.fileName().contains(".zip")) {
        JlCompress::extractDir(file.fileName());
    }
#endif
    files = file.dir().entryInfoList();

    /*if (files.empty()) {
        files = JlCompress::getFileList(file.fileName());
    }*/

    foreach (QFileInfo file, files) {
        recursiveFileScan(file, dirName);
    }
    return;
}
