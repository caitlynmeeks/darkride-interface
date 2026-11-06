//
//  MakeBlender.cpp
//  libraries/make-link/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeBlender.h"

#include <QtCore/QBuffer>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLoggingCategory>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QtEndian>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtGui/QPainter>

#include "MakeToolList.h"

#ifdef Q_OS_MAC
#include <libproc.h>
#include <signal.h>
#include <sys/resource.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(makelink)

const qint64 MakeBlender::MAX_GLB_BYTES = 64 * 1024 * 1024;
// under two minutes in all with the views: a longer tool call moves to Claude Code's background
const int MakeBlender::TIME_LIMIT_MS = 100 * 1000;
const qint64 MakeBlender::MEMORY_LIMIT_BYTES = 4LL * 1024 * 1024 * 1024;
const int MakeBlender::MINIMUM_MAJOR = 5;
const int MakeBlender::MINIMUM_MINOR = 2;

namespace {

const int MEMORY_CHECK_MS = 250;
const int MAX_LOG_BYTES = 256 * 1024;
const int MAX_REPORT_BYTES = 1024 * 1024;
const int MAX_VIEW_BYTES = 16 * 1024 * 1024;
const int MAX_VIEW_SIDE = 2048;
const int VIEW_TILE = 512;
const QStringList VIEWS { "front", "right", "back", "top" };
const QStringList FIXED_SCRIPTS { "box.sb", "prelude.py", "finish.py", "views.py" };
// what the exporter may require of a reader: plain glTF material and texture extensions, nothing that fetches
const QStringList KNOWN_REQUIRED_EXTENSIONS { "KHR_materials_emissive_strength", "KHR_texture_transform",
                                              "KHR_materials_transmission", "KHR_materials_ior",
                                              "KHR_materials_specular", "KHR_materials_volume",
                                              "KHR_materials_clearcoat", "KHR_materials_sheen",
                                              "KHR_mesh_quantization" };
const quint32 GLB_MAGIC = 0x46546C67;  // "glTF"
const quint32 CHUNK_JSON = 0x4E4F534A;
const quint32 CHUNK_BIN = 0x004E4942;

QString cut(const QString& text, int most) {
    return text.size() <= most ? text : text.left(most) + "...";
}

QString realPath(const QString& path) {
    const QString real = QFileInfo(path).canonicalFilePath();
    return real.isEmpty() ? path : real;
}

bool copyResource(const QString& name, const QString& to) {
    QFile from(":/make-link/blender/" + name);
    QFile file(to);
    if (!from.open(QIODevice::ReadOnly) || !file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(from.readAll()) >= 0;
}

QJsonArray cutStrings(const QJsonValue& value, int items, int length) {
    QJsonArray out;
    for (const QJsonValue& item : value.toArray()) {
        if (out.size() >= items) {
            break;
        }
        if (item.isString()) {
            out.append(cut(item.toString(), length));
        }
    }
    return out;
}

QJsonArray numbers(const QJsonValue& value, int count) {
    QJsonArray out;
    for (const QJsonValue& item : value.toArray()) {
        if (out.size() >= count) {
            break;
        }
        out.append(item.toDouble());
    }
    return out;
}

// Blender's own lines, as Claude may read them: the job's folder written as ".", the render's progress left out
QString logForClaude(const QByteArray& log, const QString& job) {
    QStringList kept;
    const QString text = QString::fromUtf8(log).replace(job, ".");
    for (const QString& line : text.split('\n')) {
        if (line.contains(QRegularExpression("^\\d\\d:\\d\\d\\.\\d+\\s+render\\s")) || line.startsWith("Fra:") ||
            line.trimmed().isEmpty()) {
            continue;
        }
        kept.append(cut(line, 300));
    }
    if (kept.size() > 60) {
        kept = kept.mid(kept.size() - 60);
    }
    return kept.join('\n');
}

}  // namespace

MakeBlender::MakeBlender(const QString& folder, QObject* parent) : QObject(parent), _folder(folder) {
}

MakeBlender::~MakeBlender() {
    cancel();
    forgetModels();
}

QString MakeBlender::blenderVersion(const QString& app) {
    QFile plist(app + "/Contents/Info.plist");
    if (!plist.open(QIODevice::ReadOnly) || plist.size() > 1024 * 1024) {
        return QString();
    }
    static const QRegularExpression VERSION("<key>CFBundleShortVersionString</key>\\s*<string>([0-9][0-9.]*)");
    return VERSION.match(QString::fromUtf8(plist.readAll())).captured(1);
}

QString MakeBlender::findBlender(QString& why, const QString& app) {
#ifndef Q_OS_MAC
    Q_UNUSED(app);
    why = "Make's Blender runs only on macOS for now, where it can be kept in a sandbox";
    return QString();
#else
    QStringList candidates;
    if (!app.isEmpty()) {
        candidates.append(app);
    } else {
        candidates = QStringList { "/Applications/Blender.app", QDir::homePath() + "/Applications/Blender.app" };
    }
    for (const QString& candidate : candidates) {
        const QString program = candidate + "/Contents/MacOS/Blender";
        if (!QFileInfo(program).isExecutable()) {
            continue;
        }
        const QStringList parts = blenderVersion(candidate).split('.');
        const int major = parts.value(0).toInt();
        const int minor = parts.value(1).toInt();
        if (major < MINIMUM_MAJOR || (major == MINIMUM_MAJOR && minor < MINIMUM_MINOR)) {
            why = QString("Blender %1 is older than Make needs (%2.%3 or newer)")
                      .arg(blenderVersion(candidate).isEmpty() ? QString("there") : blenderVersion(candidate))
                      .arg(MINIMUM_MAJOR).arg(MINIMUM_MINOR);
            continue;
        }
        why.clear();
        return realPath(candidate);
    }
    if (why.isEmpty()) {
        why = "Blender isn't installed (blender.org, into Applications)";
    }
    return QString();
#endif
}

void MakeBlender::make(const QString& script, const QString& kind, const QVector<Picture>& pictures,
                       std::function<void(const Made&)> done) {
    Made made;
    if (_process) {
        made.code = MakeError::BUSY;
        made.message = "a make is running already: one at a time";
        done(made);
        return;
    }
    QString why;
    if (_blender.isEmpty()) {
        _blender = findBlender(why);
    }
    if (_blender.isEmpty()) {
        made.code = MakeError::NOT_AVAILABLE_HERE;
        made.message = why;
        done(made);
        return;
    }
    // a fresh scratch folder for this make; the fixed scripts beside it, which the sandbox lets Blender only read
    const QString id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    const QString job = QDir(_folder).absoluteFilePath("jobs/" + id);
    const QString fixed = job + "/fixed";
    const QString work = job + "/work";
    if (!QDir().mkpath(fixed) || !QDir().mkpath(work + "/out") || !QDir().mkpath(work + "/textures")) {
        made.code = MakeError::NOT_AVAILABLE_HERE;
        made.message = "can't make a scratch folder for Blender";
        done(made);
        return;
    }
    QFile::setPermissions(job, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    for (const QString& name : FIXED_SCRIPTS) {
        if (!copyResource(name, fixed + "/" + name)) {
            made.code = MakeError::NOT_AVAILABLE_HERE;
            made.message = "the app's Blender scripts are missing";
            QDir(job).removeRecursively();
            done(made);
            return;
        }
    }
    QFile jobFile(work + "/job.py");
    if (!jobFile.open(QIODevice::WriteOnly) || jobFile.write(script.toUtf8()) < 0) {
        made.code = MakeError::NOT_AVAILABLE_HERE;
        made.message = "can't write the script for Blender";
        QDir(job).removeRecursively();
        done(made);
        return;
    }
    jobFile.close();
    static const QRegularExpression TEXTURE_NAME("^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,59}\\.(png|jpg|jpeg)$",
                                                 QRegularExpression::CaseInsensitiveOption);
    for (const Picture& picture : pictures) {
        if (!TEXTURE_NAME.match(picture.as).hasMatch() || picture.as.contains("..")) {
            made.code = MakeError::BAD_INPUT;
            made.message = "a texture's name must be letters, digits, '-', '_' and '.', ending .png or .jpg";
            QDir(job).removeRecursively();
            done(made);
            return;
        }
        if (!QFile::copy(picture.path, work + "/textures/" + picture.as)) {
            made.code = MakeError::NOT_FOUND;
            made.message = "no picture " + picture.as;
            QDir(job).removeRecursively();
            done(made);
            return;
        }
    }

    const QString realFixed = realPath(fixed);
    const QString realWork = realPath(work);
    _job = realPath(job);
    _done = std::move(done);
    _log.clear();
    _stopReason.clear();
    _stopCode.clear();

    // an environment built from nothing: no key, nothing of the app's
    QProcessEnvironment environment;
    environment.insert("PATH", "/usr/bin:/bin");
    environment.insert("HOME", realWork);
    environment.insert("TMPDIR", realWork);
    environment.insert("LANG", "en_US.UTF-8");
    environment.insert("MAKE_OUT", realWork + "/out");
    environment.insert("MAKE_KIND", kind == "room" ? "room" : "object");
    environment.insert("PYTHONDONTWRITEBYTECODE", "1");
    environment.insert("PYTHONNOUSERSITE", "1");

    _process = new QProcess(this);
    _process->setProcessEnvironment(environment);
    _process->setWorkingDirectory(realWork);
    _process->setProcessChannelMode(QProcess::MergedChannels);
    _process->setStandardInputFile(QProcess::nullDevice());
    connect(_process, &QProcess::readyRead, this, [this] {
        if (_process) {
            _log.append(_process->readAll());
            if (_log.size() > MAX_LOG_BYTES) {
                _log = _log.right(MAX_LOG_BYTES);
            }
        }
    });
    connect(_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) { finished(exitCode, status == QProcess::CrashExit); });
    connect(_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finished(-1, true);
        }
    });

    _limit = new QTimer(this);
    _limit->setSingleShot(true);
    connect(_limit, &QTimer::timeout, this, [this] {
        _stopCode = MakeError::TIMEOUT;
        _stopReason = QString("Blender ran past its %1 s limit and was stopped: make it simpler or faster")
                          .arg(_timeLimitMs / 1000);
        kill();
    });
    _memory = new QTimer(this);
    connect(_memory, &QTimer::timeout, this, &MakeBlender::checkMemory);

    const QStringList arguments {
        "-f", realFixed + "/box.sb",
        "-D", "BLENDER_APP=" + _blender,
        "-D", "FIXED=" + realFixed,
        "-D", "WORK=" + realWork,
        _blender + "/Contents/MacOS/Blender",
        "--background", "--factory-startup", "--disable-autoexec", "-noaudio", "--python-exit-code", "3",
        "--python", realFixed + "/prelude.py",
        "--python", realWork + "/job.py",
        "--python", realFixed + "/finish.py",
        "--python", realFixed + "/views.py",
    };
    _started = QDateTime::currentMSecsSinceEpoch();
    _process->start("/usr/bin/sandbox-exec", arguments);
    _limit->start(_timeLimitMs);
    _memory->start(MEMORY_CHECK_MS);
    qCInfo(makelink) << "Blender started for a make";
}

