//
//  make-box-test.cpp
//  tests-manual/make-link/linux-box
//
//  Make v1 on Linux (6c), the first question (security's plan, team review make-blender-box-windows-linux "Linux",
//  and its items (a)-(e) of 11-07): can Blender run, and render, inside the box Make will use on Linux, and does the
//  box refuse what it must? A stand-alone test (no Qt, no Darkride, no Claude, no key, no outside service), run as the
//  normal user (never root), headless, over SSH on a rented test machine. No user namespaces (Ubuntu 24.04 restricts
//  them): the box is built from what an unprivileged program may set on itself, in the child, before Blender starts:
//    1. its own session (setsid: no terminal), killed when this program dies (PR_SET_PDEATHSIG), standard input
//       /dev/null and output to a file in the job folder, no other open file;
//    2. limits that outlive this program: 10 min of CPU, 4 GB of data memory, 16 GB of address space, no core file;
//    3. no_new_privs, then Landlock: read /usr, /lib*, a short /etc list, a few /sys and /dev entries, the scripts
//       folder; read and run Blender's folder and its ELF loader (the kernel opens the loader to run Blender); read
//       and write the job folder; nothing else; on ABI 4 and later, no TCP at all; on ABI 6 and later, no abstract
//       Unix sockets and no signals out. /proc: none, or only the box's own entry (security, item d);
//    4. seccomp: socket() and socketpair() refused for every family (item a), fork and every clone but a thread's
//       refused (clone3 answers ENOSYS, so the C library falls back to clone), signals only to the box's own process
//       (item c), TIOCSTI refused (item e), io_uring, ptrace, namespaces, mounts, the kernel's key store and the like
//       refused; and the EXEC GATE: execve and execveat go to this program as user notifications; the launcher's own
//       start of Blender is let through, every later one is answered EPERM (with this program gone: ENOSYS), so
//       nothing but Blender runs in the box, not even through the loader. The decision is a count and reads no
//       memory of the box, so there is nothing to race (security, 11-07);
//    5. an environment built from nothing: the job folder as home, temp and Blender's own folders; no display, no
//       session bus, no runtime folder, no key (item b).
//  Then, as on Windows (runs 5 and 6):
//    - the UNBOXED control: the same Blender and probe with the box's environment and no box, so each target is shown
//      there and the box's refusal of it means something;
//    - the start ladder: one piece of the box at a time, then the whole box, strictest first; the first whole box
//      that renders is the box the probe runs in;
//    - the probe in the box (probe.py: files, network, programs, signals, the terminal, memory, the environment, four
//      views with EEVEE and Cycles, then the exec gate's two checks);
//    - the CPU cap: a looping Blender under a 5 s cap must end by itself;
//    - kill the parent: a parent copy of this program starts a boxed Blender that loops; this program kills the parent
//      (SIGKILL); Blender must end with it (its own pid and start time, never a name);
//    - information only: what user namespaces do on this machine, unboxed (release, 11-07), from a single-threaded
//      child of this program (the kernel refuses them to a threaded process such as Blender).
//  Writes <test folder>/results.json and summary.txt, prints the summary, and deletes the job folder and everything
//  the control wrote. Exit 0 only when the whole summary passes.
//    g++ -std=c++17 -O2 -Wall -Wextra -pthread -o make-box-test make-box-test.cpp
//    ./make-box-test <Blender's "blender" program> <test folder> [--keep-logs]
//  --keep-logs keeps each Blender run's own output in <test folder>/logs (for a start that fails; the logs can hold
//  paths, so only on a test machine).
//  Exit codes of a box start that fails before Blender (in the logs and results): 121 session or parent-death signal,
//  122 standard files or folder, 123 limits, 124 no_new_privs, 125 Landlock, 126 seccomp, 127 exec.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#if !defined(__linux__) || !defined(__x86_64__)
#error "the Linux box test is for x86_64 Linux (Blender's Linux build is x86_64 only)"
#endif

#include <arpa/inet.h>
#include <elf.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;

