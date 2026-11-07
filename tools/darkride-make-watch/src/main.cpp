//
//  main.cpp
//  tools/darkride-make-watch/src
//
//  darkride-make-watch: the parent of Make's Blender (security should-fix 4). The app starts this, and this starts
//  Blender's sandbox line as its own child, so Blender ends whatever happens to the app:
//
//    darkride-make-watch <cap seconds> <pid file> -- /usr/bin/sandbox-exec <its arguments>
//
//  It kills Blender (SIGKILL) when a line comes on its standard input (the app writes "stop"), when its standard input
//  ends (the app is gone: crashed, force-quit or killed), when the cap's wall-clock seconds pass, or when it is asked to
//  end (SIGTERM, SIGINT, SIGHUP). macOS's CPU limit alone can't do this: at the hard limit the kernel only sends
//  SIGXCPU, which a script can ignore.
//
//  Blender is this process's child and is reaped only after it has exited, so its process id can't be reused while this
//  may still signal it: every kill comes before the reap. Its id is written to the pid file (made new, never through a
//  link, outside the folder the box may write) for the app's memory check only; the app never signals by that file.
//  Blender gets /dev/null as standard input and this program's standard output and error (the app reads its log
//  there), and none of this program's other files.
//
//  Exits with Blender's status: its exit code, or 128 + the signal that ended it; 125 when it can't start Blender.
//  macOS (kqueue) and Linux (poll and signalfd). On Linux this program is the box too: the child makes it before
//  Blender starts (its own session and a kill when this process dies, the limits, Landlock, seccomp with the exec
//  gate this program serves), as the Linux section below says; the app passes the work and fixed scripts folders.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(__APPLE__) && !defined(__linux__)

int main(int, char**) {
    std::fputs("darkride-make-watch: macOS and Linux only.\n", stderr);
    return 125;
}

#elif defined(__linux__)

// Linux: this program is the box as well as the watcher. Blender's own box on Linux is made by the child between fork
// and exec, exactly as the Linux box test does it (tests-manual/make-link/linux-box/make-box-test.cpp, startInChild;
// runs 1 and 2 on Ubuntu 22.04 and 24.04, security's "no /proc" form):
//
//    darkride-make-watch <cap seconds> <pid file> --job <work folder> --fixed <fixed scripts folder>
//                        [--env MAKE_NAME=VALUE]... [--folder-cap <megabytes>] -- <blender> <its arguments>
//
//  In the child: its own session (setsid) and a kill when this process dies (PR_SET_PDEATHSIG); /dev/null as standard
//  input, this program's standard output and error (the app reads the log there); the work folder as its folder; the
//  limits (CPU time from the cap, 4 GB of data memory, 16 GB of address space, no core file, each file at most the
//  folder cap); no_new_privs; Landlock (read /usr, /lib*, a short /etc list, a few /sys and /dev entries, the fixed
//  scripts; read and run Blender's folder and its ELF loader; read and write the work folder; nothing else, no /proc; on
//  ABI 4 and later no TCP; on ABI 6 and later no abstract Unix sockets and no signals out); seccomp (no sockets, no
//  other process, signals to itself only, no typing into a terminal, none of the kernel's other services) with the
//  exec gate: every execve and execveat in the box comes to this program as a user notification, the child's own start
//  of Blender is let through, every later one is answered EPERM, and with this program gone the kernel answers ENOSYS;
//  then every other open file closed, and Blender started with an environment built from nothing plus the --env
//  lines (MAKE_ names only). Truncation by path is refused by the filter on every kernel, as Landlock governs it only
//  from ABI 3. /dev/dri is open to the box when there is a GPU (the driver's attack surface; accepted, security 11-07).
//
//  The watcher side: the pid file as on macOS, then poll on its standard input (a line or its end), a signalfd for the
//  ending signals and SIGCHLD, the exec gate's listener, the cap's clock, and every two seconds the work folder's size
//  (security's should-fix: Blender may fill its work folder, never the disk). x86_64 only, as Blender's Linux build is.

