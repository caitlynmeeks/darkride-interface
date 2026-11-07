//
//  make-box-test.cpp
//  tests-manual/make-link/windows-box
//
//  Make v1 on Windows (6b), the first question (security's plan, team review make-blender-box-windows-linux): can
//  Blender run, and render, inside the box Make will use, and does the box refuse what it must? A stand-alone test
//  (no Qt, no Darkride, no Claude, no key, no outside service), run as the normal user, headless:
//    1. a Darkride-only AppContainer profile with NO capabilities, the stricter LPAC form first, the plain form if
//       Blender won't start under it;
//    2. a job object set before Blender's first instruction (the job list attribute on a suspended start): one process,
//       child processes restricted, kill-on-close, 4 GB, 10 min of CPU, the UI limits;
//    3. the container's SID granted the job folder (read and write) and the scripts folder (read) only;
//    4. Blender --background --factory-startup --disable-autoexec -noaudio running probe.py (the refusals and the
//       four views, on the GPU and on the CPU);
//    5. the kill-the-parent case: a parent copy of this program starts a boxed Blender that loops; this program ends
//       the parent; Blender must end with it (the job's own pids, never a name).
//  Writes <test folder>\results.json (each check's pass and refused/opened, counts) and deletes the job folder, the
//  container profile and its throwaway test credential afterwards.
//    make-box-test.exe <blender.exe> <test folder> [--mac <address>:<port>] [--system-port] [--lan-listener]
//  Run 6 (security, 11-07): every network check runs twice, boxed and as an UNBOXED control (the same Blender, the
//  box's environment, no container, no job), so a refusal in the box means something. The checks: the program's own
//  loopback listener, a closed port on the laptop's own LAN address, the laptop's own computer name (resolved or not,
//  nothing kept), a name under .invalid, and with the flags:
//    --mac <address>:<port>  a listener scripting runs on the Mac (off the laptop, on the home network)
//    --system-port           Windows' own RPC endpoint mapper (TCP 135 on the LAN address): a bare connect, closed at
//                            once, nothing sent or read; no listener of ours, so no firewall alert
//    --lan-listener          a listener of this program's own on the LAN address (the Connect self-test's shape).
//                            On a Windows with the firewall's notifications on, opening it shows the firewall's
//                            "Allow access?" alert (a window): pass it only where that can't happen, or with the
//                            person's OK for that alert
//  The installer proof kit (installer-kit.ps1 beside this file) runs the box in Make's real profile, where the
//  installer's firewall rules apply, and makes and deletes that profile as Make does:
//    --profile Darkride.MakeBlender  the box in Make's profile (default: the test's own Darkride.MakeBoxTest)
//    --keep-profile                  leave the profile for the kit's own delete step
//    make-box-test.exe --profile-create | --profile-delete   Make's profile alone (create also says whether its SID
//                                    is the one the installer's rules name)
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <userenv.h>
#include <wincred.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// the box's AppContainer profile: the test's own throwaway one, or with --profile Darkride.MakeBlender Make's real one
// (the installer proof kit: the installer's firewall rules name that profile's package SID, so only a box in it shows
// whether they hold). No other name is taken.
const wchar_t* TEST_CONTAINER = L"Darkride.MakeBoxTest";
const wchar_t* MAKE_CONTAINER = L"Darkride.MakeBlender";
// the package SID the installer's two rules name (release, 11-07: windowz's derivation of "Darkride.MakeBlender")
const wchar_t* RULE_SID = L"S-1-15-2-579646438-1863717342-1691229924-821914009-3625600486-3308443927-3175962496";
std::wstring boxProfile = TEST_CONTAINER;

bool knownContainer(const std::wstring& name) {
    return name == TEST_CONTAINER || name == MAKE_CONTAINER;
}
const wchar_t* CREDENTIAL = L"Darkride Make box test - delete me";
const DWORD BLENDER_WAIT_MS = 8 * 60 * 1000;

std::string narrow(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), size, nullptr, nullptr);
    return s;
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += (c == '\n' || c == '\r') ? ' ' : c;
    }
    return out + "\"";
}

// the container: made once, its SID; removed at the end
HRESULT createContainer(PSID* sid) {
    const bool make = boxProfile == MAKE_CONTAINER;
    // Make's own names for its profile (scripting to release, 11-07)
    return CreateAppContainerProfile(boxProfile.c_str(), make ? L"Darkride Make (Blender)" : L"Darkride Make box test",
                                     make ? L"Darkride Make's Blender box" : L"Darkride Make box test (delete me)", nullptr,
                                     0, sid);
}