namespace {

// Landlock's interface, written out here so the test builds with older kernel headers (22.04's linux-libc-dev is 5.15)
namespace landlock {
constexpr long CREATE_RULESET = 444;  // the same number on every architecture
constexpr long ADD_RULE = 445;
constexpr long RESTRICT_SELF = 446;
constexpr uint32_t CREATE_RULESET_VERSION = 1U << 0;
constexpr int RULE_PATH_BENEATH = 1;
constexpr uint64_t EXECUTE = 1ULL << 0, WRITE_FILE = 1ULL << 1, READ_FILE = 1ULL << 2, READ_DIR = 1ULL << 3,
                   REMOVE_DIR = 1ULL << 4, REMOVE_FILE = 1ULL << 5, MAKE_DIR = 1ULL << 7, MAKE_REG = 1ULL << 8,
                   REFER = 1ULL << 13, TRUNCATE = 1ULL << 14, IOCTL_DEV = 1ULL << 15;
constexpr uint64_t NET_BIND_TCP = 1ULL << 0, NET_CONNECT_TCP = 1ULL << 1;
constexpr uint64_t SCOPE_ABSTRACT_UNIX_SOCKET = 1ULL << 0, SCOPE_SIGNAL = 1ULL << 1;
// the rights a rule on a file (not a folder) may hold
constexpr uint64_t FILE_RIGHTS = EXECUTE | WRITE_FILE | READ_FILE | TRUNCATE | IOCTL_DEV;
struct RulesetAttr {
    uint64_t fs = 0;
    uint64_t net = 0;     // ABI 4 and later
    uint64_t scoped = 0;  // ABI 6 and later
};
struct PathBeneath {
    uint64_t allowed;
    int32_t parentFd;
} __attribute__((packed));

int abi() {
    const long version = syscall(CREATE_RULESET, nullptr, 0, CREATE_RULESET_VERSION);
    return version < 0 ? 0 : static_cast<int>(version);
}

// every file right the kernel knows: all of them handled, so anything not granted is refused
uint64_t fsRights(int abi) {
    uint64_t rights = (1ULL << 13) - 1;  // ABI 1: the first 13
    if (abi >= 2) {
        rights |= REFER;
    }
    if (abi >= 3) {
        rights |= TRUNCATE;
    }
    if (abi >= 5) {
        rights |= IOCTL_DEV;
    }
    return rights;
}
}  // namespace landlock

// system calls newer than some headers, x86_64 numbers
constexpr long NR_PIDFD_SEND_SIGNAL = 424, NR_IO_URING_SETUP = 425, NR_IO_URING_ENTER = 426,
               NR_IO_URING_REGISTER = 427, NR_OPEN_TREE = 428, NR_MOVE_MOUNT = 429, NR_FSOPEN = 430, NR_FSCONFIG = 431,
               NR_FSMOUNT = 432, NR_FSPICK = 433, NR_CLONE3 = 435, NR_CLOSE_RANGE = 436, NR_PIDFD_GETFD = 438,
               NR_PROCESS_MADVISE = 440, NR_MOUNT_SETATTR = 442, NR_QUOTACTL_FD = 443;

const char* TEST_KEY = "kit-test-env-key";  // a key-shaped value in this program's environment: Blender must not see it
const int PROBE_WAIT_MS = 8 * 60 * 1000;
const int START_WAIT_MS = 3 * 60 * 1000;
const int CPU_CAP_TEST_S = 5;

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string randomHex(int bytes) {
    std::vector<unsigned char> buffer(bytes);
    if (getrandom(buffer.data(), buffer.size(), 0) != static_cast<ssize_t>(buffer.size())) {
        std::cerr << "no random bytes\n";
        std::exit(3);
    }
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char b : buffer) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::string readFile(const std::string& path, const std::string& missing = "null") {
    std::ifstream file(path);
    if (!file.is_open()) {
        return missing;
    }
    std::stringstream read;
    read << file.rdbuf();
    return read.str();
}

std::string firstLine(const std::string& path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
}

bool waitForFile(const std::string& path, int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (fs::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// the loader Blender's program names (PT_INTERP): the kernel opens it to run Blender, so the box must let it run
std::string elfInterpreter(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    Elf64_Ehdr header {};
    file.read(reinterpret_cast<char*>(&header), sizeof header);
    if (!file || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_machine != EM_X86_64) {
        return {};
    }
    for (int i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr program {};
        file.seekg(static_cast<std::streamoff>(header.e_phoff + static_cast<uint64_t>(i) * header.e_phentsize));
        file.read(reinterpret_cast<char*>(&program), sizeof program);
        if (!file) {
            return {};
        }
        if (program.p_type == PT_INTERP && program.p_filesz > 1 && program.p_filesz < 256) {
            std::string interpreter(program.p_filesz, '\0');
            file.seekg(static_cast<std::streamoff>(program.p_offset));
            file.read(&interpreter[0], static_cast<std::streamsize>(program.p_filesz));
            interpreter.resize(std::strlen(interpreter.c_str()));
            return interpreter;
        }
    }
    return {};
}

struct Paths {
    std::string self, blender, blenderDir, loader, test, job, scripts, outside, logs;
};

// fresh: a new job folder, scripts copy and decoy folder (the test); not fresh: the kill case's parent copy reuses them
bool makePaths(const std::string& blenderArg, const std::string& testArg, bool fresh, Paths& p, std::string& error) {
    std::error_code ec;
    p.self = fs::read_symlink("/proc/self/exe", ec).string();
    p.blender = fs::canonical(blenderArg, ec).string();
    if (ec || !fs::is_regular_file(p.blender) || access(p.blender.c_str(), X_OK) != 0) {
        error = "Blender's program wasn't found (give the \"blender\" file in Blender's folder)";
        return false;
    }
    p.blenderDir = fs::path(p.blender).parent_path().string();
    p.loader = elfInterpreter(p.blender);
    if (p.loader.empty() || p.loader[0] != '/') {
        error = "Blender's program isn't a 64-bit x86 ELF program with a loader";
        return false;
    }
    p.loader = fs::canonical(p.loader, ec).string();
    if (ec) {
        error = "Blender's loader wasn't found";
        return false;
    }
    fs::create_directories(testArg, ec);
    p.test = fs::canonical(testArg, ec).string();
    if (ec) {
        error = "the test folder can't be made";
        return false;
    }
    p.job = p.test + "/job";
    p.scripts = p.test + "/scripts";
    p.outside = p.test + "/outside";
    p.logs = p.test + "/logs";
    if (!fresh) {
        return fs::is_directory(p.job);
    }
    fs::remove_all(p.job, ec);
    fs::remove_all(p.outside, ec);
    for (const char* sub : { "tmp", "config/blender", "cache", "data", "user-scripts", "datafiles", "extensions" }) {
        fs::create_directories(p.job + "/" + sub);
    }
    fs::create_directories(p.scripts);
    fs::create_directories(p.outside);
    fs::permissions(p.test, fs::perms::owner_all, fs::perm_options::replace);
    fs::permissions(p.job, fs::perms::owner_all, fs::perm_options::replace);
    fs::copy_file(fs::path(p.self).parent_path() / "probe.py", p.scripts + "/probe.py", fs::copy_options::overwrite_existing,
                  ec);
    if (ec) {
        error = "probe.py isn't beside this program";
        return false;
    }
    std::ofstream(p.outside + "/decoy.txt") << randomHex(16) << "\n";
    return true;
}

// what's written outside the job folder by the unboxed control (and must stay unwritten by the box): removed again
void removeOutsideWrites(const Paths& p, const std::string& tmpName) {
    std::error_code ec;
    fs::remove(p.outside + "/written-from-the-box.txt", ec);
    fs::remove_all(p.outside + "/made-by-the-box", ec);
    fs::remove("/tmp/" + tmpName + ".txt", ec);
    fs::remove("/dev/shm/" + tmpName, ec);
    fs::remove(p.blenderDir + "/written-from-the-box.txt", ec);
    fs::remove(p.scripts + "/written-from-the-box.txt", ec);
}

// the pieces of the box; the steps take them one at a time
enum class Proc { NONE, OWN };
struct Box {
    bool limits = false;    // own session, parent-death kill, rlimits
    bool landlock = false;
    bool seccomp = false;   // with the exec gate
    Proc proc = Proc::NONE; // under Landlock: no /proc, or the box's own entry (+ cpuinfo, meminfo, stat)
    bool ownEnv = true;     // the box's environment, built from nothing
    int cpuSeconds = 600;
};

Box makeBox(bool limits, bool landlock, bool seccomp, Proc proc, bool ownEnv) {
    Box box;
    box.limits = limits;
    box.landlock = landlock;
    box.seccomp = seccomp;
    box.proc = proc;
    box.ownEnv = ownEnv;
    return box;
}

struct Rule {
    int fd;
    uint64_t access;
};

// everything the child needs, made before fork: after fork the child only makes system calls (this program has
// threads, so the child may not allocate)
struct Prepared {
    Box box;
    std::vector<std::string> argStrings, envStrings;
    std::vector<char*> argv, envp;
    std::string blender, job;
    std::vector<Rule> rules;
    landlock::RulesetAttr ruleset;
    size_t rulesetSize = 0;
    uint64_t procOwnAccess = 0;  // the rule on /proc/self, opened in the child (its pid is known only there)
    std::vector<sock_filter> filter;
    std::vector<size_t> selfPidSlots;  // the filter's "this process" comparisons, set in the child
    int devNull = -1, log = -1, gateParent = -1, gateChild = -1;
    pid_t parent = 0;
    rlimit cpu {}, data {}, as {}, core {};
};

void closeFd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void closePrepared(Prepared& p) {
    for (Rule& rule : p.rules) {
        closeFd(rule.fd);
    }
    p.rules.clear();
    closeFd(p.devNull);
    closeFd(p.log);
    closeFd(p.gateChild);
    closeFd(p.gateParent);
}

std::vector<std::string> boxEnvironment(const std::string& job) {
    return { "PATH=/usr/bin:/bin", "HOME=" + job, "TMPDIR=" + job + "/tmp", "LANG=C.UTF-8",
             "XDG_CONFIG_HOME=" + job + "/config", "XDG_CACHE_HOME=" + job + "/cache", "XDG_DATA_HOME=" + job + "/data",
             "BLENDER_USER_CONFIG=" + job + "/config/blender", "BLENDER_USER_SCRIPTS=" + job + "/user-scripts",
             "BLENDER_USER_DATAFILES=" + job + "/datafiles", "BLENDER_USER_EXTENSIONS=" + job + "/extensions",
             "PYTHONDONTWRITEBYTECODE=1", "PYTHONNOUSERSITE=1" };
}

bool landlockRules(const Paths& paths, const Box& box, int abi, Prepared& p, std::string& error) {
    using namespace landlock;
    const uint64_t handled = fsRights(abi);
    p.ruleset.fs = handled;
    p.rulesetSize = sizeof(uint64_t);
    if (abi >= 4) {
        p.ruleset.net = NET_BIND_TCP | NET_CONNECT_TCP;  // handled with no rule: no TCP at all
        p.rulesetSize = 2 * sizeof(uint64_t);
    }
    if (abi >= 6) {
        p.ruleset.scoped = SCOPE_ABSTRACT_UNIX_SOCKET | SCOPE_SIGNAL;
        p.rulesetSize = 3 * sizeof(uint64_t);
    }
    const uint64_t READ = READ_FILE | READ_DIR;
    const uint64_t JOB = READ | WRITE_FILE | REMOVE_DIR | REMOVE_FILE | MAKE_DIR | MAKE_REG | REFER | TRUNCATE;
    struct Entry {
        std::string path;
        uint64_t access;
        bool required;
    };
    std::vector<Entry> entries = {
        { "/usr", READ, true }, { "/lib", READ, false }, { "/lib64", READ, false }, { "/lib32", READ, false },
        { "/libx32", READ, false },
        // the short /etc list: the loader's cache, the time zone, fonts, the GL vendor list, Mesa's settings
        { "/etc/ld.so.cache", READ, false }, { "/etc/ld.so.conf", READ, false }, { "/etc/ld.so.conf.d", READ, false },
        { "/etc/localtime", READ, false }, { "/etc/fonts", READ, false }, { "/etc/glvnd", READ, false },
        { "/etc/drirc", READ, false },
        { "/sys/devices/system/cpu", READ, false }, { "/sys/devices/system/node", READ, false },
        { "/dev/null", READ_FILE | WRITE_FILE | TRUNCATE, true }, { "/dev/zero", READ_FILE, false },
        { "/dev/urandom", READ_FILE, false }, { "/dev/random", READ_FILE, false },
        { "/dev/dri", READ | WRITE_FILE | IOCTL_DEV, false },  // a GPU's render nodes, where there is one
        { paths.blenderDir, READ | EXECUTE, true }, { paths.loader, READ_FILE | EXECUTE, true },
        { paths.scripts, READ, true }, { paths.job, JOB, true } };
    if (box.proc == Proc::OWN) {
        // system-wide numbers only (no other process in them); the box's own entry is added in the child
        for (const char* file : { "/proc/cpuinfo", "/proc/meminfo", "/proc/stat" }) {
            entries.push_back({ file, READ_FILE, false });
        }
        p.procOwnAccess = READ & handled;
    }
    for (const Entry& entry : entries) {
        const int fd = open(entry.path.c_str(), O_PATH | O_CLOEXEC);
        if (fd < 0) {
            if (entry.required) {
                error = "a required folder or file for the box is missing";
                return false;
            }
            continue;
        }
        struct stat info {};
        fstat(fd, &info);
        uint64_t access = entry.access & handled;
        if (!S_ISDIR(info.st_mode)) {
            access &= FILE_RIGHTS;
        }
        p.rules.push_back({ fd, access });
    }
    return true;
}

// the filter. The accumulator holds the call's number between checks; a block that loads an argument ends in returns
std::vector<sock_filter> seccompFilter(std::vector<size_t>& selfSlots) {
    std::vector<sock_filter> f;
    const uint32_t NR = offsetof(seccomp_data, nr);
    const uint32_t ARCH = offsetof(seccomp_data, arch);
    auto argLow = [](int i) { return static_cast<uint32_t>(offsetof(seccomp_data, args) + 8 * i); };  // little end
    const uint32_t ALLOW = SECCOMP_RET_ALLOW;
    const uint32_t REFUSE = SECCOMP_RET_ERRNO | EPERM;
    const uint32_t NO_SOCKET = SECCOMP_RET_ERRNO | EACCES;
    const uint32_t NOT_HERE = SECCOMP_RET_ERRNO | ENOSYS;
    auto stmt = [&](uint16_t code, uint32_t k) { f.push_back(BPF_STMT(code, k)); };
    auto answer = [&](long nr, uint32_t ret) {
        f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 1));
        stmt(BPF_RET | BPF_K, ret);
    };
    auto guarded = [&](long nr, const std::vector<sock_filter>& body) {
        f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, static_cast<uint8_t>(body.size())));
        f.insert(f.end(), body.begin(), body.end());
    };
    // the box's own process only; its pid is written into the comparison in the child
    auto selfOnly = [&](long nr) {
        selfSlots.push_back(f.size() + 2);
        guarded(nr, { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, argLow(0)), BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),
                      BPF_STMT(BPF_RET | BPF_K, ALLOW), BPF_STMT(BPF_RET | BPF_K, REFUSE) });
    };

    stmt(BPF_LD | BPF_W | BPF_ABS, ARCH);
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0));
    stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    stmt(BPF_LD | BPF_W | BPF_ABS, NR);
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, __X32_SYSCALL_BIT, 0, 1));  // the x32 calls: none
    stmt(BPF_RET | BPF_K, REFUSE);

    // the exec gate: this program decides
    answer(__NR_execve, SECCOMP_RET_USER_NOTIF);
    answer(__NR_execveat, SECCOMP_RET_USER_NOTIF);
    // no other process: threads only
    answer(__NR_fork, REFUSE);
    answer(__NR_vfork, REFUSE);
    answer(NR_CLONE3, NOT_HERE);  // its flags are behind a pointer; the C library falls back to clone
    const uint32_t NAMESPACES = CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER |
                                CLONE_NEWPID | CLONE_NEWNET;
    guarded(__NR_clone, { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, argLow(0)),
                          BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, NAMESPACES, 2, 0),
                          BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 0, 1), BPF_STMT(BPF_RET | BPF_K, ALLOW),
                          BPF_STMT(BPF_RET | BPF_K, REFUSE) });
    // no sockets of any family, Unix and netlink included (item a)
    answer(__NR_socket, NO_SOCKET);
    answer(__NR_socketpair, NO_SOCKET);
    // signals: the box's own process only (item c)
    for (long nr : { static_cast<long>(__NR_kill), static_cast<long>(__NR_tkill), static_cast<long>(__NR_tgkill),
                     static_cast<long>(__NR_rt_sigqueueinfo), static_cast<long>(__NR_rt_tgsigqueueinfo) }) {
        selfOnly(nr);
    }
    answer(NR_PIDFD_SEND_SIGNAL, REFUSE);
    // no typing into a terminal (item e)
    guarded(__NR_ioctl, { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, argLow(1)), BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TIOCSTI, 2, 0),
                          BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TIOCLINUX, 1, 0), BPF_STMT(BPF_RET | BPF_K, ALLOW),
                          BPF_STMT(BPF_RET | BPF_K, REFUSE) });
    // other processes, other ways in, the kernel's own services
    for (long nr : { static_cast<long>(__NR_ptrace), static_cast<long>(__NR_process_vm_readv),
                     static_cast<long>(__NR_process_vm_writev), static_cast<long>(__NR_kcmp), NR_PIDFD_GETFD,
                     NR_PROCESS_MADVISE, NR_IO_URING_SETUP, NR_IO_URING_ENTER, NR_IO_URING_REGISTER,
                     static_cast<long>(__NR_bpf), static_cast<long>(__NR_perf_event_open),
                     static_cast<long>(__NR_userfaultfd), static_cast<long>(__NR_unshare), static_cast<long>(__NR_setns),
                     static_cast<long>(__NR_mount), static_cast<long>(__NR_umount2), static_cast<long>(__NR_pivot_root),
                     static_cast<long>(__NR_chroot), NR_OPEN_TREE, NR_MOVE_MOUNT, NR_FSOPEN, NR_FSCONFIG, NR_FSMOUNT,
                     NR_FSPICK, NR_MOUNT_SETATTR, static_cast<long>(__NR_add_key), static_cast<long>(__NR_request_key),
                     static_cast<long>(__NR_keyctl), static_cast<long>(__NR_kexec_load),
                     static_cast<long>(__NR_kexec_file_load), static_cast<long>(__NR_init_module),
                     static_cast<long>(__NR_finit_module), static_cast<long>(__NR_delete_module),
                     static_cast<long>(__NR_acct), static_cast<long>(__NR_swapon), static_cast<long>(__NR_swapoff),
                     static_cast<long>(__NR_reboot), static_cast<long>(__NR_quotactl), NR_QUOTACTL_FD,
                     static_cast<long>(__NR_open_by_handle_at), static_cast<long>(__NR_name_to_handle_at),
                     static_cast<long>(__NR_iopl), static_cast<long>(__NR_ioperm), static_cast<long>(__NR_uselib),
                     static_cast<long>(__NR_vhangup), static_cast<long>(__NR_syslog),
                     static_cast<long>(__NR_fanotify_init) }) {
        answer(nr, REFUSE);
    }
    stmt(BPF_RET | BPF_K, ALLOW);
    return f;
}