void MakeBlender::checkMemory() {
#ifdef Q_OS_MAC
    if (!_process) {
        return;
    }
    rusage_info_v2 usage;
    const pid_t pid = static_cast<pid_t>(_process->processId());
    if (pid > 0 && proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&usage)) == 0 &&
        static_cast<qint64>(usage.ri_phys_footprint) > MEMORY_LIMIT_BYTES) {
        _stopCode = MakeError::BAD_INPUT;
        _stopReason = QString("Blender used more than %1 GB of memory and was stopped: make it lighter")
                          .arg(MEMORY_LIMIT_BYTES / (1024 * 1024 * 1024));
        kill();
    }
#endif
}

void MakeBlender::kill() {
    if (_process && _process->state() != QProcess::NotRunning) {
        // the sandbox lets Blender start no other program, so the one process is all there is
        _process->kill();
    }
}

void MakeBlender::cancel() {
    if (_process) {
        _stopCode = MakeError::REFUSED;
        _stopReason = "the make was stopped";
        kill();
        _process->waitForFinished(2000);
    }
}

void MakeBlender::finished(int exitCode, bool crashed) {
    if (!_process) {
        return;
    }
    _log.append(_process->readAll());
    _process->deleteLater();
    _process = nullptr;
    for (QTimer* timer : { _limit, _memory }) {
        if (timer) {
            timer->stop();
            timer->deleteLater();
        }
    }
    _limit = nullptr;
    _memory = nullptr;

    Made made;
    made.seconds = (QDateTime::currentMSecsSinceEpoch() - _started) / 1000.0;
    made.log = logForClaude(_log, _job);
    const QString out = _job + "/work/out";
    if (!_stopCode.isEmpty()) {
        made.code = _stopCode;
        made.message = _stopReason;
    } else if (crashed || exitCode != 0 || !QFileInfo::exists(out + "/model.glb")) {
        made.code = MakeError::BAD_INPUT;
        made.message = crashed ? QString("Blender stopped unexpectedly") : QString("the script failed in Blender");
    } else {
        QFile glb(out + "/model.glb");
        QByteArray bytes;
        QString why;
        if (glb.size() > MAX_GLB_BYTES || !glb.open(QIODevice::ReadOnly)) {
            made.code = MakeError::BAD_INPUT;
            made.message = QString("the model is bigger than %1 MB").arg(MAX_GLB_BYTES / (1024 * 1024));
        } else if (bytes = glb.readAll(), !checkGlb(bytes, why)) {
            made.code = MakeError::BAD_INPUT;
            made.message = "the model was refused: " + why;
        } else {
            QFile report(out + "/report.json");
            if (report.size() <= MAX_REPORT_BYTES && report.open(QIODevice::ReadOnly)) {
                made.report = cleanReport(QJsonDocument::fromJson(report.readAll()).object());
            }
            QStringList views;
            for (const QString& view : VIEWS) {
                views.append(out + "/view-" + view + ".png");
            }
            made.views = drawViews(views, made.viewsType);
            const QString models = QDir(_folder).absoluteFilePath("models");
            QDir().mkpath(models);
            made.modelId = "m" + QUuid::createUuid().toString(QUuid::Id128).left(11);
            made.glbPath = models + "/" + made.modelId + ".glb";
            QFile kept(made.glbPath);
            if (made.views.isEmpty() || !kept.open(QIODevice::WriteOnly) || kept.write(bytes) != bytes.size()) {
                kept.remove();
                Made failed;
                failed.code = MakeError::BAD_INPUT;
                failed.message = made.views.isEmpty() ? QString("the model's views are missing") : QString("can't keep the model");
                failed.seconds = made.seconds;
                failed.log = made.log;
                made = failed;
            } else {
                made.bytes = bytes.size();
                _models.append(made.modelId);
            }
        }
    }
    QDir(_job).removeRecursively();
    _job.clear();
    finish(made);
}

