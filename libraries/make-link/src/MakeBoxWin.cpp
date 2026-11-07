//
//  MakeBoxWin.cpp
//  libraries/make-link/src
//
//  Make's Blender box on Windows (MakeBox.h): the Win32 sequences the Windows box test proved, run by the app itself.
//  The job is Blender's keeper here (there is no watcher process as on the Mac): it is in the job before its first
//  instruction, the job ends it when the app closes the job or is gone, and the job's own list says its process id.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeBox.h"

#ifdef Q_OS_WIN

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <userenv.h>
#include <wincrypt.h>
#include <softpub.h>
#include <wintrust.h>

#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLoggingCategory>

#include "MakeBlender.h"
#include "MakeFirewall.h"

Q_DECLARE_LOGGING_CATEGORY(makelink)

// the Windows 10 start attributes, named here too for an SDK header that targets an older Windows
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
#define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif
#ifndef PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY
#define PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY ProcThreadAttributeValue(14, FALSE, TRUE, FALSE)
#endif
#ifndef PROCESS_CREATION_CHILD_PROCESS_RESTRICTED
#define PROCESS_CREATION_CHILD_PROCESS_RESTRICTED 0x01
#endif
#ifndef PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY
#define PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY ProcThreadAttributeValue(15, FALSE, TRUE, FALSE)
#endif
#ifndef PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT
#define PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT 0x01
#endif

namespace MakeBox {

struct Run {
    PROCESS_INFORMATION process {};
    HANDLE job { nullptr };
    HANDLE output { nullptr };
    HWINSTA station { nullptr };
    HDESK desktop { nullptr };
    PSID sid { nullptr };
};

}  // namespace MakeBox

namespace {

const wchar_t* const LOG_NAME = L"blender-log.txt";
const wchar_t* const DESKTOP_NAME = L"DarkrideMakeBox";

// the job's UI limits: all of them, always; the box gets a desktop of its own instead
const DWORD UI_FULL = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                      JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                      JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;

// a window station and desktop of the box's own, so the job keeps all its UI limits and Blender never shares the
// person's desktop; opened to the container's SID only
const DWORD DESKTOP_EVERYTHING = DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL |
                                 DESKTOP_JOURNALPLAYBACK | DESKTOP_JOURNALRECORD | DESKTOP_READOBJECTS |
                                 DESKTOP_SWITCHDESKTOP | DESKTOP_WRITEOBJECTS | READ_CONTROL | WRITE_DAC;

std::wstring wide(const QString& text) {
    return text.toStdWString();
}

std::wstring nativePath(const QString& path) {
    return wide(QDir::toNativeSeparators(path));
}

// Make's AppContainer profile: made when missing (as the normal user), else its SID derived from the name. The
// installer's firewall rules name the SID Windows derives from this fixed name.
PSID profileSid() {
    PSID sid = nullptr;
    HRESULT made = CreateAppContainerProfile(MakeFirewall::PROFILE_NAME, L"Darkride Make (Blender)",
                                             L"Darkride Make's Blender box", nullptr, 0, &sid);
    if (made == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        made = DeriveAppContainerSidFromAppContainerName(MakeFirewall::PROFILE_NAME, &sid);
    }
    return SUCCEEDED(made) ? sid : nullptr;
}

bool grant(const std::wstring& path, PSID sid, DWORD access) {
    PACL oldDacl = nullptr;
    PACL newDacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldDacl, nullptr,
                              &descriptor) != ERROR_SUCCESS) {
        return false;
    }
    EXPLICIT_ACCESSW entry {};
    entry.grfAccessPermissions = access;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    const bool ok = SetEntriesInAclW(1, &entry, oldDacl, &newDacl) == ERROR_SUCCESS &&
                    SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                          nullptr, nullptr, newDacl, nullptr) == ERROR_SUCCESS;
    LocalFree(newDacl);
    LocalFree(descriptor);
    return ok;
}