// madeHere (optional): true when this call made the profile, false when it was there already
PSID containerSid(bool* madeHere = nullptr) {
    PSID sid = nullptr;
    HRESULT made = createContainer(&sid);
    if (madeHere) {
        *madeHere = SUCCEEDED(made);
    }
    if (made == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        made = DeriveAppContainerSidFromAppContainerName(boxProfile.c_str(), &sid);
    }
    return SUCCEEDED(made) ? sid : nullptr;
}

// the installer proof kit's steps 2 and 3: Make's profile made, then deleted, as Make does (no admin); the kit runs the
// installer's own rule check after each. Prints the HRESULT, and on create whether the derived SID is the rules' one
int profileStep(bool create) {
    boxProfile = MAKE_CONTAINER;
    if (!create) {
        const HRESULT deleted = DeleteAppContainerProfile(boxProfile.c_str());
        std::cout << "profile deleted: 0x" << std::hex << static_cast<unsigned long>(deleted) << std::dec << "\n";
        return SUCCEEDED(deleted) ? 0 : 3;
    }
    PSID sid = nullptr;
    const HRESULT made = createContainer(&sid);
    std::cout << "profile created: 0x" << std::hex << static_cast<unsigned long>(made) << std::dec << "\n";
    if (made == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        return 1;  // the kit wants it fresh
    }
    if (FAILED(made)) {
        return 3;
    }
    PSID rule = nullptr;
    const bool same = ConvertStringSidToSidW(RULE_SID, &rule) && EqualSid(sid, rule);
    std::cout << "its SID is the installer's rules' SID: " << (same ? "yes" : "NO") << "\n";
    LocalFree(rule);
    FreeSid(sid);
    return same ? 0 : 5;
}

bool grant(const fs::path& path, PSID sid, DWORD access) {
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
    bool ok = SetEntriesInAclW(1, &entry, oldDacl, &newDacl) == ERROR_SUCCESS &&
              SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                    nullptr, newDacl, nullptr) == ERROR_SUCCESS;
    LocalFree(newDacl);
    LocalFree(descriptor);
    return ok;
}

// the job's UI limits: all of them, always (security, 11-07); the box gets a desktop of its own instead
const DWORD UI_FULL = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                      JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                      JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;

// a window station and desktop of the box's own, so the job keeps ALL its UI limits and Blender never shares the
// person's desktop (security, 11-07: Chromium's alternate desktop); opened to the container's SID only
const DWORD DESKTOP_EVERYTHING = DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL |
                                 DESKTOP_JOURNALPLAYBACK | DESKTOP_JOURNALRECORD | DESKTOP_READOBJECTS |
                                 DESKTOP_SWITCHDESKTOP | DESKTOP_WRITEOBJECTS | READ_CONTROL | WRITE_DAC;

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
    bool ok = SetEntriesInAclW(1, &entry, oldDacl, &newDacl) == ERROR_SUCCESS &&
              SetSecurityInfo(object, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, newDacl, nullptr) ==
                  ERROR_SUCCESS;
    LocalFree(newDacl);
    LocalFree(descriptor);
    return ok;
}

struct OwnDesktop {
    HWINSTA station = nullptr;
    HDESK desktop = nullptr;
    std::wstring name;  // "<window station>\\<desktop>", for STARTUPINFO.lpDesktop
    DWORD error = 0;
    OwnDesktop(const OwnDesktop&) = delete;
    OwnDesktop() = default;
    ~OwnDesktop() {
        if (desktop) {
            CloseDesktop(desktop);
        }
        if (station) {
            CloseWindowStation(station);
        }
    }
};

void makeOwnDesktop(OwnDesktop& own, PSID sid) {
    HWINSTA original = GetProcessWindowStation();
    own.station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS | READ_CONTROL | WRITE_DAC, nullptr);
    if (!own.station || !SetProcessWindowStation(own.station)) {
        own.error = GetLastError();
        return;
    }
    own.desktop = CreateDesktopW(L"DarkrideMakeBox", nullptr, nullptr, 0, DESKTOP_EVERYTHING, nullptr);
    own.error = own.desktop ? 0 : GetLastError();
    SetProcessWindowStation(original);
    if (!own.desktop) {
        return;
    }
    wchar_t station[256] {};
    DWORD size = 0;
    GetUserObjectInformationW(own.station, UOI_NAME, station, sizeof station, &size);
    own.name = std::wstring(station) + L"\\DarkrideMakeBox";
    if (!grantObject(own.station, sid, WINSTA_ALL_ACCESS) ||
        !grantObject(own.desktop, sid, DESKTOP_EVERYTHING & ~(READ_CONTROL | WRITE_DAC))) {
        own.error = GetLastError();
        own.name.clear();
    }
}