// the child, between fork and exec: system calls only, nothing allocated (this program has threads)
bool sendFd(int socket, int fd) {
    char byte = 'L';
    iovec io { &byte, 1 };
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
    std::memset(control, 0, sizeof control);
    msghdr message {};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &fd, sizeof fd);
    return sendmsg(socket, &message, MSG_NOSIGNAL) == 1;
}

[[noreturn]] void startInChild(Prepared& p) {
    if (p.box.limits) {
        // its own session: no terminal it could type into (item e); killed when this program's thread dies
        if (setsid() < 0 || prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != p.parent) {
            _exit(121);
        }
    }
    if (dup2(p.devNull, 0) < 0 || dup2(p.log, 1) < 0 || dup2(p.log, 2) < 0 || chdir(p.job.c_str()) != 0) {
        _exit(122);
    }
    if (p.box.limits && (setrlimit(RLIMIT_CPU, &p.cpu) != 0 || setrlimit(RLIMIT_DATA, &p.data) != 0 ||
                         setrlimit(RLIMIT_AS, &p.as) != 0 || setrlimit(RLIMIT_CORE, &p.core) != 0)) {
        _exit(123);
    }
    if ((p.box.landlock || p.box.seccomp) && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        _exit(124);
    }
    if (p.box.landlock) {
        const int ruleset = static_cast<int>(syscall(landlock::CREATE_RULESET, &p.ruleset, p.rulesetSize, 0));
        if (ruleset < 0) {
            _exit(125);
        }
        for (const Rule& rule : p.rules) {
            landlock::PathBeneath beneath { rule.access, rule.fd };
            if (syscall(landlock::ADD_RULE, ruleset, landlock::RULE_PATH_BENEATH, &beneath, 0) != 0) {
                _exit(125);
            }
        }
        if (p.procOwnAccess) {
            const int own = open("/proc/self", O_PATH | O_DIRECTORY | O_CLOEXEC);  // this process's own entry
            landlock::PathBeneath beneath { p.procOwnAccess, own };
            if (own < 0 || syscall(landlock::ADD_RULE, ruleset, landlock::RULE_PATH_BENEATH, &beneath, 0) != 0) {
                _exit(125);
            }
        }
        if (syscall(landlock::RESTRICT_SELF, ruleset, 0) != 0) {
            _exit(125);
        }
        close(ruleset);
    }
    if (p.box.seccomp) {
        const uint32_t self = static_cast<uint32_t>(getpid());
        for (size_t slot : p.selfPidSlots) {
            p.filter[slot].k = self;
        }
        sock_fprog program { static_cast<unsigned short>(p.filter.size()), p.filter.data() };
        const int gate = static_cast<int>(syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER,
                                                  &program));
        if (gate < 0 || !sendFd(p.gateChild, gate)) {
            _exit(126);
        }
        close(gate);
    }
    // nothing of this program's open files reaches Blender: standard input, output and error only
    if (syscall(NR_CLOSE_RANGE, 3U, ~0U, 0U) != 0) {
        for (int fd = 3; fd < 65536; ++fd) {
            close(fd);
        }
    }
    execve(p.blender.c_str(), p.argv.data(), p.envp.data());
    _exit(127);
}

