//
//  MakeBlender.h
//  libraries/make-link/src
//
//  Make's Blender tool: Claude's Blender Python script, then our fixed export and views, run by the builder's own
//  Blender in an OS sandbox (security D): `--background --factory-startup --disable-autoexec`, under sandbox-exec
//  with a profile that denies everything but Blender itself, our fixed scripts (read only), one fresh scratch folder and
//  the GPU. No network, no home folder, no other program's services. Its environment is built from nothing (no key,
//  nothing of the app's), and it is killed at its time and memory limits. macOS only: elsewhere it isn't available.
//
//  What comes back is treated as untrusted, because the script runs in the same Python as our export and could change
//  it: the .glb is size-capped and its structure checked (glTF 2.0 binary, everything inside it, no outside address),
//  the report keeps only known fields, cut short, and the four views are decoded with size caps and drawn again into
//  one picture. Reference pictures go in only as the clean copies the app made, as texture files (security E18).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_MakeBlender_h
#define hifi_MakeBlender_h

#include <functional>

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtCore/QVector>

class QTimer;

class MakeBlender : public QObject {
    Q_OBJECT
public:
    /// A reference picture for the script: the app's clean copy (re-encoded, no metadata), and the file name the
    /// script loads it by (letters, digits, '-', '_' and '.', ending .png or .jpg).
    struct Picture {
        QString path;
        QString as;
    };
    /// What a make gave: a model (its id, its checked .glb in the models folder, its size), the report, the four
    /// views as one JPEG, the seconds it took; or an error code (MakeError) with a plain line and Blender's last lines.
    struct Made {
        QString code;
        QString message;
        QString modelId;
        QString glbPath;
        qint64 bytes { 0 };
        QJsonObject report;
        QByteArray views;
        QString viewsType;
        double seconds { 0.0 };
        QString log;
        bool ok() const { return code.isEmpty(); }
    };

    static const qint64 MAX_GLB_BYTES;
    static const int TIME_LIMIT_MS;
    static const qint64 MEMORY_LIMIT_BYTES;
    /// Blender's threads (its --threads): a make's CPU time can then be capped close to its time limit.
    static const int THREADS;
    /// The kernel's cap on Blender's CPU time, in seconds (RLIMIT_CPU, set in the child before exec): it holds when the
    /// app is gone (crashed or force-quit mid-make) and its own timers can't stop Blender. THREADS x the time limit,
    /// and a little more, so a make the app would let finish never meets it.
    static const int CPU_LIMIT_S;
    /// The oldest Blender the fixed scripts are written for.
    static const int MINIMUM_MAJOR;
    static const int MINIMUM_MINOR;

    /// `folder` holds the makes: a fresh scratch folder for each under jobs/, the checked models under models/.
    explicit MakeBlender(const QString& folder, QObject* parent = nullptr);
    ~MakeBlender() override;

    /// The Blender app to use: `app` if given (the person's choice), else the usual places. Empty with `why` when there
    /// is none, it's too old, or this system can't box it.
    static QString findBlender(QString& why, const QString& app = QString());
    /// The app's version as its Info.plist says ("5.2.2"); empty when it has none.
    static QString blenderVersion(const QString& app);

    void setBlender(const QString& app) { _blender = app; }
    /// A make's time limit (TIME_LIMIT_MS unless set, for tests).
    void setTimeLimit(int milliseconds) { _timeLimitMs = milliseconds; }
    /// The kernel's CPU cap (CPU_LIMIT_S unless set, for tests).
    void setCpuLimit(int seconds) { _cpuLimitS = seconds; }
    /// The running make's process id, and its watcher's (0 when none), for tests.
    qint64 processId() const { return _process ? _process->processId() : 0; }
    qint64 watcherId() const { return _watcher ? _watcher->processId() : 0; }
    QString blender() const { return _blender; }
    bool isBusy() const { return _process != nullptr; }

    /// One make: answers once through `done`, on this object's thread. `kind` is "object" or "room".
    void make(const QString& script, const QString& kind, const QVector<Picture>& pictures,
              std::function<void(const Made&)> done);
    /// Ends the running make at once (Stop, Disconnect, quit); its `done` answers that it was stopped.
    void cancel();
    /// The checked model of an earlier make, by its id; empty when there is none.
    QString modelPath(const QString& modelId) const;
    /// Removes the models made so far (when the request they belong to ends).
    void forgetModels();

    /// The structure check of a model: glTF 2.0 binary, its length and chunks right, its JSON whole, every buffer and
    /// image inside the file (no address to fetch), and no required extension outside the known few.
    static bool checkGlb(const QByteArray& bytes, QString& why);
    /// The report as it may go to Claude: known fields only, strings and lists cut short.
    static QJsonObject cleanReport(const QJsonObject& report);
    /// The four views (front, right, back, top: PNGs from the sandbox) decoded with size caps and drawn into one 2x2
    /// JPEG; empty when one is missing or too big.
    static QByteArray drawViews(const QStringList& pngs, QString& mimeType);

private:
    void finished(int exitCode, bool crashed);
    void checkMemory();
    void finish(Made made);
    void kill();
    bool startWatcher(qint64 pid);
    void endWatcher();

    QString _folder;
    QString _blender;
    QString _job;
    QPointer<QProcess> _process;
    QPointer<QProcess> _watcher;
    QTimer* _limit { nullptr };
    QTimer* _memory { nullptr };
    qint64 _started { 0 };
    QString _stopReason;
    QString _stopCode;
    int _timeLimitMs { TIME_LIMIT_MS };
    int _cpuLimitS { CPU_LIMIT_S };
    QByteArray _log;
    std::function<void(const Made&)> _done;
    QStringList _models;
};

#endif // hifi_MakeBlender_h
