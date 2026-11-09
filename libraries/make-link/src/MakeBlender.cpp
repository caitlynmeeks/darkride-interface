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
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLoggingCategory>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QtEndian>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtGui/QPainter>

#include "MakeFirewall.h"
#include "MakeToolList.h"

#ifdef Q_OS_MAC
#include <libproc.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <thread>

#include "MakeBox.h"
#endif
#ifdef Q_OS_LINUX
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(makelink)

const qint64 MakeBlender::MAX_GLB_BYTES = 64 * 1024 * 1024;
// under two minutes in all with the views: a longer tool call moves to Claude Code's background
const int MakeBlender::TIME_LIMIT_MS = 100 * 1000;
const qint64 MakeBlender::MEMORY_LIMIT_BYTES = 4LL * 1024 * 1024 * 1024;
// Linux: the work folder's size cap (the watcher's --folder-cap, and each file's RLIMIT_FSIZE in the box): far above a
// capped model and its views, far below a full disk
const qint64 MakeBlender::FOLDER_CAP_BYTES = 2LL * 1024 * 1024 * 1024;
const int MakeBlender::THREADS = 4;
const int MakeBlender::CPU_LIMIT_S = (MakeBlender::TIME_LIMIT_MS / 1000) * MakeBlender::THREADS + 20;
const int MakeBlender::MINIMUM_MAJOR = 5;
const int MakeBlender::MINIMUM_MINOR = 2;
// the dock's words (pm and Caity's wording; docs quote the second): one ellipsis character, no word of the machinery
const char* const MakeBlender::CHECKING_WORDS = "Checking Blender…";
const char* const MakeBlender::UNAVAILABLE_WORDS = "Blender isn't available here";

namespace {

#ifdef Q_OS_WIN
// the self-test's target: Windows' own RPC endpoint mapper on this computer's own address, listening on every Windows
// (a bare connection, closed at once, nothing sent or read: no listener of ours, so no firewall alert), and how long
// the control and the boxed probe may take
const int SELF_TEST_PORT = 135;
const int SELF_TEST_CONTROL_MS = 3000;
const int SELF_TEST_PROBE_MS = 45000;
#endif
#ifdef Q_OS_LINUX
// Landlock's version, as the watcher asks it before it boxes Blender (landlock_create_ruleset with the version flag,
// written out so this builds with older kernel headers): 0 when the kernel has none
int landlockAbi() {
    const long version = syscall(444, nullptr, 0, 1U);
    return version < 0 ? 0 : static_cast<int>(version);
}
#endif

const int MEMORY_CHECK_MS = 250;
// how long Stop waits for the watcher to end Blender and itself
const int STOP_WAIT_MS = 3000;
// the watcher's own exit when it can't start Blender
const int WATCHER_CANT_START = 125;
const char* WATCHER = "darkride-make-watch";
// between the CPU cap's warning (SIGXCPU, which Blender's script could ignore) and the kernel's kill (SIGKILL)
const int CPU_LIMIT_GRACE_S = 5;
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
    initMakeLinkResources();
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


// Blender's parent (the watcher), its CPU time capped by the kernel before it starts (security should-fix 4): the cap is
// set in the child between fork and exec and Blender inherits it from the watcher. Only async-signal-safe calls here.
// The script inside can lower the cap, never raise it (the hard limit needs root).
void capCpuInChild(int seconds) {
#if defined(Q_OS_MAC) || defined(Q_OS_LINUX)
    struct rlimit now;
    if (getrlimit(RLIMIT_CPU, &now) != 0) {
        _exit(126);
    }
    rlim_t hard = static_cast<rlim_t>(seconds + CPU_LIMIT_GRACE_S);
    if (now.rlim_max != RLIM_INFINITY && now.rlim_max < hard) {
        hard = now.rlim_max;
    }
    rlim_t soft = static_cast<rlim_t>(seconds);
    if (soft > hard) {
        soft = hard;
    }
    const struct rlimit capped { soft, hard };
    if (setrlimit(RLIMIT_CPU, &capped) != 0) {
        _exit(126);  // never start Blender without its cap
    }
#else
    Q_UNUSED(seconds);
#endif
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_UNIX)
class CappedProcess : public QProcess {
public:
    CappedProcess(int cpuSeconds, QObject* parent) : QProcess(parent) {
        setChildProcessModifier([cpuSeconds] { capCpuInChild(cpuSeconds); });
    }
};
#elif QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
// Windows: Qt 6's QProcess has no child modifier there (no fork), and capCpuInChild does nothing off the Mac; the CPU
// cap on Windows is the job object's (MakeBox), so this is a plain QProcess
class CappedProcess : public QProcess {
public:
    CappedProcess(int cpuSeconds, QObject* parent) : QProcess(parent) { Q_UNUSED(cpuSeconds); }
};
#else
class CappedProcess : public QProcess {
public:
    CappedProcess(int cpuSeconds, QObject* parent) : QProcess(parent), _cpuSeconds(cpuSeconds) {}

protected:
    void setupChildProcess() override { capCpuInChild(_cpuSeconds); }

private:
    const int _cpuSeconds;
};
#endif

}  // namespace

MakeBlender::MakeBlender(const QString& folder, QObject* parent) : QObject(parent), _folder(folder) {
}

MakeBlender::~MakeBlender() {
    cancel();
    forgetModels();
}