void MakeBlender::finish(Made made) {
    auto done = std::move(_done);
    _done = nullptr;
    qCInfo(makelink) << "Blender's make ended:" << (made.ok() ? QString("a model") : made.code) << "in" << made.seconds
                     << "s";
    if (done) {
        done(made);
    }
}

QString MakeBlender::modelPath(const QString& modelId) const {
    if (!_models.contains(modelId)) {
        return QString();
    }
    return QDir(_folder).absoluteFilePath("models/" + modelId + ".glb");
}

void MakeBlender::forgetModels() {
    for (const QString& model : _models) {
        QFile::remove(QDir(_folder).absoluteFilePath("models/" + model + ".glb"));
    }
    _models.clear();
}

bool MakeBlender::checkGlb(const QByteArray& bytes, QString& why) {
    if (bytes.size() > MAX_GLB_BYTES) {
        why = "too big";
        return false;
    }
    if (bytes.size() < 20) {
        why = "not a glTF binary";
        return false;
    }
    const auto word = [&bytes](int at) { return qFromLittleEndian<quint32>(bytes.constData() + at); };
    if (word(0) != GLB_MAGIC || word(4) != 2) {
        why = "not a glTF 2.0 binary";
        return false;
    }
    if (word(8) != static_cast<quint32>(bytes.size())) {
        why = "its length doesn't match its header";
        return false;
    }
    QJsonObject json;
    bool sawBin = false;
    for (qint64 at = 12; at < bytes.size();) {
        if (at + 8 > bytes.size()) {
            why = "a chunk is cut short";
            return false;
        }
        const quint32 length = word(static_cast<int>(at));
        const quint32 type = word(static_cast<int>(at + 4));
        if (length % 4 != 0 || at + 8 + static_cast<qint64>(length) > bytes.size()) {
            why = "a chunk's length is wrong";
            return false;
        }
        if (at == 12) {
            if (type != CHUNK_JSON) {
                why = "the first chunk isn't JSON";
                return false;
            }
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(bytes.mid(static_cast<int>(at + 8), static_cast<int>(length)), &error);
            if (!document.isObject()) {
                why = "its JSON doesn't parse";
                return false;
            }
            json = document.object();
        } else if (type == CHUNK_BIN && !sawBin) {
            sawBin = true;
        } else {
            why = "it has an unknown chunk";
            return false;
        }
        at += 8 + length;
    }
    if (json.value("asset").toObject().value("version").toString() != "2.0") {
        why = "it isn't glTF 2.0";
        return false;
    }
    // everything inside the file: no buffer or picture with an address of its own (one to fetch, or another file)
    for (const QString& list : { QString("buffers"), QString("images") }) {
        for (const QJsonValue& entry : json.value(list).toArray()) {
            if (entry.toObject().contains("uri")) {
                why = "a " + list.left(list.size() - 1) + " points outside the file";
                return false;
            }
        }
    }
    for (const QJsonValue& extension : json.value("extensionsRequired").toArray()) {
        if (!KNOWN_REQUIRED_EXTENSIONS.contains(extension.toString())) {
            why = "it requires the extension " + cut(extension.toString(), 60);
            return false;
        }
    }
    return true;
}