int receiveFd(int socket, int timeoutMs) {
    pollfd ready { socket, POLLIN, 0 };
    if (poll(&ready, 1, timeoutMs) <= 0) {
        return -1;
    }
    char byte = 0;
    iovec io { &byte, 1 };
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] {};
    msghdr message {};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    if (recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != 1) {
        return -1;
    }
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
        return -1;
    }
    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(header), sizeof fd);
    return fd;
}

// the exec gate: the box's first exec (the launcher's own start of Blender, from the child itself) is let through;
// every later one is answered EPERM. Once this program is gone, the kernel answers ENOSYS: it fails closed
struct ExecGate {
    int listener = -1;
    pid_t child = 0;
    std::atomic<bool> stop { false };
    std::atomic<int> allowed { 0 };
    std::atomic<int> refused { 0 };
    std::thread thread;

    void start(int fd, pid_t pid) {
        listener = fd;
        child = pid;
        thread = std::thread([this] { loop(); });
    }
    void loop() {
        while (!stop) {
            pollfd ready { listener, POLLIN, 0 };
            const int count = poll(&ready, 1, 200);
            if (count < 0 && errno != EINTR) {
                break;
            }
            if (count <= 0) {
                continue;
            }
            if (!(ready.revents & POLLIN)) {
                break;  // POLLHUP: nothing is left under the filter
            }
            seccomp_notif request;
            std::memset(&request, 0, sizeof request);
            if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &request) != 0) {
                if (errno == EINTR || errno == ENOENT) {
                    continue;
                }
                break;
            }
            seccomp_notif_resp response;
            std::memset(&response, 0, sizeof response);
            response.id = request.id;
            if (allowed == 0 && refused == 0 && static_cast<pid_t>(request.pid) == child) {
                response.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;  // the launcher's own exec of Blender
                ++allowed;
            } else {
                response.error = -EPERM;
                ++refused;
            }
            ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &response);  // ENOENT: the caller is gone meanwhile
        }
    }
    void finish() {
        stop = true;
        if (thread.joinable()) {
            thread.join();
        }
        closeFd(listener);
    }
    ~ExecGate() { finish(); }
};

struct Launch {
    pid_t pid = -1;
    std::unique_ptr<ExecGate> gate;
    std::string error;
};

struct Ended {
    bool byItself = false;  // false: it ran past its time and this program ended it
    int status = 0;
};

// our own child, by the pid fork gave us (never by a name)
Ended waitFor(pid_t pid, int ms) {
    for (int waited = 0;; waited += 50) {
        int status = 0;
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return { true, status };
        }
        if (done < 0) {
            return { true, -1 };
        }
        if (waited >= ms) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    return { false, status };
}

std::string describe(const Ended& ended) {
    std::string how = ended.status == -1           ? "unknown"
                      : WIFEXITED(ended.status)    ? "exit " + std::to_string(WEXITSTATUS(ended.status))
                      : WIFSIGNALED(ended.status)  ? "signal " + std::to_string(WTERMSIG(ended.status))
                                                   : "status " + std::to_string(ended.status);
    return ended.byItself ? how : "ran past its time, ended by the test (" + how + ")";
}

