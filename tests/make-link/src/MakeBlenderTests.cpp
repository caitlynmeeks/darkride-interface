//
//  MakeBlenderTests.cpp
//  tests/make-link/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeBlenderTests.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtEndian>
#include <QtGui/QImage>
#include <QtNetwork/QTcpServer>

#include <MakeBlender.h>
#include <MakeToolList.h>

QTEST_MAIN(MakeBlenderTests)

namespace {

QByteArray glb(const QJsonObject& json, const QByteArray& bin = QByteArray(4, '\0'), quint32 magic = 0x46546C67,
               int lengthOff = 0) {
    QByteArray text = QJsonDocument(json).toJson(QJsonDocument::Compact);
    while (text.size() % 4) {
        text.append(' ');
    }
    QByteArray body;
    auto word = [](QByteArray& to, quint32 value) {
        char bytes[4];
        qToLittleEndian(value, bytes);
        to.append(bytes, 4);
    };
    word(body, text.size());
    word(body, 0x4E4F534A);
    body.append(text);
    if (!bin.isEmpty()) {
        word(body, bin.size());
        word(body, 0x004E4942);
        body.append(bin);
    }
    QByteArray file;
    word(file, magic);
    word(file, 2);
    word(file, 12 + body.size() + lengthOff);
    file.append(body);
    return file;
}

QJsonObject plainGltf() {
    return QJsonObject { { "asset", QJsonObject { { "version", "2.0" } } },
                         { "buffers", QJsonArray { QJsonObject { { "byteLength", 4 } } } } };
}

// one make, waited for
MakeBlender::Made makeAndWait(MakeBlender& blender, const QString& script, int waitMs = 60000,
                              const QVector<MakeBlender::Picture>& pictures = {}) {
    MakeBlender::Made result;
    bool done = false;
    blender.make(script, "object", pictures, [&](const MakeBlender::Made& made) {
        result = made;
        done = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < waitMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return result;
}

const char* CUBE = "import bpy\nbpy.ops.mesh.primitive_cube_add(size=1)\nprint('CUBE MADE')\n";

bool haveBlender(MakeBlender& blender) {
    QString why;
    const QString app = MakeBlender::findBlender(why);
    if (app.isEmpty()) {
        return false;
    }
    blender.setBlender(app);
    return true;
}

}  // namespace

void MakeBlenderTests::testGlbCheck() {
    QString why;
    QVERIFY2(MakeBlender::checkGlb(glb(plainGltf()), why), qPrintable(why));
    QVERIFY(!MakeBlender::checkGlb(glb(plainGltf(), QByteArray(4, '\0'), 0x12345678), why));
    QVERIFY(!MakeBlender::checkGlb(glb(plainGltf(), QByteArray(4, '\0'), 0x46546C67, 8), why));
    QVERIFY(why.contains("length"));
    QVERIFY(!MakeBlender::checkGlb(glb(plainGltf()).left(30), why));
    QVERIFY(!MakeBlender::checkGlb(QByteArray("not a model at all"), why));

    QJsonObject outside = plainGltf();
    outside["buffers"] = QJsonArray { QJsonObject { { "byteLength", 4 }, { "uri", "https://elsewhere.example/b.bin" } } };
    QVERIFY(!MakeBlender::checkGlb(glb(outside), why));
    QVERIFY(why.contains("outside"));
    QJsonObject picture = plainGltf();
    picture["images"] = QJsonArray { QJsonObject { { "uri", "file:///Users/someone/.ssh/id_rsa" } } };
    QVERIFY(!MakeBlender::checkGlb(glb(picture), why));
    QJsonObject inside = plainGltf();
    inside["images"] = QJsonArray { QJsonObject { { "bufferView", 0 }, { "mimeType", "image/png" } } };
    QVERIFY(MakeBlender::checkGlb(glb(inside), why));
    QJsonObject required = plainGltf();
    required["extensionsRequired"] = QJsonArray { "EXT_something_new" };
    QVERIFY(!MakeBlender::checkGlb(glb(required), why));
    required["extensionsRequired"] = QJsonArray { "KHR_materials_emissive_strength" };
    QVERIFY(MakeBlender::checkGlb(glb(required), why));
    QJsonObject old = plainGltf();
    old["asset"] = QJsonObject { { "version", "1.0" } };
    QVERIFY(!MakeBlender::checkGlb(glb(old), why));
}

void MakeBlenderTests::testCleanReport() {
    QJsonArray warnings;
    for (int i = 0; i < 100; ++i) {
        warnings.append("warning " + QString::number(i));
    }
    const QJsonObject report {
        { "size_m", QJsonObject { { "width_x", 2.0 }, { "depth_y", 0.8 }, { "height_z", 2.4 } } },
        { "triangles", 604 },
        { "parts", QJsonArray { QJsonObject { { "name", QString(500, 'n') }, { "size", QJsonArray { 1, 2, 3, 4 } },
                                              { "triangles", 12 }, { "materials", QJsonArray { "oak" } },
                                              { "centre", QJsonArray { 0, 0, 0 } } } } },
        { "warnings", warnings },
        { "secret", "anything a script put here" },
    };
    const QJsonObject clean = MakeBlender::cleanReport(report);
    QCOMPARE(clean.value("size_m").toArray(), (QJsonArray { 2.0, 2.4, 0.8 }));  // x, y up, z
    QCOMPARE(clean.value("triangles").toInt(), 604);
    QCOMPARE(clean.value("warnings").toArray().size(), 30);
    QVERIFY(!clean.contains("secret"));
    const QJsonObject part = clean.value("parts").toArray().first().toObject();
    QVERIFY(part.value("name").toString().size() < 70);
    QCOMPARE(part.value("size").toArray().size(), 3);
    QVERIFY(!part.contains("centre"));
    QVERIFY(clean.value("lightmap").isNull());
}

void MakeBlenderTests::testDrawViews() {
    QTemporaryDir dir;
    QStringList views;
    const QList<QColor> colours { Qt::red, Qt::green, Qt::blue, Qt::yellow };
    for (int i = 0; i < 4; ++i) {
        QImage view(512, 512, QImage::Format_RGB32);
        view.fill(colours[i]);
        views.append(dir.filePath(QString("view-%1.png").arg(i)));
        QVERIFY(view.save(views.last(), "png"));
    }
    QString type;
    const QByteArray sheet = MakeBlender::drawViews(views, type);
    QVERIFY(!sheet.isEmpty());
    QVERIFY(type == "image/jpeg" || type == "image/png");
    const QImage drawn = QImage::fromData(sheet);
    QCOMPARE(drawn.size(), QSize(1024, 1024));
    QVERIFY(qRed(drawn.pixel(100, 100)) > 200 && qGreen(drawn.pixel(100, 100)) < 60);  // front at the top left
    QVERIFY(qBlue(drawn.pixel(100, 900)) > 200 && qRed(drawn.pixel(100, 900)) < 60);   // back at the bottom left

    QStringList missing = views;
    missing[2] = dir.filePath("nothing.png");
    QVERIFY(MakeBlender::drawViews(missing, type).isEmpty());
    QStringList huge = views;
    QImage big(4096, 16, QImage::Format_RGB32);
    big.fill(Qt::white);
    huge[1] = dir.filePath("big.png");
    QVERIFY(big.save(huge[1], "png"));
    QVERIFY(MakeBlender::drawViews(huge, type).isEmpty());  // past the size cap, refused before decoding
}

void MakeBlenderTests::testFindBlender() {
    QString why;
    const QString app = MakeBlender::findBlender(why);
    if (app.isEmpty()) {
        QVERIFY(!why.isEmpty());
        QSKIP("no Blender here");
    }
    const QStringList version = MakeBlender::blenderVersion(app).split('.');
    QVERIFY(version.value(0).toInt() > 5 || (version.value(0).toInt() == 5 && version.value(1).toInt() >= 2));
    QVERIFY(MakeBlender::findBlender(why, "/nonexistent/Blender.app").isEmpty());
}

void MakeBlenderTests::testMakeAModel() {
    QTemporaryDir dir;
    MakeBlender blender(dir.path());
    if (!haveBlender(blender)) {
        QSKIP("no Blender here");
    }
    const MakeBlender::Made made = makeAndWait(blender, CUBE);
    QVERIFY2(made.ok(), qPrintable(made.code + ": " + made.message + "\n" + made.log));
    QVERIFY(made.modelId.startsWith("m"));
    QCOMPARE(blender.modelPath(made.modelId), made.glbPath);
    QFile model(made.glbPath);
    QVERIFY(model.open(QIODevice::ReadOnly));
    QString why;
    QVERIFY(MakeBlender::checkGlb(model.readAll(), why));
    QCOMPARE(made.bytes, model.size());
    QCOMPARE(made.report.value("triangles").toInt(), 12);
    QCOMPARE(made.report.value("size_m").toArray(), (QJsonArray { 1.0, 1.0, 1.0 }));
    QCOMPARE(QImage::fromData(made.views).size(), QSize(1024, 1024));
    QVERIFY(made.log.contains("CUBE MADE"));
    QVERIFY(made.seconds < 60);
    QVERIFY(QDir(dir.filePath("jobs")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());  // the scratch folder went
    blender.forgetModels();
    QVERIFY(!QFile::exists(made.glbPath));
    QVERIFY(blender.modelPath(made.modelId).isEmpty());
}

void MakeBlenderTests::testScriptErrorComesBack() {
    QTemporaryDir dir;
    MakeBlender blender(dir.path());
    if (!haveBlender(blender)) {
        QSKIP("no Blender here");
    }
    const MakeBlender::Made made = makeAndWait(blender, "raise RuntimeError('the awning has no posts')\n");
    QCOMPARE(made.code, MakeError::BAD_INPUT);
    QVERIFY(made.log.contains("the awning has no posts"));
    QVERIFY(!made.log.contains(dir.path()));  // the scratch folder's path is written as "."
    const MakeBlender::Made nothing = makeAndWait(blender, "print('nothing built')\n");
    QCOMPARE(nothing.code, MakeError::BAD_INPUT);
    QVERIFY(nothing.log.contains("no visible mesh"));
}

void MakeBlenderTests::testTheBoxHolds() {
    QTemporaryDir dir;
    MakeBlender blender(dir.path());
    if (!haveBlender(blender)) {
        QSKIP("no Blender here");
    }
    // a key in the app's own environment, and a port listening on this computer
    qputenv("ANTHROPIC_API_KEY", "sk-ant-api03-test-key-that-must-not-reach-blender");
    QTcpServer local;
    QVERIFY(local.listen(QHostAddress::LocalHost));
    const QString outside = QDir::tempPath() + "/make-box-escape-" + QString::number(QCoreApplication::applicationPid());
    const QString script = QString(R"(
import bpy, os, socket, subprocess
HOME = %1
def tryit(name, fn):
    try:
        fn()
        print(name, "OPEN")
    except Exception as e:
        print(name, "REFUSED", type(e).__name__)
tryit("SSH", lambda: os.listdir(os.path.join(HOME, ".ssh")))
tryit("DOCUMENTS", lambda: os.listdir(os.path.join(HOME, "Documents")))
tryit("LIBRARY", lambda: os.listdir(os.path.join(HOME, "Library")))
tryit("NET", lambda: socket.create_connection(("1.1.1.1", 80), timeout=3))
tryit("LOCAL", lambda: socket.create_connection(("127.0.0.1", %2), timeout=3))
tryit("DNS", lambda: socket.getaddrinfo("example.com", 80))
tryit("WRITE", lambda: open(%3, "w").write("x"))
tryit("SUBPROCESS", lambda: subprocess.run(["/bin/echo", "hi"]))
tryit("FORK", lambda: os.fork())
print("ENVIRONMENT", " ".join(sorted(os.environ.keys())))
print("KEYVALUES", "sk-ant" in " ".join(os.environ.values()))
bpy.ops.mesh.primitive_cube_add(size=1)
)").arg("'" + QDir::homePath() + "'").arg(local.serverPort()).arg("'" + outside + "'");
    const MakeBlender::Made made = makeAndWait(blender, script);
    qunsetenv("ANTHROPIC_API_KEY");
    QVERIFY2(made.ok(), qPrintable(made.code + ": " + made.message + "\n" + made.log));
    qDebug().noquote() << "the box, as the script saw it:\n" + made.log;
    for (const QString& what : { "SSH", "DOCUMENTS", "LIBRARY", "NET", "LOCAL", "DNS", "WRITE", "SUBPROCESS", "FORK" }) {
        QVERIFY2(made.log.contains(what + " REFUSED"), qPrintable(what));
        QVERIFY2(!made.log.contains(what + " OPEN"), qPrintable(what));
    }
    QVERIFY(!made.log.contains("ANTHROPIC"));
    QVERIFY(made.log.contains("KEYVALUES False"));
    QVERIFY(!local.hasPendingConnections());
    QVERIFY(!QFile::exists(outside));
}

void MakeBlenderTests::testRunawayIsStopped() {
    QTemporaryDir dir;
    MakeBlender blender(dir.path());
    if (!haveBlender(blender)) {
        QSKIP("no Blender here");
    }
    blender.setTimeLimit(3000);
    QElapsedTimer timer;
    timer.start();
    const MakeBlender::Made made = makeAndWait(blender, "while True:\n    pass\n", 20000);
    QCOMPARE(made.code, MakeError::TIMEOUT);
    QVERIFY(timer.elapsed() >= 2900 && timer.elapsed() < 10000);
    QVERIFY(!blender.isBusy());
    QVERIFY(QDir(dir.filePath("jobs")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

void MakeBlenderTests::testOneMakeAtATime() {
    QTemporaryDir dir;
    MakeBlender blender(dir.path());
    if (!haveBlender(blender)) {
        QSKIP("no Blender here");
    }
    MakeBlender::Made first;
    bool firstDone = false;
    blender.make("while True:\n    pass\n", "object", {}, [&](const MakeBlender::Made& made) {
        first = made;
        firstDone = true;
    });
    QVERIFY(blender.isBusy());
    const MakeBlender::Made second = makeAndWait(blender, CUBE, 1000);
    QCOMPARE(second.code, MakeError::BUSY);
    blender.cancel();  // Stop
    QVERIFY(firstDone);
    QCOMPARE(first.code, MakeError::REFUSED);
    QVERIFY(!blender.isBusy());
}

void MakeBlenderTests::testTextureNamesChecked() {
    QTemporaryDir dir;
    MakeBlender blender(dir.path());
    if (!haveBlender(blender)) {
        QSKIP("no Blender here");
    }
    QImage picture(8, 8, QImage::Format_RGB32);
    picture.fill(Qt::red);
    const QString clean = dir.filePath("clean.png");
    QVERIFY(picture.save(clean, "png"));
    for (const QString& name : { "../escape.png", "a/b.png", "script.py", ".hidden.png" }) {
        const MakeBlender::Made made = makeAndWait(blender, CUBE, 5000, { { clean, name } });
        QCOMPARE(made.code, MakeError::BAD_INPUT);
    }
    const QString script = QString(CUBE) +
        "import os\nprint('TEXTURE', os.path.getsize(os.path.join(os.environ['HOME'], 'textures', 'awning.png')) > 0)\n";
    const MakeBlender::Made made = makeAndWait(blender, script, 60000, { { clean, "awning.png" } });
    QVERIFY2(made.ok(), qPrintable(made.message + "\n" + made.log));
    QVERIFY(made.log.contains("TEXTURE True"));
}
