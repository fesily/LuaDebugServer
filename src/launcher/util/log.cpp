#include <autoattach/ctx.h>
#include <common/path_utf8.h>
#include <fmt/format.h>
#include <host/platform_abi.h>
#include <util/log.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

#if !defined(_WIN32)
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <unistd.h>
#else
#    include <winsock2.h>
#    include <ws2tcpip.h>
// The AF_UNIX provider only exists on recent Windows builds; the address family
// value is fixed even when the SDK predates <afunix.h>.
#    if !defined(AF_UNIX)
#        define AF_UNIX 1
#    endif
#    define UNIX_PATH_MAX 108
struct sockaddr_un {
    unsigned short sun_family;
    char sun_path[UNIX_PATH_MAX];
};
#endif

namespace luadebug::log {
    namespace fs = debugger::fs;

    namespace {

#if defined(_WIN32)
        bool socket_ready(SOCKET socket, bool for_write) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(socket, &set);
            TIMEVAL none { 0, 0 };
            return ::select(0, for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr, &none) > 0;
        }
        SOCKET open_unix_socket() {
            return ::socket(AF_UNIX, SOCK_STREAM, 0);
        }
        void close_socket(SOCKET socket) {
            ::closesocket(socket);
        }
        bool is_valid(SOCKET socket) {
            return socket != INVALID_SOCKET;
        }
#else
        int nfds(int fd) {
            return fd + 1;
        }
        bool socket_ready(int fd, bool for_write) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd, &set);
            timeval none { 0, 0 };
            return ::select(nfds(fd), for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr, &none) > 0;
        }
        int open_unix_socket() {
            return ::socket(AF_UNIX, SOCK_STREAM, 0);
        }
        void close_socket(int fd) {
            ::close(fd);
        }
        bool is_valid(int fd) {
            return fd >= 0;
        }
#endif

        // Non-blocking, best effort: the report is sent only when the frontend has
        // already connected, and it never blocks the crashing thread.
        bool send_all(decltype(open_unix_socket()) socket, const char* data, int length) {
            while (length > 0) {
                if (!socket_ready(socket, true)) {
                    return false;
                }
#if defined(_WIN32)
                const int written = ::send(socket, data, length, 0);
#else
                const int written = static_cast<int>(::send(socket, data, static_cast<std::size_t>(length), 0));
#endif
                if (written <= 0) {
                    return false;
                }
                data += written;
                length -= written;
            }
            return true;
        }

        bool bind_unix_socket(decltype(open_unix_socket()) socket, const std::string& path) {
            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            if (path.empty() || path.size() >= sizeof(address.sun_path)) {
                return false;
            }
            std::copy(path.begin(), path.end(), address.sun_path);
            return ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
        }

        void remove_stale_path(const std::string& path) {
#if defined(_WIN32)
            // The path is UTF-8; the ANSI form would not name the same file.
            ::DeleteFileW(debugger::utf8_to_wide(path).c_str());
#else
            ::unlink(path.c_str());
#endif
        }

        // Immutable once published; the pointer swap keeps readers lock-free so a
        // fatal report can never deadlock against the launcher's context mutex.
        std::atomic<const std::string*> published_root { nullptr };

    }  // namespace

    void set_root(const std::string& root) {
        if (root.empty()) {
            return;
        }
        auto previous = published_root.load(std::memory_order_acquire);
        if (previous && *previous == root) {
            return;
        }
        published_root.store(new std::string(root), std::memory_order_release);
    }

    void notify_frontend(const std::string& msg) {
        // The published root comes from the bootstrap config when the launcher was
        // injected (a frida-mapped image has no dyld path); dlopen-based hosts fall
        // back to their own DSO path.
        fs::path rootpath;
        if (auto* stored = published_root.load(std::memory_order_acquire)) {
            rootpath = debugger::path_from_utf8(*stored);
        }
        if (rootpath.empty()) {
            // The launcher lives in `<root>/bin`, so the module directory is one
            // level down from the published root.
            auto module_dir = debugger::runtime::platform_module_directory(
                reinterpret_cast<const void*>(&notify_frontend)
            );
            if (module_dir.empty()) {
                return;
            }
            rootpath = debugger::path_from_utf8(module_dir).parent_path();
        }
#if defined(_WIN32)
        const std::string pid = std::to_string(::GetCurrentProcessId());
#else
        const std::string pid = std::to_string(::getpid());
#endif
        const std::string path = debugger::path_to_utf8(rootpath / "tmp" / ("pid_" + pid));

        const auto socket = open_unix_socket();
        if (!is_valid(socket)) {
            return;
        }
        remove_stale_path(path);
        if (!bind_unix_socket(socket, path) || ::listen(socket, 5) != 0) {
            close_socket(socket);
            return;
        }
        if (!socket_ready(socket, false)) {
            close_socket(socket);
            return;
        }
        const auto peer = ::accept(socket, nullptr, nullptr);
        close_socket(socket);
        if (!is_valid(peer)) {
            return;
        }

        std::string json = fmt::format(
            R"({{"type":"response","seq":0,"command":"{}","request_seq":2,"success":false,"message":"{}"}})",
            autoattach::ctx::get()->mode.load() == autoattach::work_mode::attach ? "attach" : "launch",
            msg
        );
        const std::string frame = fmt::format("Content-Length: {}\r\n\r\n{}", json.size(), json);
        send_all(peer, frame.data(), static_cast<int>(frame.size()));
        close_socket(peer);
    }
}  // namespace luadebug::log