Launch start(const Paths& paths, const std::string& mode, const Box& box, const std::string& logName, int abi) {
    Launch launch;
    Prepared p;
    p.box = box;
    p.blender = paths.blender;
    p.job = paths.job;
    p.parent = getpid();
    p.argStrings = { paths.blender, "--background", "--factory-startup", "--disable-autoexec", "-noaudio",
                     "--python-exit-code", "3", "--python", paths.scripts + "/probe.py", "--", paths.job, mode };
    if (box.ownEnv) {
        p.envStrings = boxEnvironment(paths.job);
    } else {
        for (char** line = environ; *line; ++line) {
            p.envStrings.push_back(*line);
        }
    }
    for (std::string& s : p.argStrings) {
        p.argv.push_back(&s[0]);
    }
    p.argv.push_back(nullptr);
    for (std::string& s : p.envStrings) {
        p.envp.push_back(&s[0]);
    }
    p.envp.push_back(nullptr);
    const rlim_t gb = 1ULL << 30;
    p.cpu = { static_cast<rlim_t>(box.cpuSeconds), static_cast<rlim_t>(box.cpuSeconds + 2) };
    p.data = { 4 * gb, 4 * gb };
    p.as = { 16 * gb, 16 * gb };
    p.core = { 0, 0 };
    p.devNull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    p.log = open((paths.job + "/" + logName).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (p.devNull < 0 || p.log < 0) {
        launch.error = "the standard files couldn't be opened";
        closePrepared(p);
        return launch;
    }
    if (box.landlock && !landlockRules(paths, box, abi, p, launch.error)) {
        closePrepared(p);
        return launch;
    }
    if (box.seccomp) {
        p.filter = seccompFilter(p.selfPidSlots);
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
            launch.error = "no socket pair for the exec gate";
            closePrepared(p);
            return launch;
        }
        p.gateParent = pair[0];
        p.gateChild = pair[1];
    }
    const pid_t pid = fork();
    if (pid < 0) {
        launch.error = "fork failed";
        closePrepared(p);
        return launch;
    }
    if (pid == 0) {
        startInChild(p);
    }
    launch.pid = pid;
    closeFd(p.gateChild);
    if (box.seccomp) {
        const int gate = receiveFd(p.gateParent, 15000);
        if (gate < 0) {
            const Ended ended = waitFor(pid, 2000);
            launch.error = "the exec gate didn't arrive (" + describe(ended) + ")";
            launch.pid = -1;
            closePrepared(p);
            return launch;
        }
        launch.gate = std::make_unique<ExecGate>();
        launch.gate->start(gate, pid);
    }
    closePrepared(p);
    return launch;
}

struct Run {
    std::string end;
    double seconds = 0;
    std::string error;
    int gateAllowed = 0;
    int gateRefused = 0;
};

Run runBlender(const Paths& paths, const std::string& mode, const Box& box, const std::string& logName, int abi,
               int waitMs) {
    Run run;
    const auto began = std::chrono::steady_clock::now();
    Launch launch = start(paths, mode, box, logName, abi);
    if (!launch.error.empty()) {
        run.error = launch.error;
        return run;
    }
    run.end = describe(waitFor(launch.pid, waitMs));
    if (launch.gate) {
        launch.gate->finish();
        run.gateAllowed = launch.gate->allowed;
        run.gateRefused = launch.gate->refused;
    }
    run.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    return run;
}

std::string runJson(const Run& run) {
    std::ostringstream out;
    out.precision(1);
    out << std::fixed << "\"end\": " << jsonString(run.error.empty() ? run.end : "not started: " + run.error)
        << ", \"seconds\": " << run.seconds << ", \"exec_gate\": {\"allowed\": " << run.gateAllowed
        << ", \"refused\": " << run.gateRefused << "}";
    return out.str();
}

// a pid's start time and state from /proc/<pid>/stat (so a reused pid is never taken for Blender)
bool processStat(pid_t pid, unsigned long long& startTime, char& state) {
    const std::string line = firstLine("/proc/" + std::to_string(pid) + "/stat");
    const size_t close = line.rfind(')');
    if (close == std::string::npos || close + 2 >= line.size()) {
        return false;
    }
    std::istringstream fields(line.substr(close + 2));  // field 3 on
    std::string skip;
    fields >> state;
    for (int i = 4; i < 22; ++i) {
        fields >> skip;
    }
    fields >> startTime;
    return !fields.fail();
}

// the network targets: listeners of this program's own (each sends the run's token and closes), and a closed port
struct Targets {
    int tcp = -1, udp = -1, unixPath = -1, unixAbstract = -1;
    std::string token, unixPathName, abstractName, runtime, sessionBus, json;
    std::atomic<int> tcpAccepted { 0 }, udpReceived { 0 }, unixAccepted { 0 }, abstractAccepted { 0 };
    std::atomic<bool> stop { false };
    std::thread loop;
};

void acceptLoop(Targets* t) {
    while (!t->stop) {
        pollfd ready[4] = { { t->tcp, POLLIN, 0 }, { t->udp, POLLIN, 0 }, { t->unixPath, POLLIN, 0 },
                            { t->unixAbstract, POLLIN, 0 } };
        if (poll(ready, 4, 200) <= 0) {
            continue;
        }
        const std::string line = t->token + "\n";
        std::atomic<int>* counts[4] = { &t->tcpAccepted, nullptr, &t->unixAccepted, &t->abstractAccepted };
        for (int i : { 0, 2, 3 }) {
            if (ready[i].fd >= 0 && (ready[i].revents & POLLIN)) {
                const int client = accept4(ready[i].fd, nullptr, nullptr, SOCK_CLOEXEC);
                if (client >= 0) {
                    ++*counts[i];
                    send(client, line.data(), line.size(), MSG_NOSIGNAL);
                    shutdown(client, SHUT_RDWR);
                    close(client);
                }
            }
        }
        if (ready[1].fd >= 0 && (ready[1].revents & POLLIN)) {
            char buffer[128];
            const ssize_t got = recv(t->udp, buffer, sizeof buffer, 0);
            if (got >= static_cast<ssize_t>(t->token.size()) && std::memcmp(buffer, t->token.data(), t->token.size()) == 0) {
                ++t->udpReceived;
            }
        }
    }
}

std::string addressJson(const sockaddr_in& address) {
    char text[INET_ADDRSTRLEN] {};
    inet_ntop(AF_INET, &address.sin_addr, text, sizeof text);
    return std::string("[\"") + text + "\", " + std::to_string(ntohs(address.sin_port)) + "]";
}