bool grantObject(HANDLE object, PSID sid, DWORD access) {
    PACL oldDacl = nullptr;
    PACL newDacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(object, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldDacl, nullptr,
                        &descriptor) != ERROR_SUCCESS) {
        return false;
    }
    EXPLICIT_ACCESSW entry {};
    entry.grfAccessPermissions = access;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    const bool ok = SetEntriesInAclW(1, &entry, oldDacl, &newDacl) == ERROR_SUCCESS &&
                    SetSecurityInfo(object, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, newDacl,
                                    nullptr) == ERROR_SUCCESS;
    LocalFree(newDacl);
    LocalFree(descriptor);
    return ok;
}

// the run's own window station and desktop; `name` is "<window station>\<desktop>" for the start, empty on failure.
// The switch of the process's window station is process-wide state, and the self-test's thread and a make can
// overlap, so one at a time
std::mutex desktopSwitch;

bool makeOwnDesktop(MakeBox::Run& run, std::wstring& name) {
    std::lock_guard<std::mutex> oneAtATime(desktopSwitch);
    HWINSTA original = GetProcessWindowStation();
    run.station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS | READ_CONTROL | WRITE_DAC, nullptr);
    if (!run.station || !SetProcessWindowStation(run.station)) {
        return false;
    }
    run.desktop = CreateDesktopW(DESKTOP_NAME, nullptr, nullptr, 0, DESKTOP_EVERYTHING, nullptr);
    SetProcessWindowStation(original);
    if (!run.desktop) {
        return false;
    }
    wchar_t station[256] {};
    DWORD size = 0;
    GetUserObjectInformationW(run.station, UOI_NAME, station, sizeof station, &size);
    name = std::wstring(station) + L"\\" + DESKTOP_NAME;
    if (!grantObject(run.station, run.sid, WINSTA_ALL_ACCESS) ||
        !grantObject(run.desktop, run.sid, DESKTOP_EVERYTHING & ~(READ_CONTROL | WRITE_DAC))) {
        name.clear();
        return false;
    }
    return true;
}

// the job: one process, no children, ended when the job's last handle closes, a memory and a CPU cap, every UI limit
HANDLE makeJob(int cpuSeconds, qint64 memoryBytes) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        return nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                              JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME |
                                              JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = static_cast<LONGLONG>(cpuSeconds) * 10000000LL;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(memoryBytes);
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui {};
    ui.UIRestrictionsClass = UI_FULL;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        !SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui)) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

std::wstring envValue(const wchar_t* name) {
    wchar_t value[1024];
    const DWORD size = GetEnvironmentVariableW(name, value, 1024);
    return size > 0 && size < 1024 ? std::wstring(value, size) : std::wstring();
}

// Blender's whole environment: the system's few, the standard folder locations, its own folders in the work folder,
// and Make's lines; nothing else of the person's (no key, no other variable)
std::wstring environmentBlock(const std::wstring& work, const QStringList& extra) {
    const std::wstring root = envValue(L"SystemRoot");
    std::vector<std::wstring> lines = {
        L"SystemRoot=" + root, L"windir=" + root, L"PATH=" + root + L"\\System32",
        L"NUMBER_OF_PROCESSORS=" + envValue(L"NUMBER_OF_PROCESSORS"),
        L"PROCESSOR_ARCHITECTURE=" + envValue(L"PROCESSOR_ARCHITECTURE"),
        L"TEMP=" + work, L"TMP=" + work, L"HOME=" + work,
        L"BLENDER_USER_CONFIG=" + work + L"\\config", L"BLENDER_USER_SCRIPTS=" + work + L"\\user-scripts",
        L"BLENDER_USER_DATAFILES=" + work + L"\\datafiles",
    };
    // the folder locations an AppContainer start needs to set the container's own folders up (without them
    // CreateProcess fails with ERROR_ENVVAR_NOT_FOUND); locations only, the box can't open them
    for (const wchar_t* name : { L"SystemDrive", L"USERPROFILE", L"LOCALAPPDATA", L"APPDATA", L"ProgramData",
                                 L"ProgramFiles", L"ALLUSERSPROFILE", L"PUBLIC" }) {
        const std::wstring value = envValue(name);
        if (!value.empty()) {
            lines.push_back(std::wstring(name) + L"=" + value);
        }
    }
    for (const QString& line : extra) {
        lines.push_back(wide(line));
    }
    std::wstring block;
    for (const auto& line : lines) {
        block += line;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

// one argument on the command line as the C runtime reads it back: quoted when it has a space, a tab or a quote
std::wstring quoted(const QString& argument) {
    const std::wstring text = wide(argument);
    if (!text.empty() && text.find_first_of(L" \t\"") == std::wstring::npos) {
        return text;
    }
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : text) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            backslashes = 0;
            out += c;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

// the version resource's ProductName, through its translation table (the first language that has one)
QString productName(const std::wstring& file) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (size == 0) {
        return QString();
    }
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) {
        return QString();
    }
    struct Translation {
        WORD language;
        WORD codePage;
    };
    Translation* translations = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&translations), &length) ||
        !translations) {
        return QString();
    }
    for (UINT i = 0; i < length / sizeof(Translation); ++i) {
        wchar_t key[64] {};
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductName", translations[i].language, translations[i].codePage);
        wchar_t* value = nullptr;
        UINT valueLength = 0;
        if (VerQueryValueW(data.data(), key, reinterpret_cast<LPVOID*>(&value), &valueLength) && value && valueLength > 0) {
            const QString name = QString::fromWCharArray(value).trimmed();
            if (!name.isEmpty()) {
                return name;
            }
        }
    }
    return QString();
}