HANDLE makeJob(DWORD uiLimits) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        return nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                              JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME |
                                              JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = 10LL * 60 * 10000000;  // 10 min, in 100 ns
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(4ULL << 30);
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui {};
    ui.UIRestrictionsClass = uiLimits;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        !SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui)) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

std::wstring envValue(const wchar_t* name) {
    wchar_t value[1024];
    DWORD size = GetEnvironmentVariableW(name, value, 1024);
    return size > 0 && size < 1024 ? std::wstring(value, size) : std::wstring();
}

// Blender's whole environment: the system's few, the standard folder locations, and its own folders in the job
// folder; nothing else of the person's (no key, no other variable)
std::wstring environmentBlock(const fs::path& job) {
    const std::wstring root = envValue(L"SystemRoot");
    std::vector<std::wstring> lines = {
        L"SystemRoot=" + root, L"windir=" + root, L"PATH=" + root + L"\\System32",
        L"NUMBER_OF_PROCESSORS=" + envValue(L"NUMBER_OF_PROCESSORS"),
        L"PROCESSOR_ARCHITECTURE=" + envValue(L"PROCESSOR_ARCHITECTURE"),
        L"TEMP=" + job.wstring(), L"TMP=" + job.wstring(),
        L"BLENDER_USER_CONFIG=" + (job / L"config").wstring(), L"BLENDER_USER_SCRIPTS=" + (job / L"user-scripts").wstring(),
        L"BLENDER_USER_DATAFILES=" + (job / L"datafiles").wstring(),
    };
    // the folder locations an AppContainer start needs to set up the container's own folders (without them
    // CreateProcess fails with ERROR_ENVVAR_NOT_FOUND); locations only, the box can't open them
    for (const wchar_t* name : { L"SystemDrive", L"USERPROFILE", L"LOCALAPPDATA", L"APPDATA", L"ProgramData",
                                 L"ProgramFiles", L"ALLUSERSPROFILE", L"PUBLIC" }) {
        const std::wstring value = envValue(name);
        if (!value.empty()) {
            lines.push_back(std::wstring(name) + L"=" + value);
        }
    }
    std::wstring block;
    for (const auto& line : lines) {
        block += line;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

struct Started {
    PROCESS_INFORMATION process {};
    HANDLE job = nullptr;
    DWORD error = 0;
};

// Blender boxed: an AppContainer token with no capabilities (LPAC: also out of "ALL APPLICATION PACKAGES"), child
// processes restricted, in the job from its first instruction (started suspended, the job given at creation)
Started startBoxed(const fs::path& blender, const fs::path& probe, const fs::path& job, const fs::path& test,
                   const wchar_t* mode, PSID sid, bool lpac, DWORD uiLimits, const std::wstring& desktop,
                   bool container = true, bool useJob = true, bool ownEnvironment = true) {
    // container, useJob and ownEnvironment off: the controls (run 3 found every box form failing at start)
    Started started;
    if (useJob) {
        started.job = makeJob(uiLimits);
        if (!started.job) {
            started.error = GetLastError();
            return started;
        }
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 4, 0, &size);
    std::vector<char> buffer(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    InitializeProcThreadAttributeList(attributes, 4, 0, &size);
    SECURITY_CAPABILITIES capabilities {};
    capabilities.AppContainerSid = sid;
    capabilities.Capabilities = nullptr;
    capabilities.CapabilityCount = 0;
    DWORD optOut = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;
    DWORD childPolicy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    HANDLE jobs[1] = { started.job };
    if (container) {
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities,
                                  sizeof capabilities, nullptr, nullptr);
        if (lpac) {
            UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY, &optOut,
                                      sizeof optOut, nullptr, nullptr);
        }
    }
    if (container || useJob) {
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &childPolicy, sizeof childPolicy,
                                  nullptr, nullptr);
    }
    if (useJob) {
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof jobs, nullptr, nullptr);
    }

    wchar_t profile[MAX_PATH] {};
    DWORD profileSize = MAX_PATH;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetUserProfileDirectoryW(token, profile, &profileSize);
        CloseHandle(token);
    }
    std::wstring command = L"\"" + blender.wstring() + L"\" --background --factory-startup --disable-autoexec -noaudio" +
                           L" --python \"" + probe.wstring() + L"\" -- \"" + job.wstring() + L"\" " + mode + L" \"" +
                           profile + L"\" \"" + test.wstring() + L"\"";
    std::wstring environment = environmentBlock(job);
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof startup;
    startup.lpAttributeList = attributes;
    std::wstring desktopName = desktop;  // empty: the person's own desktop (only the diagnosis steps)
    startup.StartupInfo.lpDesktop = desktopName.empty() ? nullptr : desktopName.data();
    if (!CreateProcessW(blender.c_str(), command.data(), nullptr, nullptr, FALSE,
                        // DETACHED_PROCESS, not CREATE_NO_WINDOW: blender.exe is a console program, and a console means a
                        // conhost.exe child, which the box's child-process policy refuses (run 4: 0xC0000142 at start)
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | DETACHED_PROCESS |
                            (ownEnvironment ? CREATE_UNICODE_ENVIRONMENT : 0),
                        ownEnvironment ? environment.data() : nullptr, job.c_str(), &startup.StartupInfo, &started.process)) {
        started.error = GetLastError();
    } else {
        ResumeThread(started.process.hThread);
    }
    DeleteProcThreadAttributeList(attributes);
    return started;
}