bool openTargets(Targets& t, const Paths& p, std::string& error) {
    t.token = "darkride-6c-" + randomHex(16);
    sockaddr_in loopback {};
    loopback.sin_family = AF_INET;
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof loopback;
    t.tcp = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in tcp = loopback;
    if (t.tcp < 0 || bind(t.tcp, reinterpret_cast<sockaddr*>(&tcp), sizeof tcp) != 0 || listen(t.tcp, 8) != 0 ||
        getsockname(t.tcp, reinterpret_cast<sockaddr*>(&tcp), &length) != 0) {
        error = "the loopback listener couldn't be opened";
        return false;
    }
    t.udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in udp = loopback;
    length = sizeof udp;
    if (t.udp < 0 || bind(t.udp, reinterpret_cast<sockaddr*>(&udp), sizeof udp) != 0 ||
        getsockname(t.udp, reinterpret_cast<sockaddr*>(&udp), &length) != 0) {
        error = "the loopback UDP port couldn't be opened";
        return false;
    }
    // this machine's own address: a UDP socket "connected" to a documentation address sends nothing and names it
    sockaddr_in own {};
    const int probe = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in documentation {};
    documentation.sin_family = AF_INET;
    documentation.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &documentation.sin_addr);
    length = sizeof own;
    if (probe < 0 || connect(probe, reinterpret_cast<sockaddr*>(&documentation), sizeof documentation) != 0 ||
        getsockname(probe, reinterpret_cast<sockaddr*>(&own), &length) != 0) {
        error = "this machine's own address wasn't found";
        return false;
    }
    close(probe);
    // a port free just now on that address: bound and closed again, never listened on
    const int spare = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    own.sin_port = 0;
    length = sizeof own;
    if (spare < 0 || bind(spare, reinterpret_cast<sockaddr*>(&own), sizeof own) != 0 ||
        getsockname(spare, reinterpret_cast<sockaddr*>(&own), &length) != 0) {
        error = "no closed port on this machine's own address";
        return false;
    }
    close(spare);
    // a Unix socket where Make's link will live (the runtime folder), and one in the abstract namespace
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    const std::string runUser = "/run/user/" + std::to_string(getuid());
    t.runtime = runtime && *runtime ? runtime : (fs::is_directory(runUser) ? runUser : std::string());
    t.unixPathName = (t.runtime.empty() ? p.test : t.runtime) + "/darkride-box-test-" + t.token.substr(12, 16) + ".sock";
    t.abstractName = "darkride-box-test-" + t.token;
    sockaddr_un path {};
    path.sun_family = AF_UNIX;
    sockaddr_un abstract {};
    abstract.sun_family = AF_UNIX;
    if (t.unixPathName.size() >= sizeof path.sun_path || t.abstractName.size() + 1 >= sizeof abstract.sun_path) {
        error = "the Unix socket names are too long";
        return false;
    }
    std::memcpy(path.sun_path, t.unixPathName.c_str(), t.unixPathName.size() + 1);
    std::memcpy(abstract.sun_path + 1, t.abstractName.data(), t.abstractName.size());
    const socklen_t abstractLength = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + t.abstractName.size());
    t.unixPath = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    t.unixAbstract = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (t.unixPath < 0 || bind(t.unixPath, reinterpret_cast<sockaddr*>(&path), sizeof path) != 0 ||
        listen(t.unixPath, 8) != 0 || t.unixAbstract < 0 ||
        bind(t.unixAbstract, reinterpret_cast<sockaddr*>(&abstract), abstractLength) != 0 || listen(t.unixAbstract, 8) != 0) {
        error = "the Unix sockets couldn't be opened";
        return false;
    }
    struct stat bus {};
    if (stat((runUser + "/bus").c_str(), &bus) == 0 && S_ISSOCK(bus.st_mode)) {
        t.sessionBus = runUser + "/bus";
    }
    t.loop = std::thread(acceptLoop, &t);
    std::ostringstream json;
    json << "{\"token\": " << jsonString(t.token) << ", \"loopback\": " << addressJson(tcp) << ", \"udp\": " << addressJson(udp)
         << ", \"own_closed\": " << addressJson(own) << ", \"unix_path\": " << jsonString(t.unixPathName)
         << ", \"unix_abstract\": " << jsonString(t.abstractName)
         << ", \"session_bus\": " << (t.sessionBus.empty() ? "null" : jsonString(t.sessionBus))
         << ", \"runtime\": " << (t.runtime.empty() ? "null" : jsonString(t.runtime))
         << ", \"home\": " << jsonString(getenv("HOME") ? getenv("HOME") : "/nonexistent")
         << ", \"outside\": " << jsonString(p.outside) << ", \"blender_dir\": " << jsonString(p.blenderDir)
         << ", \"scripts\": " << jsonString(p.scripts) << ", \"loader\": " << jsonString(p.loader)
         << ", \"parent_pid\": " << getpid() << ", \"tmp_name\": " << jsonString("darkride-box-test-" + t.token.substr(12, 16))
         << "}";
    t.json = json.str();
    return true;
}

void closeTargets(Targets& t) {
    t.stop = true;
    if (t.loop.joinable()) {
        t.loop.join();
    }
    for (int* fd : { &t.tcp, &t.udp, &t.unixPath, &t.unixAbstract }) {
        closeFd(*fd);
    }
    if (!t.unixPathName.empty()) {
        unlink(t.unixPathName.c_str());
    }
}

std::string countsJson(const Targets& t) {
    return "{\"tcp\": " + std::to_string(t.tcpAccepted.load()) + ", \"udp\": " + std::to_string(t.udpReceived.load()) +
           ", \"unix_path\": " + std::to_string(t.unixAccepted.load()) +
           ", \"unix_abstract\": " + std::to_string(t.abstractAccepted.load()) + "}";
}

int countsTotal(const Targets& t) {
    return t.tcpAccepted + t.udpReceived + t.unixAccepted + t.abstractAccepted;
}

void keepLog(const Paths& p, bool keep, const std::string& logName) {
    if (keep) {
        std::error_code ec;
        fs::create_directories(p.logs, ec);
        fs::copy_file(p.job + "/" + logName, p.logs + "/" + logName, fs::copy_options::overwrite_existing, ec);
    }
}

// the kill case's parent: a boxed Blender that loops; its pid and start time written; then it waits to be killed
int runParent(const std::string& blender, const std::string& test, bool ownProc) {
    Paths p;
    std::string error;
    if (!makePaths(blender, test, false, p, error)) {
        return 2;
    }
    Launch launch = start(p, "runaway", makeBox(true, true, true, ownProc ? Proc::OWN : Proc::NONE, true),
                          "blender-kill-case.log", landlock::abi());
    if (!launch.error.empty() || !waitForFile(p.job + "/runaway-started.txt", 120)) {
        return 2;
    }
    unsigned long long startTime = 0;
    char state = 0;
    if (!processStat(launch.pid, startTime, state)) {
        return 2;
    }
    std::ofstream(p.job + "/job-pids.txt") << launch.pid << " " << startTime << "\n";
    for (;;) {
        pause();  // killed by the test; Blender gets SIGKILL from the kernel (PR_SET_PDEATHSIG)
    }
}

