//
//  ResourceSizeLimitTests.cpp
//
//  Created by servers on 2026-11-02.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ResourceSizeLimitTests.h"

#include <QUrl>

#include <Brotli.h>
#include <Gzip.h>
#include <ResourceSizeLimit.h>

QTEST_MAIN(ResourceSizeLimitTests)

void ResourceSizeLimitTests::kindByEnding_data() {
    QTest::addColumn<QString>("url");
    QTest::addColumn<int>("limit");
    QTest::newRow("model") << "https://files.example/caitlyn/WelcomeWagon11_clean.fbx" << ResourceSizeLimit::MODEL;
    QTest::newRow("model, upper case") << "https://files.example/a/Train.GLB" << ResourceSizeLimit::MODEL;
    QTest::newRow("model with a query") << "https://files.example/a/car.gltf?v=3" << ResourceSizeLimit::MODEL;
    QTest::newRow("texture") << "https://files.example/sky/skybox.jpg" << ResourceSizeLimit::TEXTURE;
    QTest::newRow("baked texture") << "https://files.example/baked/wall.ktx" << ResourceSizeLimit::TEXTURE;
    QTest::newRow("sound") << "https://files.example/sounds/bell.wav" << ResourceSizeLimit::SOUND;
    QTest::newRow("script") << "https://files.example/scripts/sitClient.js" << ResourceSizeLimit::SCRIPT;
    QTest::newRow("world file") << "https://files.example/worlds/models.json" << ResourceSizeLimit::OTHER;
    QTest::newRow("no ending") << "https://files.example/content/abc123" << ResourceSizeLimit::OTHER;
    QTest::newRow("dot in a folder, not the file") << "https://files.example/v1.2/readme" << ResourceSizeLimit::OTHER;
    QTest::newRow("ending in the query only") << "https://files.example/get?file=x.js" << ResourceSizeLimit::OTHER;
}

void ResourceSizeLimitTests::kindByEnding() {
    QFETCH(QString, url);
    QFETCH(int, limit);
    // the request asks with the address's path, as HTTPResourceRequest does
    QCOMPARE(ResourceSizeLimit::forPath(QUrl(url).path()), limit);
}

void ResourceSizeLimitTests::limitsLeaveRoomAndStayUnderTheUnpackCeiling() {
    const int MB = 1000 * 1000;
    // the largest real files in Darkride's worlds on 2026-11-02 (models 22 MB, textures 18, sounds 19, scripts 30 KB)
    // fit with room to spare
    QVERIFY(ResourceSizeLimit::MODEL > 4 * 22 * MB);
    QVERIFY(ResourceSizeLimit::TEXTURE > 3 * 18 * MB);
    QVERIFY(ResourceSizeLimit::SOUND > 4 * 19 * MB);
    QVERIFY(ResourceSizeLimit::SCRIPT > 100 * 30 * 1000);
    // and every limit is under what gunzip and brotliDecompress would allow by default
    for (int limit : { ResourceSizeLimit::MODEL, ResourceSizeLimit::TEXTURE, ResourceSizeLimit::SOUND,
                       ResourceSizeLimit::SCRIPT, ResourceSizeLimit::OTHER }) {
        QVERIFY(limit <= GZIP_DEFAULT_MAX_SIZE / 2);
        QVERIFY(limit <= BROTLI_DEFAULT_MAX_SIZE / 2);
    }
}