#if !defined(__x86_64__)

int main(int, char**) {
    std::fputs("darkride-make-watch: Linux x86_64 only (Blender's Linux build).\n", stderr);
    return 125;
}

#else

#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>

namespace {

const int CANT_START = 125;
const int MAX_CAP_SECONDS = 3600;
const int ENDING_SIGNALS[] = { SIGTERM, SIGINT, SIGHUP };
// the work folder's size cap (the app passes its own with --folder-cap): a make's model is capped at 64 MB and its
// views are small, so this is far above any honest make and far below a full disk
const long long DEFAULT_FOLDER_CAP_BYTES = 2LL * 1024 * 1024 * 1024;
const long long MAX_FOLDER_CAP_BYTES = 64LL * 1024 * 1024 * 1024;
const int FOLDER_CHECK_MS = 2000;
// how long the child may take to install its filter and hand the exec gate over
const int GATE_WAIT_MS = 15000;
const rlim_t GB = 1ULL << 30;

// Landlock's interface, written out here so this builds with older kernel headers (22.04's linux-libc-dev is 5.15)
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
               NR_FSMOUNT = 432, NR_FSPICK = 433, NR_CLONE3 = 435, NR_CLOSE_RANGE = 436, NR_OPENAT2 = 437,
               NR_PIDFD_GETFD = 438, NR_PROCESS_MADVISE = 440, NR_MOUNT_SETATTR = 442, NR_QUOTACTL_FD = 443;

// what the filter does, one line per rule, printed by --filter-rules (the app's tests read it; MakeBlender keeps the
// same lines for the tests that run elsewhere)
const char* const FILTER_RULES[] = {
    "execve, execveat: the exec gate (the child's own start of Blender passes, every later one EPERM)",
    "truncate: EPERM (shortening a file by path; Landlock governs it only from ABI 3)",
    "open, openat with O_TRUNC in a read-only mode: EPERM (shortening without a write open; Landlock from ABI 3)",
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

int fail(const std::string& why) {
    std::fprintf(stderr, "darkride-make-watch: %s\n", why.c_str());
    return CANT_START;
}

bool parseCap(const char* text, int& seconds) {
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > MAX_CAP_SECONDS) {
        return false;
    }
    seconds = static_cast<int>(value);
    return true;
}

bool parseMegabytes(const char* text, long long& bytes) {
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value * 1024 * 1024 > MAX_FOLDER_CAP_BYTES) {
        return false;
    }
    bytes = value * 1024 * 1024;
    return true;
}

// whatever the app left open in this process (other than standard input, output and error) is closed, so Blender
// gets none of it
void closeOtherFiles() {
    if (syscall(NR_CLOSE_RANGE, 3U, ~0U, 0U) != 0) {
        for (int fd = 3; fd < 65536; ++fd) {
            close(fd);
        }
    }
}

bool writePid(const char* path, pid_t pid) {
    const int file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0) {
        return false;
    }
    char text[32];
    const int length = std::snprintf(text, sizeof(text), "%d\n", static_cast<int>(pid));
    const bool written = write(file, text, static_cast<size_t>(length)) == length;
    return close(file) == 0 && written;
}

int statusOf(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return CANT_START;
}

int reap(pid_t child) {
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return CANT_START;
        }
    }
    return statusOf(status);
}

std::string canonical(const std::string& path) {
    char* real = realpath(path.c_str(), nullptr);
    if (!real) {
        return std::string();
    }
    std::string out(real);
    free(real);
    return out;
}

