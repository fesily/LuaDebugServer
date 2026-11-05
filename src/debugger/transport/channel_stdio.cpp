#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>

#include "transport/channel.h"

#if defined(_WIN32)
#    include <fcntl.h>
#    include <io.h>
#    include <windows.h>
#else
#    include <poll.h>
#    include <unistd.h>
#endif

namespace luadebug::debugger::transport {

    namespace {

        class StdioChannel final : public Channel {
        public:
            void set_on_close(std::function<void()> callback) override {
                on_close_ = std::move(callback);
            }
            StdioChannel() {
#if defined(_WIN32)
                _setmode(_fileno(stdin), _O_BINARY);
                _setmode(_fileno(stdout), _O_BINARY);
#endif
            }

            ~StdioChannel() override = default;

            void send(const Json& message) override {
                std::string frame = encode_frame(message);
                std::fwrite(frame.data(), 1, frame.size(), stdout);
                std::fflush(stdout);
                if (debug) {
                    std::fprintf(stderr, "[send] %s\n", dump_json(message).c_str());
                }
            }

            void flush() override {
                // Writes are immediate on this channel.
                std::fflush(stdout);
            }

            bool recv(Json& out, std::string& error) override {
                if (!protocol_error_.empty()) {
                    error = protocol_error_;
                    protocol_error_.clear();
                    return false;
                }
                if (inbox_.empty()) {
                    return false;
                }
                out = std::move(inbox_.front());
                inbox_.pop_front();
                return true;
            }

            void update(int /*timeout_ms*/) override {
                // Non-blocking drain: same effect as subprocess.peek(stdin) in
                // stdio.lua, which never blocks the adapter loop. A raw read()
                // is required: stdio's fread() keeps waiting for a full buffer,
                // which would stall the adapter between frames.
                std::array<char, 8192> buffer {};
                for (;;) {
                    if (!readable()) {
                        return;
                    }
                    auto n = read_stdin(buffer.data(), buffer.size());
                    if (n == 0) {
                        closed_ = true;
                        if (on_close_) {
                            auto fn = on_close_;
                            fn();
                        }
                        return;
                    }
                    if (n < 0) {
                        return;
                    }
                    feed(buffer.data(), static_cast<std::size_t>(n));
                    if (closed_) {
                        return;
                    }
                    if (static_cast<std::size_t>(n) < buffer.size()) {
                        return;
                    }
                }
            }

            bool connected() const override {
                // stdin/stdout is connected for the lifetime of the adapter.
                return true;
            }

            bool closed() const override {
                return closed_;
            }

            void close() override {
                closed_ = true;
            }

        private:
            static long read_stdin(char* buffer, std::size_t size) {
#if defined(_WIN32)
                DWORD n = 0;
                if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, static_cast<DWORD>(size), &n, nullptr)) {
                    return 0;
                }
                return static_cast<long>(n);
#else
                auto n = ::read(0, buffer, size);
                if (n == 0) {
                    return 0;
                }
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        return -1;
                    }
                    return 0;
                }
                return static_cast<long>(n);
#endif
            }

            bool readable() {
#if defined(_WIN32)
                HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
                if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
                    return false;
                }
                DWORD available = 0;
                if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
                    // Not a pipe (e.g. console): assume input is waiting.
                    return true;
                }
                return available > 0;
#else
                pollfd p {};
                p.fd     = 0;
                p.events = POLLIN;
                int rc   = ::poll(&p, 1, 0);
                if (rc <= 0) {
                    return false;
                }
                return (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
#endif
            }

            void feed(const char* data, std::size_t size) {
                Json message;
                std::string error;
                auto status = decoder_.push(data, size, message, error);
                while (status == FrameStatus::ok) {
                    if (debug) {
                        std::fprintf(stderr, "[recv] %s\n", dump_json(message).c_str());
                    }
                    inbox_.push_back(std::move(message));
                    status = decoder_.next(message, error);
                }
                if (status == FrameStatus::invalid) {
                    protocol_error_ = error.empty() ? "Invalid protocol." : error;
                    closed_         = true;
                    if (on_close_) {
                        auto fn = on_close_;
                        fn();
                    }
                }
            }

            FrameDecoder decoder_;
            std::deque<Json> inbox_;
            std::string protocol_error_;
            std::function<void()> on_close_;
            bool closed_ = false;
        };

    }  // namespace

    std::unique_ptr<Channel> open_stdio_channel(std::string& error) {
        (void)error;
        return std::make_unique<StdioChannel>();
    }

}  // namespace luadebug::debugger::transport
