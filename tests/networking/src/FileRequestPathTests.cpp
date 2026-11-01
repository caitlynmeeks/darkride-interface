//
//  FileRequestPathTests.cpp
//
//  Created by servers on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "FileRequestPathTests.h"

#include <FileResourceRequest.h>

QTEST_MAIN(FileRequestPathTests)

void FileRequestPathTests::networkPaths_data() {
    QTest::addColumn<QString>("url");
    QTest::addColumn<bool>("isNetwork");
    QTest::newRow("a host") << "file://host/share/x.js" << true;
    QTest::newRow("localhost, a share on Windows") << "file://localhost/c:/x.js" << true;
    QTest::newRow("four slashes") << "file:////host/share/x.js" << true;
    QTest::newRow("encoded backslash") << "file:///%5Chost/share/x.js" << true;
    QTest::newRow("two encoded backslashes") << "file:///%5C%5Chost/share/x.js" << true;
    QTest::newRow("encoded slash") << "file:///%2Fhost/share/x.js" << true;
    QTest::newRow("a device path") << "file:///%5C%5C?%5CUNC%5Chost%5Cshare" << true;
    QTest::newRow("a local file") << "file:///Users/someone/x.js" << false;
    QTest::newRow("a Windows drive") << "file:///C:/Users/someone/x.js" << false;
    QTest::newRow("a local name with a backslash inside") << "file:///Users/some%5Cone/x.js" << false;
}

void FileRequestPathTests::networkPaths() {
    QFETCH(QString, url);
    QFETCH(bool, isNetwork);
    const QUrl parsed(url);
    QString filename = parsed.toLocalFile();
    if (filename.isEmpty()) {
        filename = parsed.toString();
    }
    QCOMPARE(FileResourceRequest::isNetworkPath(parsed, filename), isNetwork);
}
