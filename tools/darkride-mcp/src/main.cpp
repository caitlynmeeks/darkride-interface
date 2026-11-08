//
//  main.cpp
//  tools/darkride-mcp/src
//
//  darkride-mcp: the helper the builder's own Claude Code starts for Make, as an MCP server over its standard input
//  and output (MCP's stdio transport). It carries those bytes, unchanged, to and from the running Darkride app over a
//  local socket only the same user can open: no network port, so no web page, world script or other computer can
//  reach Make. All of MCP (the tools, their checks, the person's rights) is in the app; this only passes bytes.
//
//    darkride-mcp --socket <path>     (the line the app writes into the Make folder's MCP configuration; on
//                                     Windows the path is Make's named pipe, \\.\pipe\darkride-make-<id>)
//
//  Before it connects it checks that the socket and its folder belong to this user and that nobody else may open
//  them; after, that the app at the other end runs as this user. The app checks the same of the helper.
//  Exits 0 when either side closes, 1 when it can't connect (the reason on standard error, which Claude Code shows).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include <cerrno>
#include <chrono>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32

// Windows: Make's link is a named pipe (security's make-blender-box-windows-linux, "the link"). The name must be one
// of Make's on this computer (never a remote pipe), the app at the other end must run as this user, unboxed, and the
// app may only identify this helper, never act as it (SECURITY_IDENTIFICATION).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vector>

namespace {

const char* NOT_CONNECTED =
    "darkride-mcp: Darkride isn't ready for Make. Open Darkride, press Connect in Make, then start this again.\n";
const auto CONNECT_WAIT = std::chrono::seconds(5);
const std::wstring PIPE_PREFIX = L"\\\\.\\pipe\\darkride-make-";

void fail(const std::string& why) {
    std::fprintf(stderr, "darkride-mcp: %s\n", why.c_str());
}

// a process's user SID (bytes), and whether it runs in an AppContainer
bool userOf(HANDLE process, std::vector<char>& sid, bool& boxed) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<char> user(size);
    DWORD inContainer = 0;
    DWORD containerSize = sizeof inContainer;
    const bool read = size > 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size) &&
                      GetTokenInformation(token, TokenIsAppContainer, &inContainer, containerSize, &containerSize);
    CloseHandle(token);
    if (!read) {
        return false;
    }
    PSID userSid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
    const char* bytes = reinterpret_cast<const char*>(userSid);
    sid.assign(bytes, bytes + GetLengthSid(userSid));
    boxed = inContainer != 0;
    return true;
}

// `refused`: the pipe opened but isn't this user's own Darkride (no retry then)
HANDLE connectTo(const std::wstring& name, bool& refused) {
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return pipe;
    }
    // the app at the other end runs as this user, unboxed
    ULONG pid = 0;
    std::vector<char> theirs, ours;
    bool theirsBoxed = true, oursBoxed = true;
    HANDLE server = GetNamedPipeServerProcessId(pipe, &pid) ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)
                                                            : nullptr;
    const bool known = server && userOf(server, theirs, theirsBoxed) && userOf(GetCurrentProcess(), ours, oursBoxed);
    if (server) {
        CloseHandle(server);
    }
    if (!known || theirs != ours || theirsBoxed) {
        fail("the other end of the pipe isn't this user's own Darkride; not connecting");
        CloseHandle(pipe);
        refused = true;
        return INVALID_HANDLE_VALUE;
    }
    return pipe;
}

// one overlapped transfer on the pipe: true with the bytes moved, false when it closed or failed
bool onPipe(bool reading, HANDLE pipe, char* data, DWORD size, DWORD& moved) {
    OVERLAPPED overlapped {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        return false;
    }
    const BOOL started = reading ? ReadFile(pipe, data, size, nullptr, &overlapped)
                                 : WriteFile(pipe, data, size, nullptr, &overlapped);
    const bool ok = (started || GetLastError() == ERROR_IO_PENDING) && GetOverlappedResult(pipe, &overlapped, &moved, TRUE);
    CloseHandle(overlapped.hEvent);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0) {
            path = argv[i + 1];
        }
    }
    const std::wstring name(path.begin(), path.end());
    // only Make's own pipes on this computer: \\.\pipe\darkride-make-<32 hex>
    bool shaped = name.size() == PIPE_PREFIX.size() + 32 && name.compare(0, PIPE_PREFIX.size(), PIPE_PREFIX) == 0;
    for (size_t i = PIPE_PREFIX.size(); shaped && i < name.size(); ++i) {
        shaped = std::iswxdigit(name[i]) != 0;
    }
    if (!shaped) {
        std::fputs("usage: darkride-mcp --socket \\\\.\\pipe\\darkride-make-<id>  (Darkride's Make writes this line for "
                   "Claude Code)\n", stderr);
        return 1;
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto giveUp = std::chrono::steady_clock::now() + CONNECT_WAIT;
    while (pipe == INVALID_HANDLE_VALUE) {
        bool refused = false;
        pipe = connectTo(name, refused);
        const DWORD error = GetLastError();
        if (refused) {
            return 1;
        }
        if (pipe != INVALID_HANDLE_VALUE || std::chrono::steady_clock::now() > giveUp) {
            break;
        }
        if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(name.c_str(), 200);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));  // not there yet: Connect may be starting it
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        std::fputs(NOT_CONNECTED, stderr);
        return 1;
    }

    // both ways until either side closes; the process ends with the first to close
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread toApp([pipe, closed] {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        std::vector<char> buffer(65536);
        DWORD got = 0;
        while (ReadFile(in, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) && got > 0) {
            for (DWORD sent = 0, moved = 0; sent < got; sent += moved) {
                if (!onPipe(false, pipe, buffer.data() + sent, got - sent, moved) || moved == 0) {
                    SetEvent(closed);
                    return;
                }
            }
        }
        SetEvent(closed);  // Claude Code closed: tell the app by closing
    });
    std::thread fromApp([pipe, closed] {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        std::vector<char> buffer(65536);
        DWORD got = 0;
        while (onPipe(true, pipe, buffer.data(), static_cast<DWORD>(buffer.size()), got) && got > 0) {
            for (DWORD written = 0, moved = 0; written < got; written += moved) {
                if (!WriteFile(out, buffer.data() + written, got - written, &moved, nullptr) || moved == 0) {
                    SetEvent(closed);
                    return;
                }
            }
        }
        SetEvent(closed);  // the app closed (Disconnect, or it quit)
    });
    toApp.detach();
    fromApp.detach();
    WaitForSingleObject(closed, INFINITE);
    CloseHandle(pipe);
    return 0;
}