std::vector<DWORD> jobPids(HANDLE job) {
    std::vector<char> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 16 * sizeof(ULONG_PTR));
    auto list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
    std::vector<DWORD> pids;
    if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, static_cast<DWORD>(buffer.size()), nullptr)) {
        for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
            pids.push_back(static_cast<DWORD>(list->ProcessIdList[i]));
        }
    }
    return pids;
}

// the run's options (run 6): each extra network target only with its flag
struct Options {
    std::string macAddress;  // --mac <address>:<port>
    int macPort = 0;
    bool systemPort = false;   // --system-port
    bool lanListener = false;  // --lan-listener
    bool keepProfile = false;  // --keep-profile: the profile stays for the kit's own delete step
};

bool parseOptions(int argc, wchar_t** argv, Options& options) {
    for (int i = 3; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--system-port") {
            options.systemPort = true;
        } else if (arg == L"--lan-listener") {
            options.lanListener = true;
        } else if (arg == L"--keep-profile") {
            options.keepProfile = true;
        } else if (arg == L"--profile" && i + 1 < argc) {
            boxProfile = argv[++i];
            if (!knownContainer(boxProfile)) {
                return false;
            }
        } else if (arg == L"--mac" && i + 1 < argc) {
            const std::string value = narrow(argv[++i]);
            const size_t colon = value.find(':');
            in_addr check {};
            if (colon == std::string::npos || inet_pton(AF_INET, value.substr(0, colon).c_str(), &check) != 1) {
                return false;
            }
            options.macAddress = value.substr(0, colon);
            options.macPort = std::atoi(value.substr(colon + 1).c_str());
            if (options.macPort <= 0 || options.macPort > 65535) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

// a listener of this program's own: every connection it takes gets the run's token and is closed; it counts them
struct Listener {
    SOCKET socket = INVALID_SOCKET;
    std::atomic<int> accepted { 0 };
};

// the network targets: the program's own loopback listener, a closed port on this laptop's own LAN address, and with
// the flags the Mac's listener, Windows' own port 135 on the LAN address, and a listener of ours on the LAN address
struct Targets {
    Listener loopback;
    Listener lan;  // --lan-listener only
    std::string token;
    std::atomic<bool> stop { false };
    std::thread acceptor;
    std::string json;
};

std::string randomToken() {
    BYTE bytes[16];
    BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* digits = "0123456789abcdef";
    std::string token = "darkride-run6-";
    for (BYTE b : bytes) {
        token += digits[b >> 4];
        token += digits[b & 15];
    }
    return token;
}

void acceptLoop(Targets* targets) {
    while (!targets->stop) {
        fd_set ready;
        FD_ZERO(&ready);
        for (Listener* listener : { &targets->loopback, &targets->lan }) {
            if (listener->socket != INVALID_SOCKET) {
                FD_SET(listener->socket, &ready);
            }
        }
        timeval wait { 0, 200 * 1000 };
        const int count = select(0, &ready, nullptr, nullptr, &wait);
        if (count < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));  // no listener open: don't spin
        }
        if (count <= 0) {
            continue;
        }
        for (Listener* listener : { &targets->loopback, &targets->lan }) {
            if (listener->socket == INVALID_SOCKET || !FD_ISSET(listener->socket, &ready)) {
                continue;
            }
            SOCKET client = accept(listener->socket, nullptr, nullptr);
            if (client != INVALID_SOCKET) {
                ++listener->accepted;
                const std::string line = targets->token + "\n";
                send(client, line.data(), static_cast<int>(line.size()), 0);
                shutdown(client, SD_BOTH);
                closesocket(client);
            }
        }
    }
}