QJsonObject MakeBlender::cleanReport(const QJsonObject& report) {
    QJsonObject clean;
    // the world's axes (y up), from Blender's (z up)
    const QJsonObject size = report.value("size_m").toObject();
    clean["size_m"] = QJsonArray { size.value("width_x").toDouble(), size.value("height_z").toDouble(),
                                   size.value("depth_y").toDouble() };
    clean["triangles"] = report.value("triangles").toInt();
    QJsonArray parts;
    for (const QJsonValue& entry : report.value("parts").toArray()) {
        if (parts.size() >= 100) {
            break;
        }
        const QJsonObject part = entry.toObject();
        parts.append(QJsonObject { { "name", cut(part.value("name").toString(), 60) },
                                   { "size", numbers(part.value("size"), 3) },
                                   { "triangles", part.value("triangles").toInt() },
                                   { "materials", cutStrings(part.value("materials"), 8, 40) } });
    }
    clean["parts"] = parts;
    clean["warnings"] = cutStrings(report.value("warnings"), 30, 200);
    clean["dropped"] = cutStrings(report.value("dropped"), 10, 40);
    clean["lights"] = report.value("lights").isArray() ? QJsonValue(report.value("lights").toArray().size())
                                                       : QJsonValue(QJsonValue::Null);
    const QJsonObject lightmap = report.value("lightmap").toObject();
    if (lightmap.isEmpty()) {
        clean["lightmap"] = QJsonValue::Null;
    } else {
        QJsonObject kept;
        for (const QString& key : { QString("size"), QString("samples"), QString("bake_seconds") }) {
            kept[key] = lightmap.value(key).toDouble();
        }
        for (auto it = lightmap.begin(); it != lightmap.end(); ++it) {
            if (it.key().startsWith("light_on_surfaces") && it.value().isObject()) {
                QJsonObject light;
                const QJsonObject given = it.value().toObject();
                for (const QString& key : { QString("darkest_10pc"), QString("median"), QString("brightest_5pc") }) {
                    light[key] = given.value(key).toDouble();
                }
                kept[it.key().left(40)] = light;
            }
        }
        clean["lightmap"] = kept;
    }
    return clean;
}