std::string parentFolder(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
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

struct Rule {
    int fd;
    uint64_t access;
};

// everything the child needs, made before fork: after fork the child only makes system calls
struct Prepared {
    std::vector<std::string> argStrings, envStrings;
    std::vector<char*> argv, envp;
    std::string blender, work;
    std::vector<Rule> rules;
    landlock::RulesetAttr ruleset;
    size_t rulesetSize = 0;
    std::vector<sock_filter> filter;
    std::vector<size_t> selfPidSlots;  // the filter's "this process" comparisons, set in the child
    int devNull = -1, gateParent = -1, gateChild = -1, go = -1;
    pid_t parent = 0;
    rlimit cpu {}, data {}, as {}, core {}, fsize {};
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
    closeFd(p.gateChild);
    closeFd(p.gateParent);
    closeFd(p.go);
}

// Blender's environment, built from nothing: the work folder is its home, its temp and its own folders; then the
// app's --env lines (Make's own two)
std::vector<std::string> boxEnvironment(const std::string& work, const std::vector<std::string>& extra) {
    std::vector<std::string> env = { "PATH=/usr/bin:/bin", "HOME=" + work, "TMPDIR=" + work + "/tmp", "LANG=C.UTF-8",
                                     "XDG_CONFIG_HOME=" + work + "/config", "XDG_CACHE_HOME=" + work + "/cache",
                                     "XDG_DATA_HOME=" + work + "/data", "BLENDER_USER_CONFIG=" + work + "/config/blender",
                                     "BLENDER_USER_SCRIPTS=" + work + "/user-scripts",
                                     "BLENDER_USER_DATAFILES=" + work + "/datafiles",
                                     "BLENDER_USER_EXTENSIONS=" + work + "/extensions",
                                     "PYTHONDONTWRITEBYTECODE=1", "PYTHONNOUSERSITE=1" };
    env.insert(env.end(), extra.begin(), extra.end());
    return env;
}

// the folders Blender's environment names, made in the work folder before it starts
void makeWorkFolders(const std::string& work) {
    for (const char* sub : { "tmp", "config", "config/blender", "cache", "data", "user-scripts", "datafiles", "extensions" }) {
        mkdir((work + "/" + sub).c_str(), 0700);
    }
}

bool landlockRules(const std::string& blenderDir, const std::string& loader, const std::string& fixed,
                   const std::string& work, int abi, Prepared& p, std::string& error) {
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
    const uint64_t WORK = READ | WRITE_FILE | REMOVE_DIR | REMOVE_FILE | MAKE_DIR | MAKE_REG | REFER | TRUNCATE;
    struct Entry {
        std::string path;
        uint64_t access;
        bool required;
    };
    const std::vector<Entry> entries = {
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
        { blenderDir, READ | EXECUTE, true }, { loader, READ_FILE | EXECUTE, true },
        { fixed, READ, true }, { work, WORK, true } };
    for (const Entry& entry : entries) {
        const int fd = open(entry.path.c_str(), O_PATH | O_CLOEXEC);
        if (fd < 0) {
            if (entry.required) {
                error = "a required folder or file for the box is missing: " + entry.path;
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
    // shortening a file: Landlock's TRUNCATE right governs it only from ABI 3 (kernels from 6.2; Ubuntu 22.04's 5.15
    // is below), so the filter covers what that right would, on every kernel (at ABI 3 and later nothing changes in
    // effect): truncate by path refused; open and openat refused when their flags carry O_TRUNC in a read-only mode
    // (a write mode's O_TRUNC stays: Landlock limits write opens to the work folder from ABI 1, and Blender's saves
    // need it); openat2's flags are behind a pointer, so it answers ENOSYS and the C library falls back to openat.
    // ftruncate stays: it needs a file the box opened for writing
    answer(__NR_truncate, REFUSE);
    const std::vector<sock_filter> readOnlyTruncate = {
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, static_cast<uint32_t>(O_ACCMODE | O_TRUNC)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(O_TRUNC), 0, 1), BPF_STMT(BPF_RET | BPF_K, REFUSE),
        BPF_STMT(BPF_RET | BPF_K, ALLOW) };
    auto flagsThen = [&](int argument) {
        std::vector<sock_filter> body { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, argLow(argument)) };
        body.insert(body.end(), readOnlyTruncate.begin(), readOnlyTruncate.end());
        return body;
    };
    guarded(__NR_open, flagsThen(1));
    guarded(__NR_openat, flagsThen(2));
    answer(NR_OPENAT2, NOT_HERE);
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
    // no sockets of any family, Unix and netlink included
    answer(__NR_socket, NO_SOCKET);
    answer(__NR_socketpair, NO_SOCKET);
    // signals: the box's own process only
    for (long nr : { static_cast<long>(__NR_kill), static_cast<long>(__NR_tkill), static_cast<long>(__NR_tgkill),
                     static_cast<long>(__NR_rt_sigqueueinfo), static_cast<long>(__NR_rt_tgsigqueueinfo) }) {
        selfOnly(nr);
    }
    answer(NR_PIDFD_SEND_SIGNAL, REFUSE);
    // no typing into a terminal
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

// the child, between fork and exec: system calls only, nothing allocated. The exit codes are the box test's: 121
// session or parent-death signal, 122 standard files or folder, 123 limits, 124 no_new_privs, 125 Landlock, 126
// seccomp, 127 exec
[[noreturn]] void startInChild(Prepared& p) {
    for (int sig : ENDING_SIGNALS) {
        signal(sig, SIG_DFL);
    }
    signal(SIGPIPE, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    // its own session: no terminal it could type into; killed when this program dies
    if (setsid() < 0 || prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != p.parent) {
        _exit(121);
    }
    // /dev/null in; this program's output and error stay Blender's (the app reads its log there, as on macOS)
    if (dup2(p.devNull, 0) < 0 || chdir(p.work.c_str()) != 0) {
        _exit(122);
    }
    if (setrlimit(RLIMIT_CPU, &p.cpu) != 0 || setrlimit(RLIMIT_DATA, &p.data) != 0 || setrlimit(RLIMIT_AS, &p.as) != 0 ||
        setrlimit(RLIMIT_CORE, &p.core) != 0 || setrlimit(RLIMIT_FSIZE, &p.fsize) != 0) {
        _exit(123);
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        _exit(124);
    }
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
    if (syscall(landlock::RESTRICT_SELF, ruleset, 0) != 0) {
        _exit(125);
    }
    close(ruleset);
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
    // wait for the parent's go: it holds the exec gate and has written the pid file; without it, never start
    char said = 0;
    ssize_t got;
    do {
        got = read(p.go, &said, 1);
    } while (got < 0 && errno == EINTR);
    if (got != 1 || said != 'g') {
        _exit(127);
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

// the work folder's size: what its files take on the disk, links not followed, 16 levels deep; a folder deeper
// than that isn't walked and sets `tooDeep`, which counts as over the cap (what's below it isn't counted otherwise)
const int FOLDER_WALK_DEPTH = 16;

long long folderBytes(const std::string& path, int depth, bool& tooDeep) {
    long long total = 0;
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        return 0;
    }
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string child = path + "/" + name;
        struct stat info {};
        if (lstat(child.c_str(), &info) != 0) {
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            if (depth + 1 < FOLDER_WALK_DEPTH) {
                total += folderBytes(child, depth + 1, tooDeep);
            } else {
                tooDeep = true;
            }
        } else if (S_ISREG(info.st_mode)) {
            total += static_cast<long long>(info.st_blocks) * 512;
        }
    }
    closedir(dir);
    return total;
}

long long nowMs() {
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

}  // namespace

int main(int argc, char** argv) {
    // darkride-make-watch <cap> <pid file> --job <work> --fixed <fixed> [--env MAKE_NAME=VALUE]... [--folder-cap <MB>] -- <blender> ...
    if (argc == 2 && std::strcmp(argv[1], "--filter-rules") == 0) {
        for (const char* rule : FILTER_RULES) {
            std::printf("%s\n", rule);
        }
        return 0;
    }
    const char* usage = "usage: darkride-make-watch <cap seconds, 1-3600> <absolute pid file> --job <absolute work folder> "
                        "--fixed <absolute fixed scripts folder> [--env MAKE_NAME=VALUE]... [--folder-cap <megabytes>] -- "
                        "<absolute blender> ...";
    int cap = 0;
    if (argc < 8 || !parseCap(argv[1], cap) || argv[2][0] != '/') {
        return fail(usage);
    }
    const char* pidFile = argv[2];
    std::string work, fixed;
    std::vector<std::string> extraEnv;
    long long folderCap = DEFAULT_FOLDER_CAP_BYTES;
    int at = 3;
    while (at < argc && std::strcmp(argv[at], "--") != 0) {
        const std::string option = argv[at];
        if (at + 1 >= argc) {
            return fail(usage);
        }
        const char* value = argv[at + 1];
        if (option == "--job" && value[0] == '/') {
            work = value;
        } else if (option == "--fixed" && value[0] == '/') {
            fixed = value;
        } else if (option == "--env" && std::strncmp(value, "MAKE_", 5) == 0 && std::strchr(value, '=') != nullptr &&
                   std::strspn(value, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789") == static_cast<size_t>(std::strchr(value, '=') - value)) {
            extraEnv.push_back(value);  // Make's own lines only (MAKE_OUT, MAKE_KIND): no loader or Python variable ever
        } else if (option == "--folder-cap" && parseMegabytes(value, folderCap)) {
            // taken
        } else {
            return fail(usage);
        }
        at += 2;
    }
    if (at + 1 >= argc || work.empty() || fixed.empty() || argv[at + 1][0] != '/') {
        return fail(usage);
    }
    char** line = argv + at + 1;

    closeOtherFiles();
    // asked to end: Blender is killed first (the signals come through the signalfd below); SIGCHLD says Blender ended
    sigset_t watched;
    sigemptyset(&watched);
    for (int sig : ENDING_SIGNALS) {
        sigaddset(&watched, sig);
    }
    sigaddset(&watched, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &watched, nullptr) != 0) {
        return fail("can't watch the app");
    }
    signal(SIGPIPE, SIG_IGN);
    const int signals = signalfd(-1, &watched, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signals < 0) {
        return fail("can't watch the app");
    }

    // the box, prepared before fork
    const int abi = landlock::abi();
    if (abi < 1) {
        return fail("this kernel has no Landlock: Blender can't be boxed here");
    }
    Prepared p;
    p.parent = getpid();
    p.blender = canonical(line[0]);
    p.work = canonical(work);
    const std::string realFixed = canonical(fixed);
    struct stat info {};
    if (p.blender.empty() || stat(p.blender.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        access(p.blender.c_str(), X_OK) != 0) {
        return fail("Blender's program wasn't found");
    }
    if (p.work.empty() || realFixed.empty()) {
        return fail("the work or the fixed scripts folder is missing");
    }
    std::string loader = elfInterpreter(p.blender);
    if (loader.empty() || loader[0] != '/') {
        return fail("Blender's program isn't a 64-bit x86 ELF program with a loader");
    }
    loader = canonical(loader);
    if (loader.empty()) {
        return fail("Blender's loader wasn't found");
    }
    makeWorkFolders(p.work);
    for (char** word = line; *word; ++word) {
        p.argStrings.push_back(*word);
    }
    p.argStrings[0] = p.blender;
    p.envStrings = boxEnvironment(p.work, extraEnv);
    for (std::string& s : p.argStrings) {
        p.argv.push_back(&s[0]);
    }
    p.argv.push_back(nullptr);
    for (std::string& s : p.envStrings) {
        p.envp.push_back(&s[0]);
    }
    p.envp.push_back(nullptr);
    p.cpu = { static_cast<rlim_t>(cap), static_cast<rlim_t>(cap + 2) };
    p.data = { 4 * GB, 4 * GB };
    p.as = { 16 * GB, 16 * GB };
    p.core = { 0, 0 };
    p.fsize = { static_cast<rlim_t>(folderCap), static_cast<rlim_t>(folderCap) };
    p.devNull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (p.devNull < 0) {
        return fail("no /dev/null");
    }
    std::string error;
    if (!landlockRules(parentFolder(p.blender), loader, realFixed, p.work, abi, p, error)) {
        closePrepared(p);
        return fail(error);
    }
    p.filter = seccompFilter(p.selfPidSlots);
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        closePrepared(p);
        return fail("no socket pair for the exec gate");
    }
    p.gateParent = pair[0];
    p.gateChild = pair[1];
    int go[2];
    if (pipe2(go, O_CLOEXEC) != 0) {
        closePrepared(p);
        return fail("no pipe");
    }
    p.go = go[0];

    const pid_t child = fork();
    if (child < 0) {
        closePrepared(p);
        close(go[1]);
        return fail("can't start Blender");
    }
    if (child == 0) {
        close(go[1]);
        startInChild(p);
    }
    closeFd(p.gateChild);
    closeFd(p.go);
    // the exec gate arrives once the child's filter is in; then its id is written, then it may start
    int gate = receiveFd(p.gateParent, GATE_WAIT_MS);
    if (gate < 0 || !writePid(pidFile, child) || write(go[1], "g", 1) != 1) {
        kill(child, SIGKILL);
        close(go[1]);
        closePrepared(p);
        reap(child);
        return fail("can't watch Blender");
    }
    close(go[1]);
    closePrepared(p);

    const long long deadline = nowMs() + static_cast<long long>(cap) * 1000;
    long long nextFolderCheck = nowMs() + FOLDER_CHECK_MS;
    bool killed = false;
    bool exited = false;
    int status = 0;
    bool reaped = false;
    int gateAllowed = 0;
    bool stdinOpen = true;
    const auto end = [&](const char* why) {
        // only while Blender is still our unreaped child: the id is Blender's then, and never another process's
        if (!killed && !reaped) {
            killed = true;
            if (why) {
                std::fprintf(stderr, "darkride-make-watch: %s\n", why);
            }
            kill(child, SIGKILL);
        }
    };
    while (!exited) {
        pollfd watch[3] = { { signals, POLLIN, 0 }, { stdinOpen ? STDIN_FILENO : -1, POLLIN, 0 }, { gate, POLLIN, 0 } };
        const long long now = nowMs();
        long long wait = deadline - now;
        if (nextFolderCheck - now < wait) {
            wait = nextFolderCheck - now;
        }
        const int count = poll(watch, 3, wait < 0 ? 0 : static_cast<int>(wait > 60000 ? 60000 : wait));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            end("can't watch the app");
            break;
        }
        if (watch[2].revents & POLLIN) {
            // the exec gate: the child's own start of Blender is let through once; every later exec is refused
            seccomp_notif request;
            std::memset(&request, 0, sizeof request);
            if (ioctl(gate, SECCOMP_IOCTL_NOTIF_RECV, &request) == 0) {
                seccomp_notif_resp response;
                std::memset(&response, 0, sizeof response);
                response.id = request.id;
                if (gateAllowed == 0 && static_cast<pid_t>(request.pid) == child) {
                    response.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
                    ++gateAllowed;
                } else {
                    response.error = -EPERM;
                }
                ioctl(gate, SECCOMP_IOCTL_NOTIF_SEND, &response);  // ENOENT: the caller is gone meanwhile
            }
        } else if (watch[2].revents & (POLLHUP | POLLERR)) {
            close(gate);  // nothing is left under the filter
            gate = -1;
        }
        if (stdinOpen && (watch[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            // a line ("stop") or the end of standard input (the app is gone): Blender ends
            char bytes[256];
            const ssize_t length = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (length <= 0 || std::memchr(bytes, '\n', static_cast<size_t>(length)) != nullptr) {
                end(nullptr);
                stdinOpen = false;
            }
        }
        if (watch[0].revents & POLLIN) {
            signalfd_siginfo got;
            while (read(signals, &got, sizeof got) == static_cast<ssize_t>(sizeof got)) {
                if (got.ssi_signo == SIGCHLD) {
                    if (waitpid(child, &status, WNOHANG) == child) {
                        exited = true;
                        reaped = true;
                    }
                } else {
                    end(nullptr);
                }
            }
        }
        const long long after = nowMs();
        if (after >= deadline) {
            end("Blender ran to the cap and was ended");
            nextFolderCheck = after + FOLDER_CHECK_MS;
        } else if (after >= nextFolderCheck) {
            nextFolderCheck = after + FOLDER_CHECK_MS;
            bool tooDeep = false;
            const long long bytes = folderBytes(p.work, 0, tooDeep);
            if (tooDeep) {
                end("Blender's work folder grew deeper than the size check walks, which counts as over the cap; Blender was ended");
            } else if (bytes > folderCap) {
                end("Blender's work folder passed its size cap and Blender was ended");
            }
        }
    }
    if (gate >= 0) {
        close(gate);
    }
    return reaped ? statusOf(status) : reap(child);
}

#endif  // __x86_64__

#else

#include <fcntl.h>
#include <libproc.h>
#include <sys/event.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

const int CANT_START = 125;
const int MAX_CAP_SECONDS = 3600;
// the one program it starts: the box (sandbox-exec, which becomes Blender in the same process)
const char* PROGRAM = "/usr/bin/sandbox-exec";
const int ENDING_SIGNALS[] = { SIGTERM, SIGINT, SIGHUP };

int fail(const std::string& why) {
    std::fprintf(stderr, "darkride-make-watch: %s\n", why.c_str());
    return CANT_START;
}

bool parseCap(const char* text, int& seconds) {
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > MAX_CAP_SECONDS) {
        return false;
    }
    seconds = static_cast<int>(value);
    return true;
}

// whatever the app left open in this process (other than standard input, output and error) is closed, so Blender
// gets none of it
void closeOtherFiles() {
    const int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes <= 0) {
        return;
    }
    std::vector<proc_fdinfo> files(static_cast<size_t>(bytes) / sizeof(proc_fdinfo) + 16);
    const int got = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, files.data(),
                                 static_cast<int>(files.size() * sizeof(proc_fdinfo)));
    if (got <= 0) {
        return;
    }
    const size_t count = static_cast<size_t>(got) / sizeof(proc_fdinfo);
    for (size_t i = 0; i < count; ++i) {
        if (files[i].proc_fd > 2) {
            close(files[i].proc_fd);
        }
    }
}

bool writePid(const char* path, pid_t pid) {
    const int file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0) {
        return false;
    }
    char text[32];
    const int length = std::snprintf(text, sizeof(text), "%d\n", static_cast<int>(pid));
    const bool written = write(file, text, static_cast<size_t>(length)) == length;
    return close(file) == 0 && written;
}

int statusOf(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return CANT_START;
}

int reap(pid_t child) {
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return CANT_START;
        }
    }
    return statusOf(status);
}

