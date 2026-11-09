//
//  FileUnzipTests.h
//  tests/script-engine/src
//
//  The File API's unzip: an archive with a symbolic link entry is refused whole and nothing is written; the archive's
//  permission bits are not kept (files 0644, folders 0755); a plain archive extracts.
//
//  Created by scripting on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_FileUnzipTests_h
#define hifi_FileUnzipTests_h

#include <QtCore/QObject>
#include <QtTest/QtTest>

class FileUnzipTests : public QObject {
    Q_OBJECT
private slots:
    void testPlainArchiveExtracts();
    void testLinkEntryRefused();
    void testPermissionBitsNotKept();
    void testEntryOutsideTargetRefused();
};

#endif // hifi_FileUnzipTests_h