std::string addressJson(const char* address, int port) {
    return std::string("[\"") + address + "\", " + std::to_string(port) + "]";
}

void openTargets(Targets& targets, const Options& options) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    targets.token = randomToken();
    sockaddr_in address {};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    targets.loopback.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    bind(targets.loopback.socket, reinterpret_cast<sockaddr*>(&address), sizeof address);
    listen(targets.loopback.socket, 4);
    int length = sizeof address;
    getsockname(targets.loopback.socket, reinterpret_cast<sockaddr*>(&address), &length);
    const int loopbackPort = ntohs(address.sin_port);
    // the LAN address: a UDP socket "connected" to a documentation address sends nothing, and names the local address
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in documentation {};
    documentation.sin_family = AF_INET;
    documentation.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &documentation.sin_addr);
    connect(probe, reinterpret_cast<sockaddr*>(&documentation), sizeof documentation);
    sockaddr_in lan {};
    length = sizeof lan;
    getsockname(probe, reinterpret_cast<sockaddr*>(&lan), &length);
    closesocket(probe);
    // a port free just now on that address: bound and closed again, never listened on
    SOCKET spare = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    lan.sin_port = 0;
    bind(spare, reinterpret_cast<sockaddr*>(&lan), sizeof lan);
    length = sizeof lan;
    getsockname(spare, reinterpret_cast<sockaddr*>(&lan), &length);
    closesocket(spare);
    const int closedPort = ntohs(lan.sin_port);
    char lanText[INET_ADDRSTRLEN] {};
    inet_ntop(AF_INET, &lan.sin_addr, lanText, sizeof lanText);
    // a listener of ours on the LAN address: only with --lan-listener (the firewall's alert, see the top)
    int lanListenerPort = 0;
    if (options.lanListener) {
        sockaddr_in listenOn = lan;
        listenOn.sin_port = 0;
        targets.lan.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (bind(targets.lan.socket, reinterpret_cast<sockaddr*>(&listenOn), sizeof listenOn) == 0 &&
            listen(targets.lan.socket, 4) == 0) {
            length = sizeof listenOn;
            getsockname(targets.lan.socket, reinterpret_cast<sockaddr*>(&listenOn), &length);
            lanListenerPort = ntohs(listenOn.sin_port);
        } else {
            closesocket(targets.lan.socket);
            targets.lan.socket = INVALID_SOCKET;
        }
    }
    targets.acceptor = std::thread(acceptLoop, &targets);
    std::ostringstream json;
    json << "{\"token\": \"" << targets.token << "\", \"loopback\": " << addressJson("127.0.0.1", loopbackPort)
         << ", \"lan_closed\": " << addressJson(lanText, closedPort)
         << ", \"lan_listener\": " << (lanListenerPort ? addressJson(lanText, lanListenerPort) : "null")
         << ", \"system_port\": " << (options.systemPort ? addressJson(lanText, 135) : "null")
         << ", \"mac\": " << (options.macPort ? addressJson(options.macAddress.c_str(), options.macPort) : "null") << "}";
    targets.json = json.str();
}

void closeTargets(Targets& targets) {
    targets.stop = true;
    if (targets.acceptor.joinable()) {
        targets.acceptor.join();
    }
    for (Listener* listener : { &targets.loopback, &targets.lan }) {
        if (listener->socket != INVALID_SOCKET) {
            closesocket(listener->socket);
            listener->socket = INVALID_SOCKET;
        }
    }
}

std::string acceptedJson(const Targets& targets) {
    return "{\"loopback\": " + std::to_string(targets.loopback.accepted.load()) +
           ", \"lan_listener\": " + std::to_string(targets.lan.accepted.load()) + "}";
}

std::string readFile(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "null";
    }
    std::stringstream read;
    read << file.rdbuf();
    return read.str();
}

