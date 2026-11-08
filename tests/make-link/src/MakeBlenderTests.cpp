//
//  MakeBlenderTests.cpp
//  tests/make-link/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeBlenderTests.h"

#include <cerrno>
#include <cstdio>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtEndian>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtNetwork/QTcpServer>

#include <MakeBlender.h>
#include <MakeToolList.h>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif
#ifdef Q_OS_MAC
#include <libproc.h>
#endif

namespace {

#ifdef DARKRIDE_MAKE_WATCH_PATH
const QString WATCH_PROGRAM = DARKRIDE_MAKE_WATCH_PATH;
#else
const QString WATCH_PROGRAM;
#endif

// this test program, started again with this set, plays the app for testBlenderEndsWithTheApp: it starts a runaway
// make, says its Blender's and watcher's process ids, and waits to be killed
const char* PLAY_THE_APP = "MAKE_BLENDER_TESTS_PLAY_THE_APP";

int playTheApp(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    MakeBlender blender(qEnvironmentVariable(PLAY_THE_APP));
    QString why;
    blender.setBlender(MakeBlender::findBlender(why));
    blender.setWatcher(WATCH_PROGRAM);
    if (blender.blender().isEmpty()) {
        return 2;
    }
    // only the watcher can end this one: the app's own limits are far off
    blender.setTimeLimit(10 * 60 * 1000);
    blender.setCpuLimit(10 * 60);
    blender.make("while True:\n    pass\n", "object", {}, [](const MakeBlender::Made&) { QCoreApplication::exit(3); });
    // Blender's id is the watcher's to write: wait for it
    QElapsedTimer waited;
    waited.start();
    while (blender.processId() == 0 && waited.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    std::printf("BLENDER_PID %lld WATCHER_PID %lld\n", static_cast<long long>(blender.processId()),
                static_cast<long long>(blender.watcherId()));
    std::fflush(stdout);
    return app.exec();
}

// whether a process with this id is still there (one this user may not signal or open counts as there)
bool processExists(qint64 pid) {
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exitCode = 0;
    const bool running = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

bool processGone(qint64 pid, int waitMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < waitMs) {
        if (!processExists(pid)) {
            return true;
        }
        QTest::qWait(100);
    }
    return false;
}

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
    if (app.isEmpty() || WATCH_PROGRAM.isEmpty()) {
        return false;
    }
    blender.setBlender(app);
    blender.setWatcher(WATCH_PROGRAM);
    return true;
}

// the program a process runs now (its path), empty when there's none
QString programOf(qint64 pid) {
#ifdef Q_OS_MAC
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(static_cast<pid_t>(pid), path, sizeof(path)) > 0) {
        return QString::fromUtf8(path);
    }
#endif
    return QString();
}

qint64 parentOf(qint64 pid) {
#ifdef Q_OS_MAC
    proc_bsdinfo info;
    if (proc_pidinfo(static_cast<pid_t>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info)) {
        return info.pbi_ppid;
    }
#endif
    return 0;
}

// the watcher run straight, with a plain sandbox line (no Blender): what it said, its exit, how long it took
struct Watched {
    int exit { -1 };
    bool crashed { false };
    QByteArray said;
    qint64 ms { 0 };
};

const char* OPEN_BOX = "(version 1)(allow default)";

Watched watch(const QStringList& arguments, std::function<void(QProcess&)> during = nullptr, int waitMs = 20000) {
    Watched watched;
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    QElapsedTimer timer;
    timer.start();
    process.start(WATCH_PROGRAM, arguments);
    if (!process.waitForStarted(5000)) {
        return watched;
    }
    if (during) {
        during(process);
    }
    process.waitForFinished(waitMs);
    watched.ms = timer.elapsed();
    watched.said = process.readAll();
    watched.crashed = process.exitStatus() == QProcess::CrashExit;
    watched.exit = process.state() == QProcess::NotRunning ? process.exitCode() : -2;
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(2000);
    }
    return watched;
}