#else

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

const char* NOT_CONNECTED =
    "darkride-mcp: Darkride isn't ready for Make. Open Darkride, press Connect in Make, then start this again.\n";
// how long to wait for the app's socket (it may be starting it as this starts)
const auto CONNECT_WAIT = std::chrono::seconds(5);

void fail(const std::string& why) {
    std::fprintf(stderr, "darkride-mcp: %s\n", why.c_str());
}

enum class Place { Ready, NotYet, Unsafe };

// the socket and its folder: this user's, nobody else's
Place safePlace(const std::string& path) {
    const auto slash = path.rfind('/');
    if (path.empty() || path[0] != '/' || slash == std::string::npos || slash == 0) {
        fail("the socket path must be absolute");
        return Place::Unsafe;
    }
    const std::string folder = path.substr(0, slash);
    struct stat info;
    if (lstat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return Place::NotYet;  // not there yet: the app hasn't started the link
    }
    if (info.st_uid != getuid() || (info.st_mode & 077) != 0) {
        fail("the socket's folder is not this user's own (it must be mode 0700); not connecting");
        return Place::Unsafe;
    }
    if (lstat(path.c_str(), &info) != 0) {
        return Place::NotYet;
    }
    if (!S_ISSOCK(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077) != 0) {
        fail("the socket is not this user's own (it must be mode 0600); not connecting");
        return Place::Unsafe;
    }
    return Place::Ready;
}

// the user at the other end of a local socket: SO_PEERCRED on Linux, getpeereid on macOS and the BSDs
bool peerIsThisUser(int fd) {
#if defined(__linux__)
    struct ucred credentials;
    socklen_t length = sizeof credentials;
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 && credentials.uid == getuid();
#else
    uid_t uid = 0;
    gid_t gid = 0;
    return getpeereid(fd, &uid, &gid) == 0 && uid == getuid();
#endif
}

int connectTo(const std::string& path) {
    sockaddr_un address;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        fail("the socket path is too long");
        return -1;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size());
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    // the app at the other end runs as this user
    if (!peerIsThisUser(fd)) {
        fail("the other end of the socket isn't this user's; not connecting");
        close(fd);
        return -1;
    }
    return fd;
}

bool writeAll(int fd, const char* data, ssize_t size) {
    while (size > 0) {
        const ssize_t written = write(fd, data, static_cast<size_t>(size));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0) {
            path = argv[i + 1];
        }
    }
    if (path.empty()) {
        std::fputs("usage: darkride-mcp --socket <path>  (Darkride's Make writes this line for Claude Code)\n", stderr);
        return 1;
    }
    std::signal(SIGPIPE, SIG_IGN);

    int fd = -1;
    const auto giveUp = std::chrono::steady_clock::now() + CONNECT_WAIT;
    while (fd < 0) {
        const Place place = safePlace(path);
        if (place == Place::Unsafe) {
            return 1;
        }
        if (place == Place::Ready) {
            fd = connectTo(path);
        }
        if (fd >= 0 || std::chrono::steady_clock::now() > giveUp) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (fd < 0) {
        std::fputs(NOT_CONNECTED, stderr);
        return 1;
    }

    // both ways until either side closes
    pollfd ends[2] = { { STDIN_FILENO, POLLIN, 0 }, { fd, POLLIN, 0 } };
    char buffer[65536];
    while (true) {
        if (poll(ends, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ends[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            const ssize_t got = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (got <= 0 && !(got < 0 && errno == EINTR)) {
                break;  // Claude Code closed: tell the app by closing
            }
            if (got > 0 && !writeAll(fd, buffer, got)) {
                break;
            }
        }
        if (ends[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            const ssize_t got = read(fd, buffer, sizeof(buffer));
            if (got <= 0 && !(got < 0 && errno == EINTR)) {
                break;  // the app closed (Disconnect, or it quit)
            }
            if (got > 0 && !writeAll(STDOUT_FILENO, buffer, got)) {
                break;
            }
        }
    }
    close(fd);
    return 0;
}

#endif