std::string killCase(const Paths& p, bool ownProc) {
    std::error_code ec;
    fs::remove(p.job + "/runaway-started.txt", ec);
    fs::remove(p.job + "/job-pids.txt", ec);
    const pid_t parent = fork();
    if (parent < 0) {
        return "{\"pass\": false, \"detail\": \"fork failed\"}";
    }
    if (parent == 0) {
        const std::string proc = ownProc ? "1" : "0";
        execl(p.self.c_str(), p.self.c_str(), "--parent", p.blender.c_str(), p.test.c_str(), proc.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    bool written = false;
    for (int i = 0; i < 1500 && !written; ++i) {
        written = fs::exists(p.job + "/job-pids.txt") && !firstLine(p.job + "/job-pids.txt").empty();
        if (!written) {
            if (waitpid(parent, nullptr, WNOHANG) == parent) {
                return "{\"pass\": false, \"detail\": \"the parent copy ended before its Blender started\"}";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (!written) {
        waitFor(parent, 0);
        return "{\"pass\": false, \"detail\": \"the parent copy's Blender didn't start\"}";
    }
    std::istringstream line(firstLine(p.job + "/job-pids.txt"));
    pid_t boxed = 0;
    unsigned long long startTime = 0;
    line >> boxed >> startTime;
    kill(parent, SIGKILL);  // the parent copy: our own child, by its pid
    waitpid(parent, nullptr, 0);
    bool ended = false;
    for (int i = 0; i < 50 && !ended; ++i) {
        unsigned long long now = 0;
        char state = 0;
        ended = !processStat(boxed, now, state) || now != startTime || state == 'Z' || state == 'X';
        if (!ended) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return std::string("{\"pass\": ") + (ended ? "true" : "false") + ", \"detail\": " +
           jsonString(ended ? "Blender ended with its parent" : "Blender was still running 5 s after its parent was killed") +
           "}";
}

std::string cpuCase(const Paths& p, Box box, int abi, bool keepLogs, bool& passed) {
    std::error_code ec;
    fs::remove(p.job + "/runaway-started.txt", ec);
    box.cpuSeconds = CPU_CAP_TEST_S;
    const auto began = std::chrono::steady_clock::now();
    Launch launch = start(p, "runaway", box, "blender-cpu-case.log", abi);
    if (!launch.error.empty()) {
        passed = false;
        return "{\"pass\": false, \"detail\": " + jsonString("not started: " + launch.error) + "}";
    }
    const Ended ended = waitFor(launch.pid, 90000);
    launch.gate->finish();
    keepLog(p, keepLogs, "blender-cpu-case.log");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    const bool started = fs::exists(p.job + "/runaway-started.txt");
    const bool bySignal = ended.byItself && ended.status != -1 && WIFSIGNALED(ended.status) &&
                          (WTERMSIG(ended.status) == SIGXCPU || WTERMSIG(ended.status) == SIGKILL);
    passed = started && bySignal;
    std::ostringstream out;
    out.precision(1);
    out << std::fixed << "{\"pass\": " << (passed ? "true" : "false") << ", \"started\": " << (started ? "true" : "false")
        << ", \"end\": " << jsonString(describe(ended)) << ", \"seconds\": " << seconds << "}";
    return out.str();
}

// what user namespaces do on this machine, unboxed (release, 11-07: Ubuntu 24.04 restricts them; the box uses none).
// Information only. A single-threaded child (the kernel refuses a threaded process, as Blender is) makes a user
// namespace, maps its own user, then tries a mount namespace inside it (which needs the namespace's own capabilities);
// it exits with the step reached: 0 all three, 10+errno the user namespace refused, 20 the map refused, 30+errno the
// mount namespace refused
std::string userNamespaces() {
    char uidMap[64];
    char gidMap[64];
    std::snprintf(uidMap, sizeof uidMap, "%u %u 1\n", static_cast<unsigned>(getuid()), static_cast<unsigned>(getuid()));
    std::snprintf(gidMap, sizeof gidMap, "%u %u 1\n", static_cast<unsigned>(getgid()), static_cast<unsigned>(getgid()));
    const pid_t child = fork();
    if (child < 0) {
        return "\"fork failed\"";
    }
    if (child == 0) {  // system calls only from here
        if (unshare(CLONE_NEWUSER) != 0) {
            _exit(10 + (errno < 10 ? errno : 9));
        }
        auto put = [](const char* path, const char* text) {
            const int fd = open(path, O_WRONLY | O_CLOEXEC);
            const ssize_t length = static_cast<ssize_t>(std::strlen(text));
            const bool written = fd >= 0 && write(fd, text, std::strlen(text)) == length;
            if (fd >= 0) {
                close(fd);
            }
            return written;
        };
        if (!put("/proc/self/setgroups", "deny") || !put("/proc/self/uid_map", uidMap) || !put("/proc/self/gid_map", gidMap)) {
            _exit(20);
        }
        if (unshare(CLONE_NEWNS) != 0) {
            _exit(30 + (errno < 10 ? errno : 9));
        }
        _exit(0);
    }
    const Ended ended = waitFor(child, 5000);
    const int code = ended.byItself && WIFEXITED(ended.status) ? WEXITSTATUS(ended.status) : -1;
    const std::string what = code == 0    ? "made, mapped, and a mount namespace inside it"
                             : code >= 30 ? "made and mapped; a mount namespace inside it refused (errno " +
                                                std::to_string(code - 30) + ")"
                             : code == 20 ? "made; mapping its own user refused"
                             : code >= 10 ? "refused (errno " + std::to_string(code - 10) + ")"
                                          : "unknown (" + describe(ended) + ")";
    return jsonString(what);
}

}  // namespace

int main(int argc, char** argv) {
    if (geteuid() == 0 || getuid() == 0) {
        std::cerr << "run this as the normal user, never as root: a root run would test a different box\n";
        return 4;
    }
    if (argc == 5 && std::string(argv[1]) == "--parent") {
        return runParent(argv[2], argv[3], std::string(argv[4]) == "1");
    }
    bool keepLogs = false;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--keep-logs") {
            keepLogs = true;
        } else {
            argc = 0;
        }
    }
    if (argc < 3) {
        std::cerr << "usage: make-box-test <Blender's \"blender\" program> <test folder> [--keep-logs]\n";
        return 2;
    }
    Paths p;
    std::string error;
    if (!makePaths(argv[1], argv[2], true, p, error)) {
        std::cerr << error << "\n";
        return 3;
    }
    // a key-shaped variable in this program's own environment: Blender must not see it (its environment is built afresh)
    setenv("ANTHROPIC_API_KEY", TEST_KEY, 1);

    utsname system {};
    uname(&system);
    const int abi = landlock::abi();
    std::string lsm = firstLine("/sys/kernel/security/lsm");
    std::cout << "kernel " << system.release << ", Landlock ABI " << abi << ", LSMs " << (lsm.empty() ? "unknown" : lsm)
              << "\n";
    const std::string userNs = userNamespaces();  // unboxed, information only (the box uses none)
    std::cout << "user namespaces here (unboxed): " << userNs << "\n";

    Targets targets;
    if (!openTargets(targets, p, error)) {
        std::cerr << error << "\n";
        closeTargets(targets);
        return 3;
    }
    std::ofstream(p.job + "/targets.json") << targets.json;
    const std::string tmpName = "darkride-box-test-" + targets.token.substr(12, 16);

    // the UNBOXED control: every check, each target shown there
    Run control = runBlender(p, "control", makeBox(false, false, false, Proc::NONE, true), "blender-control.log", abi,
                             PROBE_WAIT_MS);
    keepLog(p, keepLogs, "blender-control.log");
    const std::string controlResults = readFile(p.job + "/control-results.json");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const std::string countsAfterControl = countsJson(targets);
    const int totalAfterControl = countsTotal(targets);
    removeOutsideWrites(p, tmpName);  // the box must be the one refused, not stopped by the control's leftovers
    std::cout << "unboxed control: " << (controlResults == "null" ? "no results" : "done") << " ("
              << (control.error.empty() ? control.end : control.error) << ")\n";

    // the start ladder: controls take one piece at a time; the box steps, strictest first
    struct Step {
        const char* name;
        Box box;
        bool isBox;
    };
    const std::vector<Step> ladder = {
        { "control: Blender unboxed, its usual environment", makeBox(false, false, false, Proc::NONE, false), false },
        { "control: Blender unboxed, the box's environment", makeBox(false, false, false, Proc::NONE, true), false },
        { "control: the limits only (own session, ended with its parent, CPU and memory caps)",
          makeBox(true, false, false, Proc::NONE, true), false },
        { "control: Landlock only, no /proc", makeBox(false, true, false, Proc::NONE, true), false },
        { "control: seccomp only, with the exec gate", makeBox(false, false, true, Proc::NONE, true), false },
        { "box: limits, Landlock with no /proc, seccomp with the exec gate", makeBox(true, true, true, Proc::NONE, true),
          true },
        { "box: limits, Landlock with the box's own /proc entry, seccomp with the exec gate",
          makeBox(true, true, true, Proc::OWN, true), true } };
    std::ostringstream starts;
    const Step* chosen = nullptr;
    int stepNumber = 0;
    for (const Step& step : ladder) {
        ++stepNumber;
        if (step.box.landlock && abi < 1) {
            starts << (stepNumber > 1 ? ", " : "") << "{\"step\": " << jsonString(step.name)
                   << ", \"started\": false, \"end\": \"not run: no Landlock on this kernel\"}";
            std::cout << step.name << ": not run (no Landlock)\n";
            continue;
        }
        std::error_code ec;
        fs::remove(p.job + "/started.txt", ec);
        const std::string log = "blender-start-" + std::to_string(stepNumber) + ".log";
        const Run run = runBlender(p, "start", step.box, log, abi, START_WAIT_MS);
        keepLog(p, keepLogs, log);
        const bool rendered = readFile(p.job + "/started.txt", "") == "rendered";
        starts << (stepNumber > 1 ? ", " : "") << "{\"step\": " << jsonString(step.name)
               << ", \"started\": " << (rendered ? "true" : "false") << ", " << runJson(run) << "}";
        std::cout << step.name << ": " << (rendered ? "rendered" : "didn't render") << " ("
                  << (run.error.empty() ? run.end : run.error) << ")\n";
        if (rendered && step.isBox && !chosen) {
            chosen = &step;
        }
    }

    std::string probeResults = "null";
    std::string summary;
    std::string countsAfterProbe = "null";
    int boxConnections = -1;
    Run probe;
    bool escapedDirect = false, escapedLoader = false;
    if (chosen) {
        std::error_code ec;
        fs::remove(p.job + "/probe-results.json", ec);
        probe = runBlender(p, "probe", chosen->box, "blender-probe.log", abi, PROBE_WAIT_MS);
        keepLog(p, keepLogs, "blender-probe.log");
        probeResults = readFile(p.job + "/probe-results.json");
        summary = readFile(p.job + "/probe-summary.txt", "");
        escapedDirect = fs::exists(p.job + "/exec-escaped-direct.txt");
        escapedLoader = fs::exists(p.job + "/exec-escaped-loader.txt");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        countsAfterProbe = countsJson(targets);
        boxConnections = countsTotal(targets) - totalAfterControl;
        std::cout << "probe in \"" << chosen->name << "\": " << (probe.error.empty() ? probe.end : probe.error) << "\n";
    }
    closeTargets(targets);

    std::string cpu = "\"not run\"";
    std::string killed = "\"not run\"";
    bool cpuPassed = false;
    bool killPassed = false;
    if (chosen) {
        cpu = cpuCase(p, chosen->box, abi, keepLogs, cpuPassed);
        killed = killCase(p, chosen->box.proc == Proc::OWN);
        keepLog(p, keepLogs, "blender-kill-case.log");
        killPassed = killed.rfind("{\"pass\": true", 0) == 0;
    }

    // the whole summary: the probe's lines, then the cases only this program can see
    std::vector<std::string> lines;
    auto line = [&](bool pass, const std::string& text) { lines.push_back(std::string(pass ? "PASS  " : "FAIL  ") + text); };
    lines.push_back("INFO  user namespaces on this machine, unboxed (the box uses none): " + userNs);
    if (!chosen) {
        lines.push_back("FAIL  no box form rendered: Blender would be withdrawn on this machine");
    } else {
        std::istringstream probeLines(summary);
        std::string probeLine;
        bool probeSummary = false;
        while (std::getline(probeLines, probeLine)) {
            if (probeLine.rfind("PROBE:", 0) == 0) {
                probeSummary = true;
                if (probeLine.rfind("PROBE: INCOMPLETE", 0) == 0) {
                    lines.push_back("INFO  the probe (" + probeLine + ")");  // its UNKNOWN lines are above, counted
                } else {
                    line(probeLine == "PROBE: PASS", "the probe (" + probeLine + ")");
                }
            } else if (!probeLine.empty()) {
                lines.push_back(probeLine);
            }
        }
        if (!probeSummary) {
            line(false, "the probe wrote no summary (" + probe.end + ")");
        }
        line(!escapedDirect, "no program ran in Blender's place by a direct exec (its marker is absent)");
        line(!escapedLoader, "no program ran in Blender's place through the loader (its marker is absent)");
        line(probe.gateAllowed == 1 && probe.gateRefused >= 2,
             "the exec gate let 1 start through and refused " + std::to_string(probe.gateRefused) + " (2 or more)");
        line(boxConnections == 0, "no connection or datagram from the box reached the test's listeners (" +
                                      std::to_string(boxConnections) + ")");
        line(totalAfterControl >= 4, "the unboxed control reached all 4 of the test's listeners (" +
                                         std::to_string(totalAfterControl) + " connections and datagrams)");
        line(cpuPassed, "a looping Blender under a " + std::to_string(CPU_CAP_TEST_S) + " s CPU cap ended by itself");
        line(killPassed, "Blender ended when its parent was killed");
    }
    int failed = 0;
    int unknown = 0;
    for (const std::string& l : lines) {
        failed += l.rfind("FAIL", 0) == 0;
        unknown += l.rfind("UNKNOWN", 0) == 0;
    }
    const std::string verdict = failed ? "LINUX BOX: FAIL (" + std::to_string(failed) + " failed, " + std::to_string(unknown) + " unknown)"
                                : unknown ? "LINUX BOX: INCOMPLETE (" + std::to_string(unknown) + " unknown)"
                                          : "LINUX BOX: PASS";
    lines.push_back(verdict);

    std::ofstream text(p.test + "/summary.txt");
    std::ostringstream summaryJson;
    for (size_t i = 0; i < lines.size(); ++i) {
        text << lines[i] << "\n";
        std::cout << lines[i] << "\n";
        summaryJson << (i ? ", " : "") << jsonString(lines[i]);
    }
    text.close();
    std::ofstream out(p.test + "/results.json");
    out << "{\"kernel\": " << jsonString(system.release) << ", \"landlock_abi\": " << abi
        << ", \"lsm\": " << jsonString(lsm.empty() ? "unknown" : lsm) << ", \"cores\": " << std::thread::hardware_concurrency()
        << ", \"session_bus\": " << (targets.sessionBus.empty() ? "false" : "true")
        << ", \"runtime_folder\": " << (targets.runtime.empty() ? "false" : "true") << ", \"user_namespaces\": " << userNs
        << ", \"control\": {" << runJson(control) << ", \"results\": " << controlResults << "}"
        << ", \"counts_after_control\": " << countsAfterControl << ", \"starts\": [" << starts.str() << "]"
        << ", \"box\": " << jsonString(chosen ? chosen->name : "none rendered")
        << ", \"probe\": {" << (chosen ? runJson(probe) : std::string("\"end\": \"not run\"")) << ", \"results\": " << probeResults
        << "}, \"counts_after_probe\": " << countsAfterProbe
        << ", \"exec_escaped\": {\"direct\": " << (escapedDirect ? "true" : "false") << ", \"loader\": "
        << (escapedLoader ? "true" : "false") << "}, \"cpu_cap\": " << cpu << ", \"kill_the_parent\": " << killed
        << ", \"summary\": [" << summaryJson.str() << "]}\n";
    out.close();
    std::cout << "results: " << p.test << "/results.json\n";

    std::error_code ec;
    removeOutsideWrites(p, tmpName);
    fs::remove_all(p.job, ec);
    fs::remove_all(p.outside, ec);
    return verdict == "LINUX BOX: PASS" ? 0 : 1;
}