// the file's Authenticode signature verified (no UI, no revocation check, nothing fetched) and its signer's subject
// name, as its simple display name (the common name); false with `why` when it doesn't verify
bool signerOf(const std::wstring& file, QString& signer, QString& why) {
    WINTRUST_FILE_INFO fileInfo {};
    fileInfo.cbStruct = sizeof fileInfo;
    fileInfo.pcwszFilePath = file.c_str();
    WINTRUST_DATA trust {};
    trust.cbStruct = sizeof trust;
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &fileInfo;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    HWND noWindow = static_cast<HWND>(INVALID_HANDLE_VALUE);
    const LONG status = WinVerifyTrust(noWindow, &action, &trust);
    bool ok = false;
    if (status == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* data = WTHelperProvDataFromStateData(trust.hWVTStateData);
        CRYPT_PROVIDER_SGNR* chain = data ? WTHelperGetProvSignerFromChain(data, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* cert = chain ? WTHelperGetProvCertFromChain(chain, 0) : nullptr;
        if (cert && cert->pCert) {
            wchar_t subject[512] {};
            if (CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, subject, 512) > 1) {
                signer = QString::fromWCharArray(subject);
                ok = true;
            }
        }
        if (!ok) {
            why = "That program's signer couldn't be read.";
        }
    } else {
        why = QString("That program isn't signed, or its signature doesn't verify (0x%1).")
                  .arg(static_cast<quint32>(status), 8, 16, QChar('0'));
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(noWindow, &action, &trust);
    return ok;
}

// the box's token and job, read from the child while it is still suspended: an AppContainer token for Make's SID,
// and in the job; nothing of Blender runs before this holds
bool provenBoxed(MakeBox::Run* run, QString& why) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(run->process.hProcess, TOKEN_QUERY, &token)) {
        why = "its token couldn't be read";
        return false;
    }
    DWORD isContainer = 0;
    DWORD length = 0;
    bool ok = GetTokenInformation(token, TokenIsAppContainer, &isContainer, sizeof isContainer, &length) &&
              isContainer == 1;
    if (!ok) {
        why = "its token isn't an AppContainer token";
    } else {
        std::vector<char> buffer(sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE + 64);
        auto info = reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(buffer.data());
        ok = GetTokenInformation(token, TokenAppContainerSid, buffer.data(), static_cast<DWORD>(buffer.size()), &length) &&
             info->TokenAppContainer && EqualSid(info->TokenAppContainer, run->sid);
        if (!ok) {
            why = "its token's container isn't Make's";
        }
    }
    CloseHandle(token);
    if (ok) {
        BOOL inJob = FALSE;
        ok = IsProcessInJob(run->process.hProcess, run->job, &inJob) && inJob;
        if (!ok) {
            why = "it isn't in the job";
        }
    }
    return ok;
}

}  // namespace