// the throwaway credential the box must not read: a random value, a name marked as a test, this logon only
struct TestCredential {
    bool written = false;
    TestCredential() {
        BYTE secret[16];
        BCryptGenRandom(nullptr, secret, sizeof secret, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        CREDENTIALW credential {};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<LPWSTR>(CREDENTIAL);
        credential.Comment = const_cast<LPWSTR>(L"Darkride Make box test, delete me");
        credential.CredentialBlobSize = sizeof secret;
        credential.CredentialBlob = secret;
        credential.Persist = CRED_PERSIST_SESSION;
        written = CredWriteW(&credential, 0) == TRUE;
        SecureZeroMemory(secret, sizeof secret);
    }
    ~TestCredential() { CredDeleteW(CREDENTIAL, CRED_TYPE_GENERIC, 0); }
};

bool waitForFile(const fs::path& path, int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (fs::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// the kill case's parent: a boxed Blender that loops, its job's pids written, then it waits to be ended
int runParent(const fs::path& blender, const fs::path& probe, const fs::path& job, const fs::path& test, bool lpac,
              DWORD uiLimits, bool ownDesktop) {
    PSID sid = containerSid();
    OwnDesktop own;
    if (ownDesktop) {
        makeOwnDesktop(own, sid);
    }
    Started started = startBoxed(blender, probe, job, test, L"runaway", sid, lpac, uiLimits, own.name);
    if (started.error || !waitForFile(job / L"runaway-started.txt", 120)) {
        return 2;
    }
    std::ofstream pids(job / L"job-pids.txt");
    for (DWORD pid : jobPids(started.job)) {
        pids << pid << "\n";
    }
    pids.close();
    Sleep(INFINITE);  // ended by the test; the job's last handle closes with this process
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const fs::path self = argv[0];
    if (argc == 10 && std::wstring(argv[1]) == L"--parent") {
        boxProfile = argv[9];
        if (!knownContainer(boxProfile)) {
            return 2;
        }
        return runParent(argv[2], argv[3], argv[4], argv[5], std::wstring(argv[6]) == L"lpac",
                         static_cast<DWORD>(std::wcstoul(argv[7], nullptr, 10)), std::wstring(argv[8]) == L"own");
    }
    // run as the normal user only: an elevated run would test a different box (and Make never runs elevated)
    HANDLE self_token = nullptr;
    TOKEN_ELEVATION elevation {};
    DWORD elevationSize = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &self_token)) {
        GetTokenInformation(self_token, TokenElevation, &elevation, sizeof elevation, &elevationSize);
        CloseHandle(self_token);
    }
    if (elevation.TokenIsElevated) {
        std::cerr << "run this as the normal user, not elevated\n";
        return 4;
    }
    if (argc == 2 && (std::wstring(argv[1]) == L"--profile-create" || std::wstring(argv[1]) == L"--profile-delete")) {
        return profileStep(std::wstring(argv[1]) == L"--profile-create");
    }
    Options options;
    if (argc < 3 || !parseOptions(argc, argv, options)) {
        std::wcerr << L"usage: make-box-test.exe <blender.exe> <test folder> [--mac <address>:<port>] [--system-port]"
                      L" [--lan-listener] [--profile Darkride.MakeBoxTest|Darkride.MakeBlender] [--keep-profile]\n"
                      L"       make-box-test.exe --profile-create | --profile-delete   (Make's profile, the installer kit)\n";
        return 2;
    }
    const fs::path blender = fs::absolute(argv[1]);
    const fs::path test = fs::absolute(argv[2]);
    const fs::path job = test / L"job";
    const fs::path scripts = test / L"scripts";
    std::error_code ignored;
    fs::remove_all(job, ignored);
    fs::create_directories(job / L"config");
    fs::create_directories(scripts);
    fs::copy_file(fs::absolute(self).parent_path() / L"probe.py", scripts / L"probe.py", fs::copy_options::overwrite_existing);
    // a key-shaped variable in this program's own environment: Blender must not see it (its block is built afresh)
    SetEnvironmentVariableW(L"ANTHROPIC_API_KEY", L"kit-test-env-key");

    bool profileMadeHere = false;
    PSID sid = containerSid(&profileMadeHere);
    if (!sid) {
        std::cerr << "no AppContainer profile could be made\n";
        return 3;
    }
    bool granted = grant(job, sid, FILE_ALL_ACCESS) && grant(scripts, sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
    // a Blender from its installer sits in Program Files, which the box can read; one elsewhere (the portable zip)
    // gets read and run on its own folder only
    const std::wstring programFiles = envValue(L"ProgramFiles");
    if (programFiles.empty() || blender.wstring().rfind(programFiles, 0) != 0) {
        granted = grant(blender.parent_path(), sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE) && granted;
    }
    Targets targets;
    openTargets(targets, options);
    std::ofstream(job / L"targets.json") << targets.json;
    std::cout << "targets: mac " << (options.macPort ? "yes" : "no") << ", system port " << (options.systemPort ? "yes" : "no")
              << ", lan listener " << (options.lanListener ? (targets.lan.socket != INVALID_SOCKET ? "open" : "not opened") : "no")
              << "\n";
    std::string form;
    bool lpacChosen = false;
    DWORD uiChosen = 0;
    bool ownDesktopChosen = false;
    DWORD exitCode = 0;
    std::string results = "null";
    std::string control = "null";
    DWORD controlExit = 0;
    std::string acceptedAfterControl = "null";
    std::string acceptedAfterProbe = "null";
    std::ostringstream starts;
    {
        TestCredential credential;

        // the UNBOXED control of every network check (run 6): the same Blender and probe, the box's environment, no
        // container, no job, so each target is shown reachable from this laptop and a refusal in the box means
        // something
        fs::remove(job / L"net-control.json", ignored);
        Started unboxed = startBoxed(blender, scripts / L"probe.py", job, test, L"net-control", sid, false, 0, std::wstring(),
                                     false, false, true);
        controlExit = unboxed.error;
        if (!unboxed.error) {
            WaitForSingleObject(unboxed.process.hProcess, 120000);
            GetExitCodeProcess(unboxed.process.hProcess, &controlExit);
            CloseHandle(unboxed.process.hThread);
            CloseHandle(unboxed.process.hProcess);
        }
        control = readFile(job / L"net-control.json");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        acceptedAfterControl = acceptedJson(targets);
        std::cout << "unboxed network control: " << (control == "null" ? "no results" : "done") << " (" << controlExit << ")\n";

        OwnDesktop own;
        makeOwnDesktop(own, sid);
        std::cout << "own desktop: " << (own.name.empty() ? "not made, error " + std::to_string(own.error) : "made") << "\n";
        // controls take one piece away at a time; only box steps (control false) are kept for the probe
        struct Step { bool container; bool lpac; bool job; DWORD ui; bool ownDesktop; bool ownEnv; bool control; const char* name; };
        const Step ladder[] = {
            { false, false, false, 0, false, false, true, "control: Blender unboxed, its usual environment" },
            { false, false, false, 0, false, true, true, "control: Blender unboxed, the box's environment" },
            { false, false, true, UI_FULL, false, true, true, "control: the job only, all UI limits" },
            { true, false, false, 0, false, true, true, "control: the plain AppContainer only, no job" },
            { true, true, false, 0, false, true, true, "control: the LPAC AppContainer only, no job" },
            { true, false, false, 0, true, true, true, "control: the plain AppContainer only, own desktop" },
            { true, true, true, UI_FULL, true, true, false, "lpac, own desktop, all UI limits" },
            { true, false, true, UI_FULL, true, true, false, "plain, own desktop, all UI limits" },
            { true, true, true, UI_FULL, false, true, false, "lpac, all UI limits" },
            { true, false, true, UI_FULL, false, true, false, "plain, all UI limits" } };
        bool ownChosen = false;
        bool first = true;
        for (const Step& step : ladder) {
            if (step.ownDesktop && own.name.empty()) {
                starts << (first ? "" : ", ") << "{\"step\": " << jsonString(step.name) << ", \"started\": false, \"desktop_error\": "
                       << own.error << "}";
                first = false;
                continue;
            }
            fs::remove(job / L"started.txt", ignored);
            Started started = startBoxed(blender, scripts / L"probe.py", job, test, L"start", sid, step.lpac, step.ui,
                                         step.ownDesktop ? own.name : std::wstring(), step.container, step.job, step.ownEnv);
            DWORD code = started.error;
            if (!started.error) {
                WaitForSingleObject(started.process.hProcess, 120000);
                GetExitCodeProcess(started.process.hProcess, &code);
                CloseHandle(started.process.hThread);
                CloseHandle(started.process.hProcess);
            }
            CloseHandle(started.job);
            const bool ran = !started.error && fs::exists(job / L"started.txt");
            starts << (first ? "" : ", ") << "{\"step\": " << jsonString(step.name) << ", \"started\": " << (ran ? "true" : "false")
                   << ", \"" << (started.error ? "start_error" : "exit") << "\": " << code << "}";
            first = false;
            std::cout << step.name << ": " << (ran ? "started" : "didn't start") << " (" << code << ")\n";
            if (ran && form.empty() && !step.control) {
                form = step.name;
                lpacChosen = step.lpac;
                uiChosen = step.ui;
                ownChosen = step.ownDesktop;
                ownDesktopChosen = step.ownDesktop;
            }
        }
        if (!form.empty()) {
            fs::remove(job / L"probe-results.json", ignored);
            Started started = startBoxed(blender, scripts / L"probe.py", job, test, L"probe", sid, lpacChosen, uiChosen,
                                         ownChosen ? own.name : std::wstring());
            if (!started.error) {
                WaitForSingleObject(started.process.hProcess, BLENDER_WAIT_MS);
                GetExitCodeProcess(started.process.hProcess, &exitCode);
                CloseHandle(started.process.hThread);
                CloseHandle(started.process.hProcess);
            }
            CloseHandle(started.job);  // kill-on-close: nothing of it outlives this
            std::cout << "probe under \"" << form << "\": Blender exit " << exitCode << "\n";
            results = readFile(job / L"probe-results.json");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            acceptedAfterProbe = acceptedJson(targets);  // counts since the start: the box's own are these minus the control's
        }
    }  // the test credential is deleted here
    closeTargets(targets);

    // kill the parent: Blender, looping in the parent's job, must end with it
    std::string killed = "\"not run\"";
    if (!form.empty()) {
        fs::remove(job / L"runaway-started.txt", ignored);
        fs::remove(job / L"job-pids.txt", ignored);
        std::wstring command = L"\"" + fs::absolute(self).wstring() + L"\" --parent \"" + blender.wstring() + L"\" \"" +
                               (scripts / L"probe.py").wstring() + L"\" \"" + job.wstring() + L"\" \"" + test.wstring() +
                               L"\" " + (lpacChosen ? L"lpac" : L"plain") + L" " + std::to_wstring(uiChosen) + L" " +
                               (ownDesktopChosen ? L"own" : L"shared") + L" " + boxProfile;
        STARTUPINFOW startup { sizeof startup };
        PROCESS_INFORMATION parent {};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &parent) &&
            waitForFile(job / L"job-pids.txt", 150)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::vector<HANDLE> boxed;
            std::ifstream pids(job / L"job-pids.txt");
            DWORD pid = 0;
            while (!(pids >> pid).fail()) {
                if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                    boxed.push_back(h);
                }
            }
            TerminateProcess(parent.hProcess, 1);
            bool allEnded = !boxed.empty();
            for (HANDLE h : boxed) {
                allEnded = allEnded && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0;
                CloseHandle(h);
            }
            killed = std::string(allEnded ? "true" : "false") + ", \"boxed_pids\": " + std::to_string(boxed.size());
            CloseHandle(parent.hThread);
            CloseHandle(parent.hProcess);
        }
    }

    std::ofstream out(test / L"results.json");
    out << "{\"form\": " << jsonString(form.empty() ? "none started" : form) << ", \"granted\": " << (granted ? "true" : "false")
        << ", \"profile\": " << jsonString(narrow(boxProfile)) << ", \"profile_made_here\": " << (profileMadeHere ? "true" : "false")
        << ", \"profile_kept\": " << (options.keepProfile ? "true" : "false")
        << ", \"targets\": {\"mac\": " << (options.macPort ? "true" : "false") << ", \"system_port\": "
        << (options.systemPort ? "true" : "false") << ", \"lan_listener\": " << (options.lanListener ? "true" : "false")
        << "}, \"control_exit\": " << controlExit << ", \"control\": " << control
        << ", \"accepted_after_control\": " << acceptedAfterControl << ", \"accepted_after_probe\": " << acceptedAfterProbe
        << ", \"starts\": [" << starts.str() << "], \"blender_exit\": " << exitCode << ", \"probe\": " << results << ", \"kill_the_parent_ends_blender\": " << killed
        << "}\n";
    out.close();
    std::cout << "results: " << narrow((test / L"results.json").wstring()) << "\n";

    fs::remove_all(job, ignored);
    fs::remove(test / L"written-from-the-box.txt", ignored);
    // Make's real profile is deleted only when this run made it: one Make made earlier (and its folder) stays
    // (security, 11-07); the test's own throwaway profile always goes
    if (!options.keepProfile && (profileMadeHere || boxProfile == TEST_CONTAINER)) {
        DeleteAppContainerProfile(boxProfile.c_str());
    }
    FreeSid(sid);
    WSACleanup();
    return form.empty() ? 1 : 0;
}