QString MakeBlender::blenderVersion(const QString& app) {
#ifdef Q_OS_WIN
    // from blender.exe's own version resource: no process started; a folder means the blender.exe in it
    const QString program = QFileInfo(app).isDir() ? QDir(app).absoluteFilePath("blender.exe") : app;
    return MakeBox::fileVersion(program);
#elif defined(Q_OS_LINUX)
    // the unpacked tarball's folder says the version in its name (its program's folder when given the program)
    return versionFromTarball(QFileInfo(app).isDir() ? app : QFileInfo(app).absolutePath());
#else
    QFile plist(app + "/Contents/Info.plist");
    if (!plist.open(QIODevice::ReadOnly) || plist.size() > 1024 * 1024) {
        return QString();
    }
    static const QRegularExpression VERSION("<key>CFBundleShortVersionString</key>\\s*<string>([0-9][0-9.]*)");
    return VERSION.match(QString::fromUtf8(plist.readAll())).captured(1);
#endif
}

QString MakeBlender::defaultWatcher() {
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(WATCHER);
}

bool MakeBlender::versionIsEnough(const QString& version) {
    const QStringList parts = version.split('.');
    bool majorRead = false;
    const int major = parts.value(0).toInt(&majorRead);
    const int minor = parts.value(1).toInt();
    return majorRead && (major > MINIMUM_MAJOR || (major == MINIMUM_MAJOR && minor >= MINIMUM_MINOR));
}

QString MakeBlender::tooOldLine(const QString& version) {
    if (version.isEmpty()) {
        return QString("That Blender's version can't be read; Make needs %1.%2 or newer.").arg(MINIMUM_MAJOR).arg(MINIMUM_MINOR);
    }
    return QString("That's Blender %1; Make needs %2.%3 or newer.").arg(version).arg(MINIMUM_MAJOR).arg(MINIMUM_MINOR);
}

bool MakeBlender::readSelfTestResult(const QByteArray& text, bool& held) {
    const QString word = QString::fromLatin1(text.left(64)).trimmed();
    if (word == "refused") {
        held = true;
        return true;
    }
    if (word == "reached") {
        held = false;
        return true;
    }
    return false;
}

bool MakeBlender::looksLikeBlender(const QString& fileName, const QString& productName, QString& why) {
    if (QString::compare(fileName, "blender.exe", Qt::CaseInsensitive) != 0) {
        why = "That program isn't blender.exe.";
        return false;
    }
    if (productName.trimmed() != "Blender") {
        why = "That program doesn't say it is Blender.";
        return false;
    }
    return true;
}

bool MakeBlender::isBlendersOwnFolder(const QString& folder, const QString& version, const QStringList& forbidden,
                                      QString& why) {
    static const QString NOT_OWN = "That folder isn't Blender's own.";
    const QString own = QDir::cleanPath(QDir::fromNativeSeparators(folder));
    if (own.isEmpty() || QDir(own).isRoot()) {
        why = NOT_OWN;
        return false;
    }
    for (const QString& known : forbidden) {
        const QString other = QDir::cleanPath(QDir::fromNativeSeparators(known));
        if (other.isEmpty()) {
            continue;
        }
        // the folder itself, or a parent of it: a grant there would open the known folder to the box
        if (QString::compare(own, other, Qt::CaseInsensitive) == 0 || other.startsWith(own + "/", Qt::CaseInsensitive)) {
            why = NOT_OWN;
            return false;
        }
    }
    const QStringList parts = version.split('.');
    if (parts.size() < 2 || !QFileInfo(own + "/" + parts.at(0) + "." + parts.at(1)).isDir()) {
        why = NOT_OWN;
        return false;
    }
    return true;
}

QStringList MakeBlender::linuxForbiddenFolders() {
    // the system's own folders (a distro package's Blender lives in /usr/bin, and has no "5.2" folder beside it
    // anyway) and the person's own, canonical; the usual places (/opt, the home folder, Downloads) are in it
    // themselves, so only a Blender folder inside one passes, never the place itself or a parent of it
    QStringList folders { "/", "/usr", "/usr/bin", "/usr/local/bin", "/bin", "/opt" };
    for (QStandardPaths::StandardLocation location : { QStandardPaths::HomeLocation, QStandardPaths::DownloadLocation,
                                                       QStandardPaths::DesktopLocation,
                                                       QStandardPaths::DocumentsLocation }) {
        const QString path = QStandardPaths::writableLocation(location);
        if (!path.isEmpty()) {
            const QString canonical = QFileInfo(path).canonicalFilePath();
            folders.append(canonical.isEmpty() ? path : canonical);
        }
    }
    folders.append(QDir::homePath());
    folders.removeDuplicates();
    return folders;
}

QStringList MakeBlender::linuxBoxFilterRules() {
    // the same lines as the watcher's FILTER_RULES (tools/darkride-make-watch/src/main.cpp): change both together; the
    // Linux test compares them against the watcher's --filter-rules output
    return QStringList {
        "execve, execveat: the exec gate (the child's own start of Blender passes, every later one EPERM)",
        "truncate: EPERM (shortening a file by path; Landlock governs it only from ABI 3)",
        "open, openat with O_TRUNC in any mode but write (read-only, or access mode 3): EPERM (shortening without a write open; "
        "Landlock from ABI 3)",
        "openat2: ENOSYS (its flags are behind a pointer; the C library falls back to openat)",
        "fork, vfork: EPERM; clone3: ENOSYS; clone with a namespace flag: EPERM (threads only)",
        "socket, socketpair: EACCES (no sockets of any family)",
        "kill, tkill, tgkill, rt_sigqueueinfo, rt_tgsigqueueinfo to another process: EPERM; pidfd_send_signal: EPERM",
        "ioctl TIOCSTI, TIOCLINUX: EPERM (no typing into a terminal)",
        "ptrace, process_vm_readv, process_vm_writev, kcmp, pidfd_getfd, process_madvise, io_uring, bpf, perf_event_open, "
        "userfaultfd, unshare, setns, mount, umount2, pivot_root, chroot, open_tree, move_mount, fsopen, fsconfig, fsmount, "
        "fspick, mount_setattr, add_key, request_key, keyctl, kexec_load, kexec_file_load, init_module, finit_module, "
        "delete_module, acct, swapon, swapoff, reboot, quotactl, quotactl_fd, open_by_handle_at, name_to_handle_at, iopl, "
        "ioperm, uselib, vhangup, syslog, fanotify_init: EPERM",
        "x32 system calls: EPERM; another architecture: the process is killed",
    };
}