QByteArray MakeBlender::drawViews(const QStringList& pngs, QString& mimeType) {
    if (pngs.size() != 4) {
        return QByteArray();
    }
    QImage sheet(VIEW_TILE * 2, VIEW_TILE * 2, QImage::Format_RGB32);
    sheet.fill(Qt::black);
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (int i = 0; i < 4; ++i) {
        QFile file(pngs[i]);
        if (!file.exists() || file.size() > MAX_VIEW_BYTES || !file.open(QIODevice::ReadOnly)) {
            return QByteArray();
        }
        QImageReader reader(&file, "png");
        const QSize size = reader.size();
        if (!size.isValid() || size.width() > MAX_VIEW_SIDE || size.height() > MAX_VIEW_SIDE) {
            return QByteArray();
        }
        const QImage view = reader.read();
        if (view.isNull()) {
            return QByteArray();
        }
        painter.drawImage(QRect((i % 2) * VIEW_TILE, (i / 2) * VIEW_TILE, VIEW_TILE, VIEW_TILE), view);
    }
    painter.end();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (QImageWriter::supportedImageFormats().contains("jpeg")) {
        QImageWriter writer(&buffer, "jpeg");
        writer.setQuality(85);
        if (writer.write(sheet)) {
            mimeType = "image/jpeg";
            return bytes;
        }
        bytes.clear();
        buffer.seek(0);
    }
    QImageWriter writer(&buffer, "png");
    if (!writer.write(sheet)) {
        return QByteArray();
    }
    mimeType = "image/png";
    return bytes;
}