namespace MakeBox {

QString logPath(const QString& jobFolder) {
    return QDir(jobFolder).absoluteFilePath(QString::fromWCharArray(LOG_NAME));
}

bool underProgramFiles(const QString& path) {
    for (const wchar_t* name : { L"ProgramFiles", L"ProgramW6432" }) {
        const QString root = QDir::fromNativeSeparators(QString::fromStdWString(envValue(name)));
        if (!root.isEmpty() && QDir::fromNativeSeparators(path).startsWith(root + "/", Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

Run* start(const Start& given, QString& why) {
    Run* run = new Run;
    run->sid = profileSid();
    if (!run->sid) {
        why = "Make's box profile couldn't be made";
        end(run);
        return nullptr;
    }
    // what the box may touch: the work folder to read and write, the fixed scripts (and Blender's own folder when it
    // isn't under Program Files, which the box reads already) to read and run; nothing else is opened to it
    if (!grant(nativePath(given.workingFolder), run->sid, FILE_ALL_ACCESS)) {
        why = "the box couldn't be given its work folder";
        end(run);
        return nullptr;
    }
    for (const QString& folder : given.readable) {
        if (!grant(nativePath(folder), run->sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)) {
            why = "the box couldn't be given a folder to read";
            end(run);
            return nullptr;
        }
    }
    std::wstring desktop;
    if (!makeOwnDesktop(*run, desktop)) {
        why = "the box's own desktop couldn't be made";
        end(run);
        return nullptr;
    }
    run->job = makeJob(given.cpuSeconds, given.memoryBytes);
    if (!run->job) {
        why = "the box's job couldn't be made";
        end(run);
        return nullptr;
    }
    // Blender's output: a file in the job folder, which the box can't open by name; it writes through the handle
    SECURITY_ATTRIBUTES inheritable {};
    inheritable.nLength = sizeof inheritable;
    inheritable.bInheritHandle = TRUE;
    run->output = CreateFileW(nativePath(logPath(given.jobFolder)).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (run->output == INVALID_HANDLE_VALUE) {
        run->output = nullptr;
        why = "the box's output file couldn't be made";
        end(run);
        return nullptr;
    }

    // an AppContainer token with no capabilities (LPAC: also out of "ALL APPLICATION PACKAGES"), child processes
    // restricted, in the job from its first instruction (started suspended, the job given at creation), and only the
    // output handle inherited
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 5, 0, &size);
    std::vector<char> buffer(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 5, 0, &size)) {
        why = "the box's start couldn't be set up";
        end(run);
        return nullptr;
    }
    SECURITY_CAPABILITIES capabilities {};
    capabilities.AppContainerSid = run->sid;
    capabilities.Capabilities = nullptr;
    capabilities.CapabilityCount = 0;
    DWORD optOut = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;
    DWORD childPolicy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    HANDLE jobs[1] = { run->job };
    HANDLE handles[1] = { run->output };
    // every attribute must take, or Blender would start as the person, outside the job or able to start programs
    struct Attribute {
        DWORD_PTR id;
        PVOID value;
        SIZE_T size;
        const char* name;
        bool wanted;
    };
    const Attribute wantedAttributes[] = {
        { PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities, sizeof capabilities, "the container", true },
        { PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY, &optOut, sizeof optOut, "the stricter form", given.lpac },
        { PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &childPolicy, sizeof childPolicy, "no child processes", true },
        { PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof jobs, "the job", true },
        { PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles, "the one output handle", true },
    };
    for (const Attribute& attribute : wantedAttributes) {
        if (attribute.wanted &&
            !UpdateProcThreadAttribute(attributes, 0, attribute.id, attribute.value, attribute.size, nullptr, nullptr)) {
            why = QString("the box's start couldn't be set up: %1 (error %2)").arg(attribute.name).arg(GetLastError());
            DeleteProcThreadAttributeList(attributes);
            end(run);
            return nullptr;
        }
    }

    std::wstring command = quoted(QDir::toNativeSeparators(given.blender));
    for (const QString& argument : given.arguments) {
        command += L" " + quoted(argument);
    }
    const std::wstring work = nativePath(given.workingFolder);
    std::wstring environment = environmentBlock(work, given.environment);
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof startup;
    startup.lpAttributeList = attributes;
    startup.StartupInfo.lpDesktop = desktop.data();
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = run->output;
    startup.StartupInfo.hStdError = run->output;
    const std::wstring program = nativePath(given.blender);
    // DETACHED_PROCESS, not CREATE_NO_WINDOW: blender.exe is a console program, and a console means a conhost.exe
    // child, which the box's child-process policy refuses
    const BOOL started = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | DETACHED_PROCESS |
                                            CREATE_UNICODE_ENVIRONMENT,
                                        environment.data(), work.c_str(), &startup.StartupInfo, &run->process);
    const DWORD error = started ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!started) {
        why = QString("Blender didn't start in the box (error %1)").arg(error);
        end(run);
        return nullptr;
    }
    // the box proven on the suspended child, before its first instruction: its token is an AppContainer token for
    // Make's SID and it is in the job; otherwise it never runs
    QString unproven;
    if (!provenBoxed(run, unproven)) {
        TerminateProcess(run->process.hProcess, 1);
        why = "the box wasn't proven before Blender's first instruction: " + unproven;
        end(run);
        return nullptr;
    }
    ResumeThread(run->process.hThread);
    qCInfo(makelink) << "Blender started in its box" << (given.lpac ? "(the stricter form)" : "(the plain form)");
    return run;
}

bool exited(Run* run, int& exitCode) {
    if (!run || !run->process.hProcess) {
        exitCode = -1;
        return true;
    }
    DWORD code = STILL_ACTIVE;
    if (!GetExitCodeProcess(run->process.hProcess, &code)) {
        exitCode = -1;
        return true;
    }
    if (code == STILL_ACTIVE && WaitForSingleObject(run->process.hProcess, 0) != WAIT_OBJECT_0) {
        return false;
    }
    exitCode = static_cast<int>(code);
    return true;
}

bool wait(Run* run, int ms) {
    return run && run->process.hProcess && WaitForSingleObject(run->process.hProcess, static_cast<DWORD>(ms)) == WAIT_OBJECT_0;
}

qint64 pid(Run* run) {
    int code = 0;
    if (!run || exited(run, code)) {
        return 0;
    }
    // from the job's own list: the one process it holds (its active-process limit is one)
    std::vector<char> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 16 * sizeof(ULONG_PTR));
    auto list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
    if (QueryInformationJobObject(run->job, JobObjectBasicProcessIdList, list, static_cast<DWORD>(buffer.size()), nullptr) &&
        list->NumberOfProcessIdsInList > 0) {
        return static_cast<qint64>(list->ProcessIdList[0]);
    }
    return 0;
}

qint64 peakMemory(Run* run) {
    if (!run || !run->job) {
        return 0;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info {};
    if (!QueryInformationJobObject(run->job, JobObjectExtendedLimitInformation, &info, sizeof info, nullptr)) {
        return 0;
    }
    return static_cast<qint64>(info.PeakProcessMemoryUsed);
}

void terminate(Run* run) {
    if (run && run->job) {
        TerminateJobObject(run->job, 1);
    }
}

void end(Run* run) {
    if (!run) {
        return;
    }
    if (run->process.hThread) {
        CloseHandle(run->process.hThread);
    }
    if (run->process.hProcess) {
        CloseHandle(run->process.hProcess);
    }
    if (run->job) {
        CloseHandle(run->job);  // kill-on-close: nothing of the run outlives this
    }
    if (run->output) {
        CloseHandle(run->output);
    }
    if (run->desktop) {
        CloseDesktop(run->desktop);
    }
    if (run->station) {
        CloseWindowStation(run->station);
    }
    if (run->sid) {
        FreeSid(run->sid);
    }
    delete run;
}

QString ownAddress() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return QString();
    }
    QString address;
    // a UDP socket "connected" to a documentation address sends nothing, and names the local address it would use
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe != INVALID_SOCKET) {
        sockaddr_in documentation {};
        documentation.sin_family = AF_INET;
        documentation.sin_port = htons(9);
        inet_pton(AF_INET, "192.0.2.1", &documentation.sin_addr);
        sockaddr_in local {};
        int length = sizeof local;
        if (connect(probe, reinterpret_cast<sockaddr*>(&documentation), sizeof documentation) == 0 &&
            getsockname(probe, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
            char text[INET_ADDRSTRLEN] {};
            if (inet_ntop(AF_INET, &local.sin_addr, text, sizeof text)) {
                address = QString::fromLatin1(text);
            }
        }
        closesocket(probe);
    }
    WSACleanup();
    return address.startsWith("0.") ? QString() : address;
}

