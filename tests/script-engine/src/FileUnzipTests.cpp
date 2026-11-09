//
//  FileUnzipTests.cpp
//  tests/script-engine/src
//
//  Created by scripting on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "FileUnzipTests.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>

#include <quazip/quazip.h>
#include <quazip/quazipfile.h>
#include <quazip/quazipnewinfo.h>

#include <FileScriptingInterface.h>

QTEST_MAIN(FileUnzipTests)

namespace {

struct Entry {
    QString name;
    quint32 mode;       // the unix mode zip tools store in the high half of the external attributes
    QByteArray content; // a link entry's content is its target
};

// An archive written as zip tools write one: each entry with its unix mode (a folder, a file, a symbolic link)
bool writeArchive(const QString& zipPath, const QList<Entry>& entries) {
    QuaZip zip(zipPath);
    if (!zip.open(QuaZip::mdCreate)) {
        return false;
    }
    for (const Entry& entry : entries) {
        QuaZipFile out(&zip);
        QuaZipNewInfo info(entry.name);
        info.externalAttr = entry.mode << 16;
        if (!out.open(QIODevice::WriteOnly, info)) {
            return false;
        }
        out.write(entry.content);
        out.close();
        if (out.getZipError() != UNZ_OK) {
            return false;
        }
    }
    zip.close();
    return zip.getZipError() == UNZ_OK;
}

const quint32 FILE_RW = 0100644;
const quint32 FILE_RWX = 0100755;
const quint32 FOLDER_PRIVATE = 040700;
const quint32 FILE_PRIVATE = 0100600;
const quint32 LINK = 0120777;

}  // namespace

void FileUnzipTests::testPlainArchiveExtracts() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString zipPath = temp.filePath("plain.zip");
    QVERIFY(writeArchive(zipPath, { { "a.txt", FILE_RW, "A" }, { "sub/", FOLDER_PRIVATE, "" }, { "sub/b.txt", FILE_RW, "B" } }));
    const QString target = temp.filePath("out");
    const QStringList written = FileScriptingInterface::unzipArchive(zipPath, target);
    QVERIFY2(written.size() >= 2, qPrintable("the two files are listed; got " + written.join(", ")));
    QFile a(target + "/a.txt");
    QVERIFY(a.open(QIODevice::ReadOnly));
    QCOMPARE(a.readAll(), QByteArray("A"));
    QFile b(target + "/sub/b.txt");
    QVERIFY(b.open(QIODevice::ReadOnly));
    QCOMPARE(b.readAll(), QByteArray("B"));
}

void FileUnzipTests::testLinkEntryRefused() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString outside = temp.filePath("outside");
    QVERIFY(QDir().mkpath(outside));
    const QString zipPath = temp.filePath("link.zip");
    // "x" is a link to a folder outside the target; "x/evil.txt" stored under it would be written through the link
    QVERIFY(writeArchive(zipPath, { { "ok.txt", FILE_RW, "fine" }, { "x", LINK, outside.toUtf8() }, { "x/evil.txt", FILE_RW, "written through the link" } }));
    const QString target = temp.filePath("out");
    const QStringList written = FileScriptingInterface::unzipArchive(zipPath, target);
    QVERIFY2(written.isEmpty(), qPrintable("refused, nothing written; got " + written.join(", ")));
    QVERIFY2(!QDir(target).exists() || QDir(target).isEmpty(), "the target folder stays empty");
    QVERIFY2(!QFileInfo::exists(outside + "/evil.txt"), "nothing landed outside the target");
    QVERIFY2(!QFileInfo(target + "/x").isSymLink(), "no link was made");
}

void FileUnzipTests::testPermissionBitsNotKept() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString zipPath = temp.filePath("bits.zip");
    QVERIFY(writeArchive(zipPath, { { "tool.sh", FILE_RWX, "#!/bin/sh\n" }, { "sub/", FOLDER_PRIVATE, "" }, { "sub/secret.txt", FILE_PRIVATE, "s" } }));
    const QString target = temp.filePath("out");
    const QStringList written = FileScriptingInterface::unzipArchive(zipPath, target);
    QVERIFY(!written.isEmpty());
    const QFile::Permissions tool = QFile::permissions(target + "/tool.sh");
    QVERIFY2(!(tool & (QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther)), "a file stored 0755 lands without execute bits");
    QVERIFY2((tool & QFileDevice::ReadOther) && (tool & QFileDevice::WriteOwner), "a file lands 0644");
    const QFile::Permissions folder = QFile::permissions(target + "/sub");
    QVERIFY2((folder & QFileDevice::ExeOwner) && (folder & QFileDevice::ExeOther) && (folder & QFileDevice::ReadOther), "a folder stored 0700 lands 0755");
    const QFile::Permissions secret = QFile::permissions(target + "/sub/secret.txt");
    QVERIFY2((secret & QFileDevice::ReadOther) && !(secret & QFileDevice::ExeOwner), "a file stored 0600 lands 0644");
}

void FileUnzipTests::testEntryOutsideTargetRefused() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString zipPath = temp.filePath("climb.zip");
    QVERIFY(writeArchive(zipPath, { { "ok.txt", FILE_RW, "fine" }, { "../climb.txt", FILE_RW, "outside" } }));
    const QString target = temp.filePath("out");
    QVERIFY(FileScriptingInterface::unzipArchive(zipPath, target).isEmpty());
    QVERIFY(!QFileInfo::exists(temp.filePath("climb.txt")));
    QVERIFY(!QFileInfo::exists(target + "/ok.txt"));
}
