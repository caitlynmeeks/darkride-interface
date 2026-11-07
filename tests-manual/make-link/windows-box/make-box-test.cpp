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
//    make-box-test.exe <blender.exe> <test folder>
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

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

const wchar_t* CONTAINER = L"Darkride.MakeBoxTest";
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
PSID containerSid() {
    PSID sid = nullptr;
    HRESULT made = CreateAppContainerProfile(CONTAINER, L"Darkride Make box test", L"Darkride Make box test (delete me)",
                                             nullptr, 0, &sid);
    if (made == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        made = DeriveAppContainerSidFromAppContainerName(CONTAINER, &sid);
    }
    return SUCCEEDED(made) ? sid : nullptr;
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

HANDLE makeJob() {
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
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                             JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                             JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
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
                   const wchar_t* mode, PSID sid, bool lpac) {
    Started started;
    started.job = makeJob();
    if (!started.job) {
        started.error = GetLastError();
        return started;
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
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities, sizeof capabilities,
                              nullptr, nullptr);
    if (lpac) {
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY, &optOut, sizeof optOut,
                                  nullptr, nullptr);
    }
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &childPolicy, sizeof childPolicy,
                              nullptr, nullptr);
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof jobs, nullptr, nullptr);

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
    if (!CreateProcessW(blender.c_str(), command.data(), nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                        environment.data(), job.c_str(), &startup.StartupInfo, &started.process)) {
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

// the network targets: the program's own loopback listener, and a closed port on this laptop's own LAN address
struct Targets {
    SOCKET loopback = INVALID_SOCKET;
    std::string json;
};

Targets openTargets() {
    Targets targets;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    targets.loopback = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    bind(targets.loopback, reinterpret_cast<sockaddr*>(&address), sizeof address);
    listen(targets.loopback, 4);
    int length = sizeof address;
    getsockname(targets.loopback, reinterpret_cast<sockaddr*>(&address), &length);
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
    char lanText[INET_ADDRSTRLEN] {};
    inet_ntop(AF_INET, &lan.sin_addr, lanText, sizeof lanText);
    std::ostringstream json;
    json << "{\"loopback\": [\"127.0.0.1\", " << loopbackPort << "], \"lan\": [\"" << lanText << "\", " << ntohs(lan.sin_port)
         << "]}";
    targets.json = json.str();
    return targets;
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
int runParent(const fs::path& blender, const fs::path& probe, const fs::path& job, const fs::path& test, bool lpac) {
    PSID sid = containerSid();
    Started started = startBoxed(blender, probe, job, test, L"runaway", sid, lpac);
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
    if (argc == 6 && std::wstring(argv[1]) == L"--parent") {
        return runParent(argv[2], argv[3], argv[4], argv[5], false);
    }
    if (argc == 7 && std::wstring(argv[1]) == L"--parent") {
        return runParent(argv[2], argv[3], argv[4], argv[5], std::wstring(argv[6]) == L"lpac");
    }
    if (argc != 3) {
        std::wcerr << L"usage: make-box-test.exe <blender.exe> <test folder>\n";
        return 2;
    }
    // run as the normal user only: an elevated run would test a different box
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

    PSID sid = containerSid();
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
    Targets targets = openTargets();
    std::ofstream(job / L"targets.json") << targets.json;
    std::string form;
    DWORD exitCode = 0;
    std::string results = "null";
    {
        TestCredential credential;
        for (bool lpac : { true, false }) {
            fs::remove(job / L"probe-results.json", ignored);
            Started started = startBoxed(blender, scripts / L"probe.py", job, test, L"probe", sid, lpac);
            if (started.error) {
                std::cout << (lpac ? "lpac" : "plain") << ": Blender didn't start, error " << started.error << "\n";
                CloseHandle(started.job);
                continue;
            }
            WaitForSingleObject(started.process.hProcess, BLENDER_WAIT_MS);
            GetExitCodeProcess(started.process.hProcess, &exitCode);
            CloseHandle(started.process.hThread);
            CloseHandle(started.process.hProcess);
            CloseHandle(started.job);  // kill-on-close: nothing of it outlives this
            std::cout << (lpac ? "lpac" : "plain") << ": Blender exit " << exitCode << "\n";
            std::ifstream file(job / L"probe-results.json");
            if (file.is_open()) {
                std::stringstream read;
                read << file.rdbuf();
                results = read.str();
                form = lpac ? "lpac" : "plain";
                break;
            }
        }
    }  // the test credential is deleted here
    closesocket(targets.loopback);

    // kill the parent: Blender, looping in the parent's job, must end with it
    std::string killed = "\"not run\"";
    if (!form.empty()) {
        fs::remove(job / L"runaway-started.txt", ignored);
        fs::remove(job / L"job-pids.txt", ignored);
        std::wstring command = L"\"" + fs::absolute(self).wstring() + L"\" --parent \"" + blender.wstring() + L"\" \"" +
                               (scripts / L"probe.py").wstring() + L"\" \"" + job.wstring() + L"\" \"" + test.wstring() +
                               L"\" " + (form == "lpac" ? L"lpac" : L"plain");
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
        << ", \"blender_exit\": " << exitCode << ", \"probe\": " << results << ", \"kill_the_parent_ends_blender\": " << killed
        << "}\n";
    out.close();
    std::cout << "results: " << narrow((test / L"results.json").wstring()) << "\n";

    fs::remove_all(job, ignored);
    fs::remove(test / L"written-from-the-box.txt", ignored);
    DeleteAppContainerProfile(CONTAINER);
    FreeSid(sid);
    WSACleanup();
    return form.empty() ? 1 : 0;
}