Reach connectOnce(const QString& address, int port, int timeoutMs) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return Reach::NotReached;
    }
    Reach reach = Reach::NotReached;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        sockaddr_in target {};
        target.sin_family = AF_INET;
        target.sin_port = htons(static_cast<u_short>(port));
        if (inet_pton(AF_INET, address.toLatin1().constData(), &target.sin_addr) == 1) {
            if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof target) == 0) {
                reach = Reach::Connected;
            } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set writable, failed;
                FD_ZERO(&writable);
                FD_ZERO(&failed);
                FD_SET(s, &writable);
                FD_SET(s, &failed);
                timeval limit { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
                const int ready = select(0, nullptr, &writable, &failed, &limit);
                if (ready > 0 && FD_ISSET(s, &writable)) {
                    reach = Reach::Connected;
                } else if (ready > 0 && FD_ISSET(s, &failed)) {
                    int error = 0;
                    int length = sizeof error;
                    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
                    reach = error == WSAECONNREFUSED ? Reach::Refused : Reach::NotReached;
                }
            }
        }
        closesocket(s);  // at once: nothing sent, nothing read
    }
    WSACleanup();
    return reach;
}

QString fileVersion(const QString& path) {
    const std::wstring file = nativePath(path);
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (size == 0) {
        return QString();
    }
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) {
        return QString();
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) || !info || length == 0) {
        return QString();
    }
    const int major = HIWORD(info->dwFileVersionMS);
    const int minor = LOWORD(info->dwFileVersionMS);
    const int patch = HIWORD(info->dwFileVersionLS);
    if (major == 0 && minor == 0 && patch == 0) {
        return QString();
    }
    return QString("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

// the signer certificate's subject of Blender's Windows builds, its common name as CertGetNameStringW gives it with
// CERT_NAME_SIMPLE_DISPLAY_TYPE, matched whole and case-sensitively (a near name never passes). Read from a real
// Blender 5.2 blender.exe on the Windows lab laptop (11-07): the signature is valid and the subject is
// "CN=Blender Foundation, O=Blender Foundation, L=Amsterdam, S=Noord-Holland, C=NL", so the name is exactly this.
const char* const BLENDER_SIGNER = "Blender Foundation";

bool isBlender(const QString& exe, QString& why) {
    const QFileInfo info(exe);
    if (!info.isFile()) {
        why = "That isn't a program file.";
        return false;
    }
    const std::wstring file = nativePath(info.absoluteFilePath());
    if (!MakeBlender::looksLikeBlender(info.fileName(), productName(file), why)) {
        return false;
    }
    QString signer;
    if (!signerOf(file, signer, why)) {
        return false;
    }
    if (signer != QString::fromLatin1(BLENDER_SIGNER)) {
        why = "That program isn't signed by the Blender Foundation.";
        return false;
    }
    return true;
}

QStringList knownFolders() {
    QStringList folders;
    for (const KNOWNFOLDERID* id : { &FOLDERID_Profile, &FOLDERID_Downloads, &FOLDERID_Desktop, &FOLDERID_Documents,
                                     &FOLDERID_LocalAppData, &FOLDERID_RoamingAppData, &FOLDERID_Public }) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, nullptr, &path)) && path) {
            folders.append(QDir::cleanPath(QDir::fromNativeSeparators(QString::fromWCharArray(path))));
        }
        CoTaskMemFree(path);
    }
    return folders;
}

}  // namespace MakeBox

#endif  // Q_OS_WIN
