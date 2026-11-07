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
//  macOS only (kqueue); on Linux, Blender's box is to use PR_SET_PDEATHSIG instead.
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

#ifndef __APPLE__

int main(int, char**) {
    std::fputs("darkride-make-watch: macOS only.\n", stderr);
    return 125;
}

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