QString MakeBlender::versionFromTarball(const QString& folder) {
    // the official tarball unpacks as "blender-5.2.3-linux-x64": the version is in the name; a renamed folder still
    // holds Blender's own "5.2" folder beside the program (the newest such folder when there are several)
    static const QRegularExpression NAME("^blender-?([0-9]+\\.[0-9]+(?:\\.[0-9]+)?)",
                                         QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch named = NAME.match(QFileInfo(folder).fileName());
    if (named.hasMatch()) {
        return named.captured(1);
    }
    static const QRegularExpression INSIDE("^[0-9]+\\.[0-9]+$");
    const auto newer = [](const QString& a, const QString& b) {
        const QStringList pa = a.split('.'), pb = b.split('.');
        return pa.value(0).toInt() > pb.value(0).toInt() ||
               (pa.value(0).toInt() == pb.value(0).toInt() && pa.value(1).toInt() > pb.value(1).toInt());
    };
    QString best;
    for (const QString& name : QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (INSIDE.match(name).hasMatch() && (best.isEmpty() || newer(name, best))) {
            best = name;
        }
    }
    return best;
}

qint64 MakeBlender::vmRssBytes(const QByteArray& status) {
    // /proc/<pid>/status: "VmRSS:\t  123456 kB" (the kernel writes kB)
    for (const QByteArray& line : status.split('\n')) {
        if (!line.startsWith("VmRSS:")) {
            continue;
        }
        const QList<QByteArray> words = line.mid(6).simplified().split(' ');
        bool number = false;
        const qint64 value = words.value(0).toLongLong(&number);
        if (!number || value < 0) {
            return -1;
        }
        return words.value(1).toLower() == "kb" ? value * 1024 : value;
    }
    return -1;
}

bool MakeBlender::available(QString& why) const {
#if defined(Q_OS_WIN)
    // Windows: a Blender found, and this Connect's self-test (selfTest) passed: the box's own connection out was
    // refused, so the installer's firewall rules hold for it. The rules' read is a reported extra, never the decision.
    if (findBlender(why, _blender).isEmpty()) {
        return false;
    }
    switch (_selfTest) {
        case SelfTest::Running:
            why = CHECKING_WORDS;
            return false;
        case SelfTest::Failed:
            why = UNAVAILABLE_WORDS;
            return false;
        case SelfTest::Passed:
        case SelfTest::NotRun:
            // before the first Connect, what it would decide isn't known: a found Blender counts, the Connect decides,
            // and a make runs only after a passed self-test (make())
            why.clear();
            return true;
    }
    return false;
#elif defined(Q_OS_LINUX)
    // Linux: Landlock in the kernel (the box's file rules; ABI 1 is enough, newer ones add the TCP and signal scoping),
    // the watcher beside the app (on Linux it is the box: its child takes the limits, Landlock and seccomp with the
    // exec gate before Blender starts), and a Blender found. Without Landlock there is no box, and Blender never runs
    // unsealed
    if (landlockAbi() < 1) {
        qCInfo(makelink) << "Blender isn't offered: this kernel has no Landlock";
        why = UNAVAILABLE_WORDS;
        return false;
    }
    if (!QFileInfo(_watch.isEmpty() ? defaultWatcher() : _watch).isExecutable()) {
        why = "Make's Blender watcher is missing from the app";
        return false;
    }
    return !findBlender(why, _blender).isEmpty();
#elif !defined(Q_OS_MAC)
    why = UNAVAILABLE_WORDS;
    return false;
#else
    if (!QFileInfo(QStringLiteral("/usr/bin/sandbox-exec")).isExecutable()) {
        why = "Make can't close Blender off from the internet and your files on this Mac";
        return false;
    }
    if (!QFileInfo(_watch.isEmpty() ? defaultWatcher() : _watch).isExecutable()) {
        why = "Make's Blender watcher is missing from the app";
        return false;
    }
    return !findBlender(why, _blender).isEmpty();
#endif
}

QString MakeBlender::findBlender(QString& why, const QString& app) {
#if defined(Q_OS_WIN)
    // the person's choice (blender.exe, or its folder), else the usual places: Blender's installer under Program
    // Files, an unpacked zip in Downloads or on the Desktop; the newest that is new enough wins. Cheap: file names and
    // version resources only, no process started (the dock asks whenever its window shows)
    QStringList candidates;
    if (!app.isEmpty()) {
        candidates.append(QFileInfo(app).isDir() ? QDir(app).absoluteFilePath("blender.exe") : app);
    } else {
        for (const QString& root : { qEnvironmentVariable("ProgramW6432"), qEnvironmentVariable("ProgramFiles") }) {
            if (root.isEmpty()) {
                continue;
            }
            const QDir foundation(QDir::fromNativeSeparators(root) + "/Blender Foundation");
            for (const QString& name : foundation.entryList({ "Blender *" }, QDir::Dirs | QDir::NoDotAndDotDot)) {
                candidates.append(foundation.absoluteFilePath(name + "/blender.exe"));
            }
        }
        for (const QString& place : { QDir::homePath() + "/Downloads", QDir::homePath() + "/Desktop" }) {
            const QDir folder(place);
            for (const QString& name : folder.entryList({ "blender-*" }, QDir::Dirs | QDir::NoDotAndDotDot)) {
                candidates.append(folder.absoluteFilePath(name + "/blender.exe"));
            }
        }
    }
    QString best;
    QStringList bestVersion;
    const QStringList known = MakeBox::knownFolders();
    for (const QString& candidate : candidates) {
        if (!QFileInfo(candidate).isFile()) {
            continue;
        }
        // Blender by name, product name and signature, in its own folder: nothing else is ever opened to the box
        QString reason;
        if (!MakeBox::isBlender(candidate, reason)) {
            why = reason;
            continue;
        }
        const QString version = blenderVersion(candidate);
        if (!versionIsEnough(version)) {
            why = QString("Blender %1 is older than Make needs (%2.%3 or newer)")
                      .arg(version.isEmpty() ? QString("there") : version).arg(MINIMUM_MAJOR).arg(MINIMUM_MINOR);
            continue;
        }
        if (!isBlendersOwnFolder(QFileInfo(candidate).absolutePath(), version, known, reason)) {
            why = reason;
            continue;
        }
        const QStringList parts = version.split('.');
        bool newer = best.isEmpty();
        for (int i = 0; i < 3 && !newer && i < parts.size(); ++i) {
            if (parts.value(i).toInt() != bestVersion.value(i).toInt()) {
                newer = parts.value(i).toInt() > bestVersion.value(i).toInt();
                break;
            }
        }
        if (newer) {
            best = QFileInfo(candidate).absoluteFilePath();
            bestVersion = parts;
        }
    }
    if (!best.isEmpty()) {
        why.clear();
        return best;
    }
    if (why.isEmpty()) {
        why = QString("Blender isn't installed (blender.org, %1.%2 or newer)").arg(MINIMUM_MAJOR).arg(MINIMUM_MINOR);
    }
    return QString();
#elif defined(Q_OS_LINUX)
    // the official tarball from blender.org, unpacked: its "blender" program, an x86_64 ELF the box runs under Landlock
    // and seccomp as the Linux box test proved; the person's choice (the program or its folder), else the usual
    // places, the newest that is new enough wins. Never a distro package's /usr/bin/blender, a Flatpak or a Snap:
    // those start through wrappers, other loaders or sandboxes of their own that the box hasn't been proven on.
    // Cheap: folder names only, no process started (the dock asks whenever its window shows)
    QStringList candidates;
    if (!app.isEmpty()) {
        candidates.append(QFileInfo(app).isDir() ? QDir(app).absoluteFilePath("blender") : app);
    } else {
        const QStringList places { QDir::homePath(), QDir::homePath() + "/Downloads", QDir::homePath() + "/Applications",
                                   "/opt" };
        for (const QString& place : places) {
            const QDir folder(place);
            for (const QString& name : folder.entryList({ "blender*" }, QDir::Dirs | QDir::NoDotAndDotDot)) {
                candidates.append(folder.absoluteFilePath(name + "/blender"));
            }
        }
    }
    QString best;
    QStringList bestVersion;
    const QStringList forbidden = linuxForbiddenFolders();
    for (const QString& candidate : candidates) {
        const QFileInfo program(candidate);
        if (!program.isFile() || !program.isExecutable()) {
            continue;
        }
        // the folder the box is opened to is the canonical one (the watcher grants read+run on it): it must be
        // Blender's own, with Blender's version folder in it and none of the person's or the system's folders
        const QString real = realPath(candidate);
        const QString version = blenderVersion(real);
        if (!versionIsEnough(version)) {
            why = QString("Blender %1 is older than Make needs (%2.%3 or newer)")
                      .arg(version.isEmpty() ? QString("there") : version).arg(MINIMUM_MAJOR).arg(MINIMUM_MINOR);
            continue;
        }
        QString reason;
        if (!isBlendersOwnFolder(QFileInfo(real).absolutePath(), version, forbidden, reason)) {
            why = reason;
            continue;
        }
        const QStringList parts = version.split('.');
        bool newer = best.isEmpty();
        for (int i = 0; i < 3 && !newer && i < parts.size(); ++i) {
            if (parts.value(i).toInt() != bestVersion.value(i).toInt()) {
                newer = parts.value(i).toInt() > bestVersion.value(i).toInt();
                break;
            }
        }
        if (newer) {
            best = realPath(candidate);
            bestVersion = parts;
        }
    }
    if (!best.isEmpty()) {
        why.clear();
        return best;
    }
    if (why.isEmpty()) {
        why = QString("Blender isn't installed (the blender.org download, unpacked in your home folder, Downloads, "
                      "Applications or /opt; %1.%2 or newer)").arg(MINIMUM_MAJOR).arg(MINIMUM_MINOR);
    }
    return QString();
#elif !defined(Q_OS_MAC)
    Q_UNUSED(app);
    why = UNAVAILABLE_WORDS;
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
        if (!versionIsEnough(blenderVersion(candidate))) {
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
#ifdef Q_OS_WIN
    // only after this Connect's self-test passed (selfTest): the tools are offered only then, and this holds it too
    if (_selfTest != SelfTest::Passed) {
        made.code = MakeError::NOT_AVAILABLE_HERE;
        made.message = UNAVAILABLE_WORDS;
        done(made);
        return;
    }
#else
    const QString watch = _watch.isEmpty() ? defaultWatcher() : _watch;
    if (!QFileInfo(watch).isExecutable()) {
        made.code = MakeError::NOT_AVAILABLE_HERE;
        made.message = "Make's Blender watcher is missing from the app, so Blender can't be started safely";
        done(made);
        return;
    }
#endif
#ifdef Q_OS_LINUX
    // Blender's folder proven its own before any folder is opened to the box: the watcher grants read+run on the
    // canonical folder of the program it is given, so that folder is the one judged
    if (!isBlendersOwnFolder(QFileInfo(realPath(_blender)).absolutePath(), blenderVersion(realPath(_blender)),
                             linuxForbiddenFolders(), why)) {
        made.code = MakeError::NOT_AVAILABLE_HERE;
        made.message = why;
        done(made);
        return;
    }
#endif
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
    _blenderPid = 0;
    _done = std::move(done);
    _log.clear();
    _stopReason.clear();
    _stopCode.clear();

    _limit = new QTimer(this);
    _limit->setSingleShot(true);
    _limit->setTimerType(Qt::PreciseTimer);   // the limit means what it says (a coarse timer may fire 5% early)
    connect(_limit, &QTimer::timeout, this, [this] {
        _stopCode = MakeError::TIMEOUT;
        _stopReason = QString("Blender ran past its %1 s limit and was stopped: make it simpler or faster")
                          .arg(_timeLimitMs / 1000);
        kill();
    });
    _memory = new QTimer(this);
    connect(_memory, &QTimer::timeout, this, &MakeBlender::checkMemory);

#ifdef Q_OS_WIN
    // Blender in the box (MakeBox): the job is its keeper, the work folder its only writable place, the fixed scripts
    // read-only beside it, Make's two lines in an environment built from nothing
    MakeBox::Start boxed;
    boxed.blender = _blender;
    boxed.arguments = QStringList {
        "--background", "--factory-startup", "--disable-autoexec", "-noaudio", "--threads", QString::number(THREADS),
        "--python-exit-code", "3",
        "--python", QDir::toNativeSeparators(realFixed + "/prelude.py"),
        "--python", QDir::toNativeSeparators(realWork + "/job.py"),
        "--python", QDir::toNativeSeparators(realFixed + "/finish.py"),
        "--python", QDir::toNativeSeparators(realFixed + "/views.py"),
    };
    boxed.jobFolder = _job;
    boxed.workingFolder = realWork;
    boxed.readable = QStringList { realFixed };
    if (!MakeBox::underProgramFiles(_blender)) {
        boxed.readable.append(QFileInfo(_blender).absolutePath());  // the unpacked zip: its own folder, read and run
    }
    boxed.environment = QStringList { "MAKE_OUT=" + QDir::toNativeSeparators(realWork + "/out"),
                                      "MAKE_KIND=" + QString(kind == "room" ? "room" : "object"),
                                      "PYTHONDONTWRITEBYTECODE=1", "PYTHONNOUSERSITE=1" };
    boxed.cpuSeconds = _cpuLimitS;
    boxed.memoryBytes = MEMORY_LIMIT_BYTES;
    boxed.lpac = _lpac;
    // Blender proven Blender, and its folder its own, before any folder is opened to the box
    const bool fit = MakeBox::isBlender(_blender, why) &&
                     (MakeBox::underProgramFiles(_blender) ||
                      isBlendersOwnFolder(QFileInfo(_blender).absolutePath(), blenderVersion(_blender),
                                          MakeBox::knownFolders(), why));
    if (fit) {
        _box = MakeBox::start(boxed, why);
    }
    if (!_box) {
        for (QTimer* timer : { _limit, _memory }) {
            timer->deleteLater();
        }
        _limit = nullptr;
        _memory = nullptr;
        QDir(_job).removeRecursively();
        _job.clear();
        made.code = MakeError::NOT_AVAILABLE_HERE;
        made.message = fit ? "Make couldn't start Blender in its box: " + why : why;
        finish(made);
        return;
    }
    _started = QDateTime::currentMSecsSinceEpoch();
    _limit->start(_timeLimitMs);
    _memory->start(MEMORY_CHECK_MS);   // also watches for Blender's end
    qCInfo(makelink) << "Blender started for a make";
    return;
#else
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

    // the watcher's cap: its wall-clock seconds, and on Linux also the CPU time it gives Blender (cap, cap + 2)
    const int watchCap = qBound(1, qMax(_cpuLimitS, _timeLimitMs / 1000 + 20), 3600);

    // the watcher, Blender's parent: its standard input is a pipe from here (a line stops Blender, its end means the
    // app is gone); Blender gets /dev/null, and the watcher's output, where its log is read. On the Mac Blender keeps
    // the watcher's CPU cap; on Linux the watcher sets Blender's own, which may only lower its hard limit (raising one
    // needs privilege), so the watcher is capped at the cap it gives. Make's own limits give the same number either way
#if defined(Q_OS_LINUX)
    _process = new CappedProcess(watchCap, this);
#else
    _process = new CappedProcess(_cpuLimitS, this);
#endif
    _process->setProcessEnvironment(environment);
    _process->setWorkingDirectory(realWork);
    _process->setProcessChannelMode(QProcess::MergedChannels);
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

#if defined(Q_OS_MAC)
    const QStringList arguments {
        QString::number(watchCap), _job + "/blender.pid", "--", "/usr/bin/sandbox-exec",
        "-f", realFixed + "/box.sb",
        "-D", "BLENDER_APP=" + _blender,
        "-D", "FIXED=" + realFixed,
        "-D", "WORK=" + realWork,
        _blender + "/Contents/MacOS/Blender",
        "--background", "--factory-startup", "--disable-autoexec", "-noaudio", "--threads", QString::number(THREADS),
        "--python-exit-code", "3",
        "--python", realFixed + "/prelude.py",
        "--python", realWork + "/job.py",
        "--python", realFixed + "/finish.py",
        "--python", realFixed + "/views.py",
    };
#else
    // Linux: the watcher is the box (its child takes the limits, Landlock and seccomp with the exec gate before
    // Blender starts): it is given the work folder (the box's only writable place, Blender's home and temp), the fixed
    // scripts (read only), Make's two lines for the environment it builds from nothing, and the work folder's size cap;
    // the pid file stays outside the work folder
    const QStringList arguments {
        QString::number(watchCap), _job + "/blender.pid",
        "--job", realWork, "--fixed", realFixed,
        "--env", "MAKE_OUT=" + realWork + "/out",
        "--env", QString("MAKE_KIND=") + (kind == "room" ? "room" : "object"),
        "--folder-cap", QString::number(FOLDER_CAP_BYTES / (1024 * 1024)),
        "--", _blender,
        "--background", "--factory-startup", "--disable-autoexec", "-noaudio", "--threads", QString::number(THREADS),
        "--python-exit-code", "3",
        "--python", realFixed + "/prelude.py",
        "--python", realWork + "/job.py",
        "--python", realFixed + "/finish.py",
        "--python", realFixed + "/views.py",
    };
#endif
    _started = QDateTime::currentMSecsSinceEpoch();
    _process->start(watch, arguments);
    _limit->start(_timeLimitMs);
    _memory->start(MEMORY_CHECK_MS);
    qCInfo(makelink) << "Blender started for a make";
#endif
}

// Blender's id as the watcher wrote it, outside the folder the box may write, and only while Blender is the watcher's
// child: read for the memory check (and the tests), never to signal
qint64 MakeBlender::blenderPid() {
#ifdef Q_OS_MAC
    if (!_process || _job.isEmpty()) {
        return 0;
    }
    if (_blenderPid == 0) {
        QFile file(_job + "/blender.pid");
        if (QFileInfo(file.fileName()).isSymLink() || !file.open(QIODevice::ReadOnly)) {
            return 0;
        }
        bool number = false;
        const qint64 pid = QString::fromLatin1(file.read(32)).trimmed().toLongLong(&number);
        if (!number || pid <= 1) {
            return 0;
        }
        _blenderPid = pid;
    }
    proc_bsdinfo info;
    if (proc_pidinfo(static_cast<pid_t>(_blenderPid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info) ||
        static_cast<qint64>(info.pbi_ppid) != _process->processId()) {
        return 0;
    }
    return _blenderPid;
#elif defined(Q_OS_WIN)
    // from the job's own list (MakeBox): never a name, never signalled from here
    return _box ? MakeBox::pid(_box) : 0;
#elif defined(Q_OS_LINUX)
    if (!_process || _job.isEmpty()) {
        return 0;
    }
    if (_blenderPid == 0) {
        QFile file(_job + "/blender.pid");
        if (QFileInfo(file.fileName()).isSymLink() || !file.open(QIODevice::ReadOnly)) {
            return 0;
        }
        bool number = false;
        const qint64 pid = QString::fromLatin1(file.read(32)).trimmed().toLongLong(&number);
        if (!number || pid <= 1) {
            return 0;
        }
        _blenderPid = pid;
    }
    // still the watcher's child: /proc/<pid>/stat names the parent right after the command's closing parenthesis
    QFile stat(QString("/proc/%1/stat").arg(_blenderPid));
    if (!stat.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QByteArray text = stat.read(4096);
    const int command = text.lastIndexOf(')');
    const QList<QByteArray> fields = text.mid(command + 2).split(' ');  // the state, then the parent's id
    if (command < 0 || fields.size() < 2 || fields.at(1).toLongLong() != _process->processId()) {
        return 0;
    }
    return _blenderPid;
#else
    return 0;
#endif
}

void MakeBlender::checkMemory() {
#ifdef Q_OS_MAC
    if (!_process) {
        return;
    }
    rusage_info_v2 usage;
    const pid_t pid = static_cast<pid_t>(blenderPid());
    if (pid > 0 && proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&usage)) == 0 &&
        static_cast<qint64>(usage.ri_phys_footprint) > MEMORY_LIMIT_BYTES) {
        _stopCode = MakeError::BAD_INPUT;
        _stopReason = QString("Blender used more than %1 GB of memory and was stopped: make it lighter")
                          .arg(MEMORY_LIMIT_BYTES / (1024 * 1024 * 1024));
        kill();
    }
#elif defined(Q_OS_WIN)
    // the same tick watches for Blender's end (the job holds no other process); the job's own memory cap ends a make
    // past it too, this gives it the plain line
    if (!_box) {
        return;
    }
    int exitCode = 0;
    if (MakeBox::exited(_box, exitCode)) {
        finished(exitCode, static_cast<quint32>(exitCode) >= 0xC0000000u);
        return;
    }
    if (MakeBox::peakMemory(_box) > MEMORY_LIMIT_BYTES) {
        _stopCode = MakeError::BAD_INPUT;
        _stopReason = QString("Blender used more than %1 GB of memory and was stopped: make it lighter")
                          .arg(MEMORY_LIMIT_BYTES / (1024 * 1024 * 1024));
        kill();
    }
#elif defined(Q_OS_LINUX)
    // the resident memory /proc reports for the pid the watcher wrote (read only, never signalled by it)
    if (!_process) {
        return;
    }
    const qint64 pid = blenderPid();
    if (pid <= 0) {
        return;
    }
    QFile status(QString("/proc/%1/status").arg(pid));
    if (status.open(QIODevice::ReadOnly) && vmRssBytes(status.read(16384)) > MEMORY_LIMIT_BYTES) {
        _stopCode = MakeError::BAD_INPUT;
        _stopReason = QString("Blender used more than %1 GB of memory and was stopped: make it lighter")
                          .arg(MEMORY_LIMIT_BYTES / (1024 * 1024 * 1024));
        kill();
    }
#endif
}

// through the watcher: a line on its standard input, then its end; it kills Blender (the sandbox lets Blender start no
// other program, so the one process is all there is), reaps it and exits. On Windows the job is ended: Blender is its
// one process
void MakeBlender::kill() {
#ifdef Q_OS_WIN
    if (_box) {
        MakeBox::terminate(_box);
    }
#else
    if (_process && _process->state() != QProcess::NotRunning) {
        _process->write("stop\n");
        _process->closeWriteChannel();
    }
#endif
}

void MakeBlender::cancel() {
#ifdef Q_OS_WIN
    if (_box) {
        _stopCode = MakeError::REFUSED;
        _stopReason = "the make was stopped";
        kill();
        int exitCode = 1;
        if (!MakeBox::wait(_box, STOP_WAIT_MS)) {
            qCWarning(makelink) << "Blender's job didn't end in time; closing it";
        }
        MakeBox::exited(_box, exitCode);
        finished(exitCode, false);
    }
    return;
#endif
    if (_process) {
        _stopCode = MakeError::REFUSED;
        _stopReason = "the make was stopped";
        kill();
        if (_process && !_process->waitForFinished(STOP_WAIT_MS) && _process) {
            // the watcher has had its stop and its input's end; the last resort is the app's own child, the watcher
            qCWarning(makelink) << "Blender's watcher didn't end; killing it";
            _process->kill();
            if (_process) {
                _process->waitForFinished(1000);
            }
        }
    }
}

void MakeBlender::finished(int exitCode, bool crashed) {
#ifdef Q_OS_WIN
    if (!_box) {
        return;
    }
    // Blender's output, from the file it wrote through its inherited handle (the box can't open it by name)
    MakeBox::end(_box);
    _box = nullptr;
    QFile logFile(MakeBox::logPath(_job));
    if (logFile.open(QIODevice::ReadOnly)) {
        if (logFile.size() > MAX_LOG_BYTES) {
            logFile.seek(logFile.size() - MAX_LOG_BYTES);
        }
        _log = logFile.readAll();
    }
#else
    if (!_process) {
        return;
    }
    _log.append(_process->readAll());
    _process->deleteLater();
    _process = nullptr;
#endif
    _blenderPid = 0;
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
#ifndef Q_OS_WIN
    } else if (exitCode == WATCHER_CANT_START) {
        made.code = MakeError::NOT_AVAILABLE_HERE;
        made.message = "Make couldn't start Blender with its watcher";
#endif
    } else if (crashed || exitCode > 128 || exitCode != 0 || !QFileInfo::exists(out + "/model.glb")) {
        // the watcher says 128 + the signal when Blender was ended by one
        made.code = MakeError::BAD_INPUT;
        made.message = (crashed || exitCode > 128) ? QString("Blender stopped unexpectedly")
                                                    : QString("the script failed in Blender");
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

#ifdef Q_OS_WIN
namespace {

struct ProbeOutcome {
    bool known { false };   // the probe answered "refused" or "reached"
    bool held { false };    // the box's connection was refused
    bool lpac { true };     // the box form it ran under
    QString detail;
};

// the self-test, off the UI thread: the unboxed control first, then Blender in the box running selftest.py against
// the same target; the stricter box form first, the plain one when Blender wrote nothing under it (as the box test's
// ladder did). Nothing here touches the MakeBlender object.
ProbeOutcome runSelfTest(const QString& blender, const QString& job, bool lpacFirst, int cpuSeconds) {
    ProbeOutcome outcome;
    const QString address = MakeBox::ownAddress();
    if (address.isEmpty()) {
        outcome.detail = "the control found no network address of this computer";
        return outcome;
    }
    if (MakeBox::connectOnce(address, SELF_TEST_PORT, SELF_TEST_CONTROL_MS) != MakeBox::Reach::Connected) {
        outcome.detail = "the control couldn't reach this computer's own address";
        return outcome;
    }
    // Blender proven Blender, and its folder its own, before the probe opens any folder to the box
    QString reason;
    if (!MakeBox::isBlender(blender, reason) ||
        (!MakeBox::underProgramFiles(blender) &&
         !MakeBlender::isBlendersOwnFolder(QFileInfo(blender).absolutePath(), MakeBlender::blenderVersion(blender),
                                           MakeBox::knownFolders(), reason))) {
        outcome.detail = "Blender's program or folder was refused: " + reason;
        return outcome;
    }
    const QString fixed = job + "/fixed";
    const QString work = job + "/work";
    const QString result = work + "/selftest.txt";
    const bool forms[2] = { lpacFirst, false };
    for (int i = 0; i < (lpacFirst ? 2 : 1); ++i) {
        const bool lpac = forms[i];
        QFile::remove(result);
        MakeBox::Start boxed;
        boxed.blender = blender;
        boxed.arguments = QStringList { "--background", "--factory-startup", "--disable-autoexec", "-noaudio",
                                        "--python", QDir::toNativeSeparators(fixed + "/selftest.py"), "--",
                                        address, QString::number(SELF_TEST_PORT), QDir::toNativeSeparators(result) };
        boxed.jobFolder = job;
        boxed.workingFolder = work;
        boxed.readable = QStringList { fixed };
        if (!MakeBox::underProgramFiles(blender)) {
            boxed.readable.append(QFileInfo(blender).absolutePath());
        }
        boxed.environment = QStringList { "PYTHONDONTWRITEBYTECODE=1", "PYTHONNOUSERSITE=1" };
        boxed.cpuSeconds = cpuSeconds;
        boxed.memoryBytes = MakeBlender::MEMORY_LIMIT_BYTES;
        boxed.lpac = lpac;
        QString why;
        MakeBox::Run* run = MakeBox::start(boxed, why);
        if (!run) {
            outcome.detail = "the box didn't start: " + why;
            continue;
        }
        const bool ended = MakeBox::wait(run, SELF_TEST_PROBE_MS);
        int exitCode = 0;
        if (!ended) {
            MakeBox::terminate(run);
        }
        MakeBox::exited(run, exitCode);
        MakeBox::end(run);
        if (!ended) {
            outcome.detail = "the probe didn't end in time";
            break;
        }
        QFile file(result);
        if (!file.open(QIODevice::ReadOnly)) {
            outcome.detail = QString("the probe wrote nothing (Blender exit %1)").arg(exitCode);
            continue;   // Blender may not start under this form: the next one
        }
        bool held = false;
        if (!MakeBlender::readSelfTestResult(file.read(64), held)) {
            outcome.detail = "the probe's answer couldn't be read";
            break;
        }
        outcome.known = true;
        outcome.held = held;
        outcome.lpac = lpac;
        outcome.detail = held ? "the box couldn't reach this computer's own address"
                              : "the box reached this computer's own address";
        break;
    }
    return outcome;
}

}  // namespace
#endif

void MakeBlender::selfTest(std::function<void(bool, const QString&)> done) {
#ifdef Q_OS_WIN
    QString why;
    const QString blender = findBlender(why, _blender);
    if (blender.isEmpty()) {
        _selfTest = SelfTest::Failed;
        done(false, why);
        return;
    }
    // the installer's firewall rules: read and logged, never the decision (Group Policy can make Windows ignore local
    // rules while they still read as present and enabled), so the probe below decides
    QString rules;
    if (MakeFirewall::boxRulesInPlace(rules)) {
        qCInfo(makelink) << "the box's firewall rules: in place";
    } else {
        qCInfo(makelink) << "the box's firewall rules: not confirmed:" << rules;
    }
    const QString id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    const QString job = QDir(_folder).absoluteFilePath("jobs/selftest-" + id);
    if (!QDir().mkpath(job + "/fixed") || !QDir().mkpath(job + "/work") ||
        !copyResource("selftest.py", job + "/fixed/selftest.py")) {
        QDir(job).removeRecursively();
        _selfTest = SelfTest::Failed;
        qCWarning(makelink) << "the self-test couldn't make its folder";
        done(false, UNAVAILABLE_WORDS);
        return;
    }
    _selfTest = SelfTest::Running;
    _selfTestDone = std::move(done);
    const int round = ++_selfTestRound;
    const bool lpacFirst = _lpac;
    const int cpuSeconds = _cpuLimitS;
    QPointer<MakeBlender> self(this);
    // the answer comes back on this object's thread; a later Connect's round makes an earlier answer stale
    std::thread([self, round, blender, job, lpacFirst, cpuSeconds] {
        const ProbeOutcome outcome = runSelfTest(blender, job, lpacFirst, cpuSeconds);
        QDir(job).removeRecursively();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, round, outcome] {
            if (self) {
                self->selfTestEnded(round, outcome.known, outcome.held, outcome.detail, outcome.lpac);
            }
        }, Qt::QueuedConnection);
    }).detach();
#else
    QString why;
    const bool offered = available(why);
    _selfTest = offered ? SelfTest::Passed : SelfTest::Failed;
    done(offered, why);
#endif
}

void MakeBlender::selfTestEnded(int round, bool known, bool held, const QString& detail, bool lpac) {
    if (round != _selfTestRound) {
        return;   // a later Connect asked again; its own answer comes
    }
    const bool passed = known && held;
    _selfTest = passed ? SelfTest::Passed : SelfTest::Failed;
    if (known) {
        _lpac = lpac;
    }
    qCInfo(makelink) << "the box's self-test:" << (passed ? "passed" : "withdrew Blender") << "(" << detail << ")"
                     << (known ? (lpac ? "under the stricter form" : "under the plain form") : "");
    auto done = std::move(_selfTestDone);
    _selfTestDone = nullptr;
    if (done) {
        done(passed, passed ? QString() : QString(UNAVAILABLE_WORDS));
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
