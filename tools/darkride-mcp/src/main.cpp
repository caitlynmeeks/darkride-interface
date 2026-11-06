//
//  main.cpp
//  tools/darkride-mcp/src
//
//  darkride-mcp: the helper the builder's own Claude Code starts for Make, as an MCP server over its standard input
//  and output (MCP's stdio transport). It carries those bytes, unchanged, to and from the running Darkride app over a
//  local socket only the same user can open: no network port, so no web page, world script or other computer can
//  reach Make. All of MCP (the tools, their checks, the person's rights) is in the app; this only passes bytes.
//
//    darkride-mcp --socket <path>     (the line the app writes into the Make folder's MCP configuration)
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
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32

int main(int, char**) {
    std::fputs("darkride-mcp: Make's link isn't on Windows yet.\n", stderr);
    return 1;
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
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(fd, &uid, &gid) != 0 || uid != getuid()) {
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