QStringList boxLine(const QString& pidFile, const QStringList& line, int cap = 60) {
    return QStringList { QString::number(cap), pidFile, "--", "/usr/bin/sandbox-exec", "-p", OPEN_BOX } + line;
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

    // available: only sealed. Without its watcher, or with a Blender that isn't there, it says no and why
    QTemporaryDir dir;
    MakeBlender blender(dir.path());
    blender.setWatcher(dir.filePath("no-watcher"));
    QVERIFY(!blender.available(why));
    QVERIFY(why.contains("watcher"));
    if (!WATCH_PROGRAM.isEmpty()) {
        blender.setWatcher(WATCH_PROGRAM);
        QVERIFY2(blender.available(why), qPrintable(why));
        blender.setBlender("/nonexistent/Blender.app");
        QVERIFY(!blender.available(why));
        QVERIFY(!why.isEmpty());
    }
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

// the Keychain from inside the box (security's kit case): Blender's script asks for Make's own item and must not reach
// the Keychain at all. Only whether it exists is asked, never its secret, so no permission dialog can come up; and an
// "item not found" would mean the Keychain answered, so that fails too. The same script without the box must get
// "item not found" (the control), so the difference is the box's.
void MakeBlenderTests::testKeychainUnreachableInTheBox() {
    QTemporaryDir dir;
    MakeBlender blender(dir.path());
    if (!haveBlender(blender)) {
        QSKIP("no Blender here");
    }
    const QString script = QString(CUBE) + R"(
import ctypes, subprocess
try:
    security = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/Security.framework/Security")
    find = security.SecKeychainFindGenericPassword
    find.restype = ctypes.c_int32
    find.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_char_p,
                     ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    service = b"Darkride Make"
    account = b"Anthropic API key"
    print("KEYCHAIN_STATUS", find(None, len(service), service, len(account), account, None, None, None))
except Exception as e:
    print("KEYCHAIN_STATUS refused", type(e).__name__)
try:
    subprocess.run(["/usr/bin/security", "find-generic-password", "-s", "Darkride Make"], check=True,
                   capture_output=True)
    print("SECURITY_CLI OPEN")
except Exception as e:
    print("SECURITY_CLI REFUSED", type(e).__name__)
)";
    const QRegularExpression STATUS("KEYCHAIN_STATUS (\\S+)");
    const int errSecItemNotFound = -25300;

    // the control (security): the same job script, the same Blender and the same environment built from nothing, but
    // without the box, must reach the Keychain ("item not found": no Make key is kept on a test Mac). Only then does a
    // different answer from inside show the box made the difference, not a wrong call in Blender's Python.
    QFile job(dir.filePath("control-job.py"));
    QVERIFY(job.open(QIODevice::WriteOnly) && job.write(script.toUtf8()) > 0);
    job.close();
    QProcessEnvironment environment;
    environment.insert("PATH", "/usr/bin:/bin");
    environment.insert("HOME", dir.path());
    environment.insert("TMPDIR", dir.path());
    environment.insert("LANG", "en_US.UTF-8");
    environment.insert("PYTHONDONTWRITEBYTECODE", "1");
    environment.insert("PYTHONNOUSERSITE", "1");
    QProcess control;
    control.setProcessEnvironment(environment);
    control.setWorkingDirectory(dir.path());
    control.setProcessChannelMode(QProcess::MergedChannels);
    control.setStandardInputFile(QProcess::nullDevice());
    control.start(blender.blender() + "/Contents/MacOS/Blender",
                  { "--background", "--factory-startup", "--disable-autoexec", "-noaudio", "--python-exit-code", "3",
                    "--python", job.fileName() });
    QVERIFY(control.waitForFinished(60000));
    const QString controlLog = QString::fromUtf8(control.readAll());
    const QString outside = STATUS.match(controlLog).captured(1);
    qDebug().noquote() << "the Keychain without the box: status" << outside;
    QVERIFY2(outside == QString::number(errSecItemNotFound), qPrintable(controlLog.right(2000)));

    const MakeBlender::Made made = makeAndWait(blender, script);
    QVERIFY2(made.ok(), qPrintable(made.code + ": " + made.message + "\n" + made.log));
    const QString status = STATUS.match(made.log).captured(1);
    qDebug().noquote() << "the Keychain from the box: status" << status;
    QVERIFY2(!status.isEmpty(), qPrintable(made.log));
    QVERIFY2(status != "0" && status != QString::number(errSecItemNotFound), qPrintable(made.log));
    QVERIFY2(made.log.contains("SECURITY_CLI REFUSED"), qPrintable(made.log));
    QVERIFY(!made.log.contains("SECURITY_CLI OPEN"));
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

// the kernel's CPU cap and the thread cap are on Blender inside the box (security should-fix 4)
void MakeBlenderTests::testCpuCapInTheBox() {
    QTemporaryDir dir;
    MakeBlender blender(dir.path());
    if (!haveBlender(blender)) {
        QSKIP("no Blender here");
    }
    const QString script = QString(CUBE) +
        "import resource\nsoft, hard = resource.getrlimit(resource.RLIMIT_CPU)\nprint('CPULIMIT', soft, hard)\n"
        "print('THREADS', bpy.context.scene.render.threads)\n";
    const MakeBlender::Made made = makeAndWait(blender, script);
    QVERIFY2(made.ok(), qPrintable(made.message + "\n" + made.log));
    QVERIFY2(made.log.contains(QString("CPULIMIT %1 %2").arg(MakeBlender::CPU_LIMIT_S).arg(MakeBlender::CPU_LIMIT_S + 5)),
             qPrintable(made.log));
    QVERIFY2(made.log.contains(QString("THREADS %1").arg(MakeBlender::THREADS)), qPrintable(made.log));
    QVERIFY(MakeBlender::CPU_LIMIT_S >= (MakeBlender::TIME_LIMIT_MS / 1000) * MakeBlender::THREADS);
}

// the app killed mid-make (kill -9): Blender ends by itself, through the watcher, and the watcher ends too
void MakeBlenderTests::testBlenderEndsWithTheApp() {
#ifdef Q_OS_WIN
    QSKIP("the watcher is macOS and Linux's: on Windows a job object ends Blender with the app");
#else
    QTemporaryDir dir;
    MakeBlender probe(dir.path());
    if (!haveBlender(probe)) {
        QSKIP("no Blender here");
    }
    QProcess app;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(PLAY_THE_APP, dir.path());
    app.setProcessEnvironment(environment);
    app.start(QCoreApplication::applicationFilePath(), QStringList());
    QVERIFY(app.waitForStarted(5000));
    QByteArray said;
    QElapsedTimer timer;
    timer.start();
    while (!said.contains('\n') && timer.elapsed() < 15000) {
        app.waitForReadyRead(200);
        said.append(app.readAllStandardOutput());
    }
    const QRegularExpression IDS("BLENDER_PID (\\d+) WATCHER_PID (\\d+)");
    const QRegularExpressionMatch ids = IDS.match(QString::fromLatin1(said));
    QVERIFY2(ids.hasMatch(), said.constData());
    const qint64 blenderPid = ids.captured(1).toLongLong();
    const qint64 watcherPid = ids.captured(2).toLongLong();
    QVERIFY(blenderPid > 0 && watcherPid > 0);
    QTest::qWait(1500);
    QVERIFY2(processExists(blenderPid), "Blender should be running its runaway script");
    QVERIFY2(processExists(watcherPid), "the watcher should be waiting");

    QCOMPARE(::kill(static_cast<pid_t>(app.processId()), SIGKILL), 0);
    QVERIFY(app.waitForFinished(5000));
    QVERIFY2(processGone(blenderPid, 5000), "Blender ran on after the app was killed");
    QVERIFY2(processGone(watcherPid, 5000), "the watcher ran on after the app was killed");
#endif
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

// the watcher by itself, with a plain box line and no Blender: what it passes on, what ends its child, what it refuses
void MakeBlenderTests::testWatcherPassesTheExitOn() {
#ifdef Q_OS_WIN
    QSKIP("the watcher is macOS and Linux's");
#else
    if (WATCH_PROGRAM.isEmpty()) {
        QSKIP("the watcher isn't built with these tests");
    }
    QTemporaryDir dir;
    // Blender's exit code comes back; its standard input is /dev/null; it has only 0, 1 and 2 of this side's files
    Watched watched = watch(boxLine(dir.filePath("a.pid"), { "/bin/sh", "-c",
        "read line; echo READ=$?; for fd in 3 4 5 6 7 8 9 10 11 12; do if [ -e /dev/fd/$fd ]; then echo OPEN=$fd; fi; "
        "done; exit 7" }), [](QProcess& process) { process.write("not for the child, and no line"); });
    QCOMPARE(watched.exit, 7);
    QVERIFY2(watched.said.contains("READ=1"), watched.said.constData());
    // the shell's own [ -e ] test may open one; nothing of ours may be there
    QVERIFY2(watched.said.count("OPEN=") <= 1, watched.said.constData());
    // the pid file: the child's id, made new
    QFile pid(dir.filePath("a.pid"));
    QVERIFY(pid.open(QIODevice::ReadOnly));
    QVERIFY(pid.readAll().trimmed().toLongLong() > 1);
    QCOMPARE(int(pid.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)), 0);

    watched = watch(boxLine(dir.filePath("b.pid"), { "/usr/bin/true" }));
    QCOMPARE(watched.exit, 0);
#endif
}

void MakeBlenderTests::testWatcherEndsItsChild() {
#ifdef Q_OS_WIN
    QSKIP("the watcher is macOS and Linux's");
#else
    if (WATCH_PROGRAM.isEmpty()) {
        QSKIP("the watcher isn't built with these tests");
    }
    QTemporaryDir dir;
    const QStringList sleeper { "/bin/sleep", "60" };
    // "stop" on standard input
    Watched watched = watch(boxLine(dir.filePath("stop.pid"), sleeper), [](QProcess& process) {
        QTest::qWait(500);
        process.write("stop\n");
    });
    QCOMPARE(watched.exit, 128 + SIGKILL);
    QVERIFY(watched.ms < 5000);
    // standard input ends (the app gone)
    watched = watch(boxLine(dir.filePath("eof.pid"), sleeper), [](QProcess& process) {
        QTest::qWait(500);
        process.closeWriteChannel();
    });
    QCOMPARE(watched.exit, 128 + SIGKILL);
    QVERIFY(watched.ms < 5000);
    // its wall-clock cap
    watched = watch(boxLine(dir.filePath("cap.pid"), sleeper, 2));
    QCOMPARE(watched.exit, 128 + SIGKILL);
    QVERIFY2(watched.ms >= 1900 && watched.ms < 6000, qPrintable(QString::number(watched.ms)));
    // asked to end: the child first
    qint64 child = 0;
    const QString termPid = dir.filePath("term.pid");
    watched = watch(boxLine(termPid, sleeper), [&](QProcess& process) {
        QTest::qWait(500);
        QFile file(termPid);
        if (file.open(QIODevice::ReadOnly)) {
            child = file.readAll().trimmed().toLongLong();
        }
        ::kill(static_cast<pid_t>(process.processId()), SIGTERM);
    });
    QCOMPARE(watched.exit, 128 + SIGKILL);
    QVERIFY(child > 1);
    QVERIFY(processGone(child, 2000));
    // a child that ignores SIGTERM and SIGXCPU still ends (SIGKILL; exec, so no grandchild is left behind)
    watched = watch(boxLine(dir.filePath("trap.pid"), { "/bin/sh", "-c", "trap '' TERM XCPU HUP INT; exec /bin/sleep 60" }),
                    [](QProcess& process) {
                        QTest::qWait(500);
                        process.write("stop\n");
                    });
    QCOMPARE(watched.exit, 128 + SIGKILL);
    QVERIFY(watched.ms < 5000);
#endif
}

void MakeBlenderTests::testWatcherRefuses() {
#ifdef Q_OS_WIN
    QSKIP("the watcher is macOS and Linux's");
#else
    if (WATCH_PROGRAM.isEmpty()) {
        QSKIP("the watcher isn't built with these tests");
    }
    QTemporaryDir dir;
    const QString marker = dir.filePath("ran");
    const QStringList touch { "/usr/bin/touch", marker };
    // only the box's program
    Watched watched = watch({ "60", dir.filePath("p1.pid"), "--", "/bin/sh", "-c", "touch '" + marker + "'" });
    QCOMPARE(watched.exit, 125);
    // a cap that isn't a number of seconds from 1 to 3600, a relative pid file, no "--"
    for (const QStringList& bad : { QStringList { "0", dir.filePath("p2.pid"), "--", "/usr/bin/sandbox-exec" },
                                    QStringList { "3601", dir.filePath("p3.pid"), "--", "/usr/bin/sandbox-exec" },
                                    QStringList { "5s", dir.filePath("p4.pid"), "--", "/usr/bin/sandbox-exec" },
                                    QStringList { "60", "p5.pid", "--", "/usr/bin/sandbox-exec" },
                                    QStringList { "60", dir.filePath("p6.pid"), "/usr/bin/sandbox-exec" } }) {
        watched = watch(bad + QStringList { "-p", OPEN_BOX } + touch);
        QCOMPARE(watched.exit, 125);
    }
    // a pid file that is there already, or a link: refused, and the child never starts
    QFile there(dir.filePath("there.pid"));
    QVERIFY(there.open(QIODevice::WriteOnly));
    there.close();
    watched = watch(boxLine(dir.filePath("there.pid"), touch));
    QCOMPARE(watched.exit, 125);
    QVERIFY(QFile::link(dir.filePath("elsewhere"), dir.filePath("link.pid")));
    watched = watch(boxLine(dir.filePath("link.pid"), touch));
    QCOMPARE(watched.exit, 125);
    QVERIFY(!QFile::exists(dir.filePath("elsewhere")));
    QTest::qWait(300);
    QVERIFY(!QFile::exists(marker));
#endif
}

// in a make, Blender is the watcher's child, its id written outside the folder the box may write
void MakeBlenderTests::testWatcherIsBlendersParent() {
#ifdef Q_OS_WIN
    QSKIP("the watcher is macOS and Linux's");
#else
    QTemporaryDir dir;
    MakeBlender blender(dir.path());
    if (!haveBlender(blender)) {
        QSKIP("no Blender here");
    }
    MakeBlender::Made made;
    bool done = false;
    blender.make("while True:\n    pass\n", "object", {}, [&](const MakeBlender::Made& result) {
        made = result;
        done = true;
    });
    QElapsedTimer timer;
    timer.start();
    qint64 pid = 0;
    while (pid == 0 && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        pid = blender.processId();
    }
    QVERIFY(pid > 1);
    QVERIFY(pid != blender.watcherId());
    // once it is Blender (sandbox-exec becomes it in the same process), still the watcher's child
    while (!programOf(pid).endsWith("/Contents/MacOS/Blender") && timer.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    QVERIFY2(programOf(pid).endsWith("/Contents/MacOS/Blender"), qPrintable(programOf(pid)));
    QCOMPARE(parentOf(pid), blender.watcherId());
    QCOMPARE(parentOf(blender.watcherId()), QCoreApplication::applicationPid());
    const QStringList jobs = QDir(dir.filePath("jobs")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(jobs.size(), 1);
    QVERIFY(QFile::exists(dir.filePath("jobs/" + jobs.first() + "/blender.pid")));
    QVERIFY(!QFile::exists(dir.filePath("jobs/" + jobs.first() + "/work/blender.pid")));
    blender.cancel();
    QVERIFY(done);
    QCOMPARE(made.code, MakeError::REFUSED);
    QVERIFY(processGone(pid, 3000));
#endif
}

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsSet(PLAY_THE_APP)) {
        return playTheApp(argc, argv);
    }
    QGuiApplication app(argc, argv);
    MakeBlenderTests tests;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tests, argc, argv);
}
