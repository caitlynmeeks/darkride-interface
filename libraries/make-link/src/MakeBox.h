//
//  MakeBox.h
//  libraries/make-link/src
//
//  Make's Blender box on Windows: Blender started as the normal user inside an AppContainer (Make's own profile,
//  Darkride.MakeBlender, with no capabilities), inside a job object from its first instruction (kill-on-close, one
//  process, no children, a memory and a CPU cap, every UI limit), on a window station and desktop of its own, with an
//  environment built from nothing, its output into a file the box can't open by name, and only the folders Make
//  grants it: the job's work folder to read and write, the fixed scripts (and Blender's own folder when it isn't under
//  Program Files) to read and run. The same box runs the Connect self-test's probe. This is the product form of the
//  box the Windows box test proved (tests-manual/make-link/windows-box).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_MakeBox_h
#define hifi_MakeBox_h

#include <QtCore/QString>
#include <QtCore/QStringList>

#ifdef Q_OS_WIN

namespace MakeBox {

/// One boxed Blender: its process, its job, its desktop, its output file. Made by start(), freed by end().
struct Run;

/// What a run is given.
struct Start {
    QString blender;           // blender.exe
    QStringList arguments;     // after it
    QString jobFolder;         // the make's folder: not granted to the box; its output file is written here
    QString workingFolder;     // Blender's folder, TEMP and its own config folders: granted to read and write
    QStringList readable;      // granted to read and run (the fixed scripts; Blender's folder outside Program Files)
    QStringList environment;   // NAME=value lines added to the box's own few (MAKE_OUT, MAKE_KIND, ...)
    int cpuSeconds { 0 };      // the job's cap on Blender's CPU time
    qint64 memoryBytes { 0 };  // the job's cap on its memory
    bool lpac { true };        // the stricter form (also out of ALL APPLICATION PACKAGES); the plain form when false
};

/// Starts Blender in the box; null with `why` when it couldn't (the profile, a grant, the desktop, the job or the start).
Run* start(const Start& start, QString& why);
/// Whether Blender has ended, and its exit code then.
bool exited(Run* run, int& exitCode);
/// Waits up to `ms` for Blender to end; true when it has.
bool wait(Run* run, int ms);
/// Blender's process id (0 after it ended).
qint64 pid(Run* run);
/// The most memory Blender's process has used so far, in bytes (the job's figure).
qint64 peakMemory(Run* run);
/// Ends Blender now: the job is terminated, so nothing of it stays.
void terminate(Run* run);
/// Closes everything of the run (the job's kill-on-close ends anything left) and frees it.
void end(Run* run);
/// Where a run's standard output and error go: a file in the job folder.
QString logPath(const QString& jobFolder);

/// This computer's own network address (the one it would send from), or empty.
QString ownAddress();
enum class Reach { Connected, Refused, NotReached };
/// A bare TCP connection from this process, unboxed, to `address`:`port`, closed at once, nothing sent or read: the
/// self-test's control.
Reach connectOnce(const QString& address, int port, int timeoutMs);
/// A program file's version as its version resource says ("5.2.2"); empty when it has none.
QString fileVersion(const QString& path);
/// Whether a path is under Program Files (where the box may read already).
bool underProgramFiles(const QString& path);

/// The name Blender's Windows builds are signed with: the signer certificate's subject, read as its simple display
/// name (CertGetNameStringW with CERT_NAME_SIMPLE_DISPLAY_TYPE, the common name) and matched whole, case-sensitively.
extern const char* const BLENDER_SIGNER;
/// Whether `exe` is Blender, before any folder is opened to the box and before any make: a file named blender.exe,
/// its version resource's ProductName "Blender", and its Authenticode signature verifying (WinVerifyTrust, no UI, no
/// revocation check, nothing fetched) with the signer's name exactly BLENDER_SIGNER. False with a plain `why`.
bool isBlender(const QString& exe, QString& why);
/// The person's known folders a Blender's own folder must never be nor contain: the profile, Downloads, Desktop,
/// Documents, Local and Roaming AppData, Public (canonical, forward slashes).
QStringList knownFolders();

}  // namespace MakeBox

#endif  // Q_OS_WIN

#endif  // hifi_MakeBox_h