// the child, between fork and exec: only async-signal-safe calls
[[noreturn]] void startBlender(int go, char** line) {
    for (int sig : ENDING_SIGNALS) {
        signal(sig, SIG_DFL);
    }
    signal(SIGPIPE, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    const int null = open("/dev/null", O_RDONLY);
    if (null < 0 || dup2(null, STDIN_FILENO) < 0) {
        _exit(127);
    }
    if (null != STDIN_FILENO) {
        close(null);
    }
    // wait for the parent's go: it is watching this process and has written the pid file; without it, never start
    char said = 0;
    ssize_t got;
    do {
        got = read(go, &said, 1);
    } while (got < 0 && errno == EINTR);
    if (got != 1 || said != 'g') {
        _exit(127);
    }
    execv(PROGRAM, line);
    _exit(127);
}

}  // namespace

int main(int argc, char** argv) {
    // darkride-make-watch <cap> <pid file> -- /usr/bin/sandbox-exec ...
    int cap = 0;
    if (argc < 5 || !parseCap(argv[1], cap) || argv[2][0] != '/' || std::strcmp(argv[3], "--") != 0 ||
        std::strcmp(argv[4], PROGRAM) != 0) {
        return fail("usage: darkride-make-watch <cap seconds, 1-3600> <absolute pid file> -- /usr/bin/sandbox-exec ...");
    }
    const char* pidFile = argv[2];
    char** line = argv + 4;

    closeOtherFiles();
    // asked to end: Blender is killed first (the signals come through the queue below)
    for (int sig : ENDING_SIGNALS) {
        signal(sig, SIG_IGN);
    }
    signal(SIGPIPE, SIG_IGN);

    const int queue = kqueue();
    if (queue < 0) {
        return fail("no kqueue");
    }
    fcntl(queue, F_SETFD, FD_CLOEXEC);
    std::vector<struct kevent> watch;
    for (int sig : ENDING_SIGNALS) {
        struct kevent event;
        EV_SET(&event, sig, EVFILT_SIGNAL, EV_ADD, 0, 0, nullptr);
        watch.push_back(event);
    }
    struct kevent event;
    EV_SET(&event, STDIN_FILENO, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    watch.push_back(event);
    EV_SET(&event, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_SECONDS, cap, nullptr);
    watch.push_back(event);
    if (kevent(queue, watch.data(), static_cast<int>(watch.size()), nullptr, 0, nullptr) < 0) {
        return fail("can't watch the app");
    }

    int go[2];
    if (pipe(go) != 0) {
        return fail("no pipe");
    }
    fcntl(go[0], F_SETFD, FD_CLOEXEC);
    fcntl(go[1], F_SETFD, FD_CLOEXEC);
    const pid_t child = fork();
    if (child < 0) {
        return fail("can't start Blender");
    }
    if (child == 0) {
        close(go[1]);
        startBlender(go[0], line);
    }
    close(go[0]);

    // watch the child's exit (this doesn't reap it), then write its id, then let it start
    EV_SET(&event, child, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    if (kevent(queue, &event, 1, nullptr, 0, nullptr) < 0 || !writePid(pidFile, child) || write(go[1], "g", 1) != 1) {
        kill(child, SIGKILL);
        close(go[1]);
        reap(child);
        return fail("can't watch Blender");
    }
    close(go[1]);

    bool killed = false;
    bool exited = false;
    const auto end = [&] {
        if (!killed) {
            killed = true;
            kill(child, SIGKILL);  // still our unreaped child: the id is Blender's
        }
    };
    while (!exited) {
        struct kevent got[8];
        const int count = kevent(queue, nullptr, 0, got, 8, nullptr);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            end();
            break;
        }
        for (int i = 0; i < count; ++i) {
            switch (got[i].filter) {
                case EVFILT_PROC:
                    exited = true;
                    break;
                case EVFILT_READ: {
                    // a line ("stop") or the end of standard input (the app is gone): Blender ends
                    char bytes[256];
                    const ssize_t length = read(STDIN_FILENO, bytes, sizeof(bytes));
                    if (length <= 0 || std::memchr(bytes, '\n', static_cast<size_t>(length)) != nullptr) {
                        end();
                        struct kevent stop;
                        EV_SET(&stop, STDIN_FILENO, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
                        kevent(queue, &stop, 1, nullptr, 0, nullptr);
                    }
                    break;
                }
                case EVFILT_TIMER:
                case EVFILT_SIGNAL:
                    end();
                    break;
                default:
                    break;
            }
        }
    }
    return reap(child);
}

#endif
