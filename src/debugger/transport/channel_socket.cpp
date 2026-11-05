// Socket channel: the DAP byte stream between the adapter and the target-side
// session, over TCP, TCP6 or a Unix domain socket.
//
// The previous implementation drove a bee `bpoll` reactor with callback state
// machines. This one keeps the same observable contract and expresses the I/O as
// `boost::asio::awaitable` coroutines on the process-wide Reactor context:
//
//   connect_loop  retries a failed connection once per update round, and only
//                 until the first session is established
//   accept_loop   accepts one debuggee at a time; extra connections are closed
//   read_loop     async_read_some into the existing FrameDecoder
//   write_loop    one writer per socket, async_write of the queue front
//
// Lifetime rule: the coroutines take a `shared_ptr<State>` by value. `State` never
// owns the facade - the facade's close callback is stored as a plain function and
// cleared before any pending operation can complete - so a channel cannot be kept
// alive (or touched after destruction) by its own I/O.

#include <boost/asio/async_result.hpp>
#include <boost/asio/basic_socket_acceptor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/generic/stream_protocol.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include "transport/channel.h"

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
#    include <boost/asio/local/stream_protocol.hpp>
#endif

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/log.h"
#include "transport/reactor.h"

#if defined(_WIN32)
#    if defined(_MSC_VER)
#        pragma warning(push)
#        pragma warning(disable : 4996)
#    endif
#    include <WinSock2.h>
#    include <ws2tcpip.h>
#    if defined(_MSC_VER)
#        pragma warning(pop)
#    endif
#else
#    include <sys/stat.h>
#    include <sys/un.h>
#    include <unistd.h>

#    include <cerrno>
#endif

namespace luadebug::debugger::transport {

    namespace {

        namespace asio = boost::asio;

        using stream_socket   = asio::generic::stream_protocol::socket;
        using stream_acceptor = asio::basic_socket_acceptor<asio::generic::stream_protocol>;

        std::string collapse_temp_directory() {
            std::string text;
#if defined(_WIN32)
            char buffer[MAX_PATH * 2];
            DWORD size = ::GetTempPathA(static_cast<DWORD>(sizeof(buffer)), buffer);
            if (size == 0 || size >= sizeof(buffer)) {
                return {};
            }
            text.assign(buffer, size);
#else
            const char* env = std::getenv("TMPDIR");
            if (!env || !*env) {
                env = "/tmp";
            }
            text.assign(env);
#endif
            while (!text.empty() && (text.back() == '/' || text.back() == '\\')) {
                text.pop_back();
            }
            return text;
        }

        void remove_file(const std::string& path) {
            std::remove(path.c_str());
        }

        void replace_all(std::string& subject, std::string_view from, std::string_view to) {
            if (from.empty()) {
                return;
            }
            std::size_t pos = 0;
            while ((pos = subject.find(from, pos)) != std::string::npos) {
                subject.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        bool is_ipv4_literal(std::string_view text) {
            int dots = 0;
            std::string_view part;
            for (std::size_t i = 0; i <= text.size(); ++i) {
                if (i == text.size() || text[i] == '.') {
                    if (part.empty() || part.size() > 3) {
                        return false;
                    }
                    for (char c : part) {
                        if (c < '0' || c > '9') {
                            return false;
                        }
                    }
                    part = std::string_view();
                    ++dots;
                } else {
                    if (part.empty()) {
                        part = text.substr(i);
                    }
                }
            }
            return dots == 4;
        }

        bool is_ipv6_literal(std::string_view text) {
            bool has_colon = false;
            for (char c : text) {
                if (c == ':') {
                    has_colon = true;
                    continue;
                }
                const bool hexish = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (!hexish) {
                    return false;
                }
            }
            return has_colon;
        }

#if defined(_WIN32)
        // Windows only gained AF_UNIX in 1803; when the provider is missing the
        // address file carries a localhost TCP port instead (see the emulation
        // helpers below). The probe is the only part of that protocol that asks
        // the OS, so it is ported as-is.
        bool unix_domain_sockets_supported() {
            static const bool supported = []() {
                DWORD length = 0;
                ::WSAEnumProtocolsW(0, nullptr, &length);
                std::vector<char> buffer(length ? length : sizeof(WSAPROTOCOL_INFOW));
                auto* protocols = reinterpret_cast<LPWSAPROTOCOL_INFOW>(buffer.data());
                const int count = ::WSAEnumProtocolsW(0, protocols, &length);
                if (count == SOCKET_ERROR) {
                    return false;
                }
                static const GUID kUnixProvider = { 0xA00943D9, 0x9C2E, 0x4633, { 0x9B, 0x59, 0x00, 0x57, 0xA3, 0x16, 0x09, 0x94 } };
                for (int i = 0; i < count; ++i) {
                    if (protocols[i].iAddressFamily != AF_UNIX) {
                        continue;
                    }
                    if (!::IsEqualGUID(protocols[i].ProviderId, kUnixProvider)) {
                        continue;
                    }
                    const SOCKET probe = ::WSASocketW(PF_UNIX, SOCK_STREAM, 0, &protocols[i], 0, WSA_FLAG_NO_HANDLE_INHERIT);
                    if (probe == INVALID_SOCKET) {
                        return false;
                    }
                    ::closesocket(probe);
                    return true;
                }
                return false;
            }();
            return supported;
        }

        std::string read_port_file(const std::string& path) {
            FILE* file = std::fopen(path.c_str(), "rb");
            if (!file) {
                return {};
            }
            std::string text;
            char buffer[32];
            std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
            std::fclose(file);
            text.assign(buffer, read);
            return text;
        }

        bool write_port_file(const std::string& path, std::uint16_t port) {
            FILE* file = std::fopen(path.c_str(), "wb");
            if (!file) {
                return false;
            }
            char buffer[8];
            const int length = std::snprintf(buffer, sizeof(buffer), "%u", static_cast<unsigned>(port));
            std::fwrite(buffer, 1, static_cast<std::size_t>(length), file);
            std::fclose(file);
            return true;
        }

        std::uint16_t local_port(SOCKET handle) {
            sockaddr_storage storage {};
            int length = sizeof(storage);
            if (::getsockname(handle, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
                return 0;
            }
            if (storage.ss_family != AF_INET) {
                return 0;
            }
            auto* inet = reinterpret_cast<sockaddr_in*>(&storage);
            return ntohs(inet->sin_port);
        }
#else
        bool unix_domain_sockets_supported() {
            return true;
        }
#endif

        // Keeps SIGPIPE from killing the debuggee when the peer is gone. Asio
        // passes MSG_NOSIGNAL where the kernel has it; Apple's does not, so the
        // socket option covers that case.
        void apply_no_sigpipe(stream_socket& socket) {
#if defined(SO_NOSIGPIPE)
            int enable = 1;
            boost::system::error_code ignored;
            socket.set_option(asio::socket_base::do_not_route(false), ignored);
            ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));
#else
            (void)socket;
#endif
        }

        struct State final {
            explicit State(asio::io_context& context)
                : ctx(context) {
            }

            asio::io_context& ctx;
            AddressSpec spec;
            bool debug = false;

            std::optional<stream_socket> socket;
            std::optional<stream_acceptor> acceptor;

            std::deque<std::string> outbox;
            std::deque<Json> inbox;
            std::string protocol_error;
            FrameDecoder decoder;

            bool session_alive   = false;  // a peer is connected
            bool established     = false;  // a peer connected at least once
            bool read_closed     = false;  // peer sent EOF
            bool closed          = false;  // channel is finished (no more events)
            bool reported_close  = false;
            bool writing         = false;  // writer coroutine is alive
            bool write_in_flight = false;
            bool flush_took_over = false;  // flush() cancelled the writer on purpose
            bool emulated_unix   = false;  // Windows: localhost TCP behind a path
            std::string emulated_path;

            // Held as a callable so the writer can be resumed either because data
            // arrived or because the channel finished; exactly one of them runs.
            std::function<void()> write_waiter;

            // Set by the facade, cleared as soon as the channel finishes: a
            // completion that runs after the facade is gone finds nothing to call.
            std::function<void()> on_close_cb;

            void report_close() {
                if (reported_close) {
                    return;
                }
                reported_close = true;
                if (on_close_cb) {
                    auto callback = std::move(on_close_cb);
                    on_close_cb   = nullptr;
                    callback();
                }
            }

            // Terminal state for an unusable connection: everything pending is
            // dropped so no caller can wait on a queue that will never be written.
            void fail() {
                outbox.clear();
                close_transport();
                closed = true;
                notify_writer();
                report_close();
            }

            // Graceful end: the peer is gone and nothing is left to send.
            void finish_close() {
                outbox.clear();
                close_transport();
                closed = true;
                notify_writer();
                report_close();
            }

            void close_transport() {
                session_alive = false;
                if (socket) {
                    boost::system::error_code ignored;
                    socket->shutdown(asio::socket_base::shutdown_both, ignored);
                    socket->close(ignored);
                }
                if (acceptor) {
                    boost::system::error_code ignored;
                    acceptor->close(ignored);
                }
                if (emulated_unix && !emulated_path.empty()) {
                    remove_file(emulated_path);
                }
            }

            bool usable() const {
                return !closed && socket && socket->is_open();
            }

            // Wakes the writer coroutine: it re-checks the queue and the channel
            // state instead of being told what happened.
            void notify_writer() {
                if (!write_waiter) {
                    return;
                }
                auto waiter  = std::move(write_waiter);
                write_waiter = nullptr;
                asio::post(ctx, [waiter]() { waiter(); });
            }

            template <typename CompletionToken>
            auto async_wait_for_writes(CompletionToken&& token) {
                return asio::async_initiate<CompletionToken, void()>(
                    [this](auto&& handler) {
                        auto holder  = std::make_shared<std::decay_t<decltype(handler)>>(std::move(handler));
                        write_waiter = [holder]() { (*holder)(); };
                    },
                    token
                );
            }

            asio::awaitable<void> wait_for_writes() {
                if (!outbox.empty() || closed || read_closed) {
                    co_return;
                }
                co_await async_wait_for_writes(asio::use_awaitable);
            }

            void consume(const char* data, std::size_t size) {
                Json message;
                std::string error;
                auto status = decoder.push(data, size, message, error);
                for (;;) {
                    if (status == FrameStatus::ok) {
                        if (debug) {
                            std::fprintf(stderr, "[recv] %s\n", dump_json(message).c_str());
                        }
                        inbox.push_back(std::move(message));
                        message = Json();
                        status  = decoder.next(message, error);
                        continue;
                    }
                    if (status == FrameStatus::invalid) {
                        protocol_error = error.empty() ? "Invalid protocol." : error;
                        finish_close();
                    }
                    return;
                }
            }
        };

        // --- endpoints -------------------------------------------------------
        //
        // Connect mode resolves the endpoint on every attempt: the Windows
        // emulation reads the port file that the listening process wrote, and a
        // missing file simply means "not listening yet".

        bool resolve_unix_path(const AddressSpec& spec, std::string& out) {
            out = spec.host;
            replace_all(out, "$tmp", collapse_temp_directory());
            return !out.empty();
        }

        bool resolve_endpoint(State& state, asio::generic::stream_protocol::endpoint& out, std::string& error) {
            const auto& spec = state.spec;
            switch (spec.proto) {
            case AddressSpec::protocol::unix_: {
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
                if (state.emulated_unix) {
#    if defined(_WIN32)
                    const std::string text = read_port_file(state.emulated_path);
                    if (text.empty()) {
                        error = "the address file has no port yet";
                        return false;
                    }
                    char* end       = nullptr;
                    const long port = std::strtol(text.c_str(), &end, 10);
                    if (port <= 0 || port > 65535) {
                        error = "the address file holds an invalid port";
                        return false;
                    }
                    boost::system::error_code ec;
                    auto address = asio::ip::make_address("127.0.0.1", ec);
                    if (ec) {
                        error = ec.message();
                        return false;
                    }
                    out = asio::generic::stream_protocol::endpoint(
                        asio::ip::tcp::endpoint(address, static_cast<unsigned short>(port))
                    );
                    return true;
#    else
                    error = "emulated unix sockets are a Windows-only path";
                    return false;
#    endif
                }
                std::string path;
                if (!resolve_unix_path(spec, path)) {
                    error = "Invalid address.";
                    return false;
                }
#    if defined(_WIN32)
                // The Windows provider takes the path as UTF-8 bytes.
                out = asio::generic::stream_protocol::endpoint(
                    asio::local::stream_protocol::endpoint(path)
                );
#    else
                out = asio::generic::stream_protocol::endpoint(
                    asio::local::stream_protocol::endpoint(path)
                );
#    endif
                return true;
#else
                error = "unix domain sockets are not available";
                return false;
#endif
            }
            case AddressSpec::protocol::tcp6: {
                boost::system::error_code ec;
                auto address = asio::ip::make_address_v6(spec.host, ec);
                if (ec) {
                    error = ec.message();
                    return false;
                }
                out = asio::generic::stream_protocol::endpoint(asio::ip::tcp::endpoint(address, spec.port));
                return true;
            }
            case AddressSpec::protocol::tcp:
            default: {
                boost::system::error_code ec;
                auto address = asio::ip::make_address_v4(spec.host, ec);
                if (ec) {
                    error = ec.message();
                    return false;
                }
                out = asio::generic::stream_protocol::endpoint(asio::ip::tcp::endpoint(address, spec.port));
                return true;
            }
            }
        }

        // --- coroutines ------------------------------------------------------

        template <typename Loop>
        void spawn_loop(std::shared_ptr<State> state, Loop&& loop) {
            asio::co_spawn(
                state->ctx,
                std::forward<Loop>(loop)(state),
                [state](std::exception_ptr failure) {
                    if (!failure) {
                        return;
                    }
                    try {
                        std::rethrow_exception(failure);
                    } catch (const std::exception& error) {
                        log_debug("socket channel coroutine failed: %s", error.what());
                    } catch (...) {
                        log_debug("socket channel coroutine failed");
                    }
                    state->fail();
                }
            );
        }

        asio::awaitable<void> read_loop(std::shared_ptr<State> state) {
            std::array<char, 8192> buffer {};
            for (;;) {
                if (state->closed || !state->usable()) {
                    co_return;
                }
                boost::system::error_code ec;
                const std::size_t read = co_await state->socket->async_read_some(
                    asio::buffer(buffer.data(), buffer.size()),
                    asio::redirect_error(asio::use_awaitable, ec)
                );
                if (state->closed) {
                    co_return;
                }
                if (ec) {
                    if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset) {
                        // Peer EOF: stop reading, but keep the writer alive until
                        // the frames already queued have been handed to the peer.
                        state->read_closed = true;
                        if (state->outbox.empty()) {
                            state->finish_close();
                        } else {
                            state->notify_writer();
                        }
                        co_return;
                    }
                    state->fail();
                    co_return;
                }
                if (read > 0) {
                    state->consume(buffer.data(), read);
                    if (state->closed) {
                        co_return;
                    }
                }
            }
        }

        asio::awaitable<void> write_loop(std::shared_ptr<State> state) {
            for (;;) {
                while (state->outbox.empty()) {
                    if (state->closed) {
                        co_return;
                    }
                    if (state->read_closed) {
                        // Peer is gone and there is nothing left to send.
                        state->finish_close();
                        co_return;
                    }
                    co_await state->wait_for_writes();
                }
                if (state->closed || !state->usable()) {
                    co_return;
                }
                // The frame lives in the coroutine frame: close() may empty the
                // queue while the write is in flight without invalidating the
                // buffer the kernel is reading from.
                std::string frame = std::move(state->outbox.front());
                state->outbox.pop_front();

                state->write_in_flight = true;
                boost::system::error_code ec;
                co_await asio::async_write(
                    *state->socket, asio::buffer(frame),
                    asio::redirect_error(asio::use_awaitable, ec)
                );
                state->write_in_flight = false;

                if (state->closed) {
                    co_return;
                }
                if (state->flush_took_over) {
                    // flush() cancelled this write to send the remaining frames
                    // synchronously; the queue is no longer ours to drain.
                    co_return;
                }
                if (ec) {
                    // A failed write must not leave a queue that nothing will
                    // ever send: fail() drops it and finishes the channel.
                    state->fail();
                    co_return;
                }
                if (state->read_closed && state->outbox.empty()) {
                    state->finish_close();
                    co_return;
                }
            }
        }

        asio::awaitable<void> connect_loop(std::shared_ptr<State> state) {
            for (;;) {
                if (state->closed || state->established) {
                    co_return;
                }
                asio::generic::stream_protocol::endpoint endpoint;
                std::string error;
                if (!resolve_endpoint(*state, endpoint, error)) {
                    // Address not usable yet (a not-yet-written port file, for
                    // example): retry on the next round, like a refused connect.
                    co_await Reactor::instance().next_update();
                    continue;
                }
                stream_socket socket(state->ctx, endpoint.protocol());
                apply_no_sigpipe(socket);
                boost::system::error_code ec;
                co_await socket.async_connect(endpoint, asio::redirect_error(asio::use_awaitable, ec));
                if (state->closed || state->established) {
                    boost::system::error_code ignored;
                    socket.close(ignored);
                    co_return;
                }
                if (ec) {
                    boost::system::error_code ignored;
                    socket.close(ignored);
                    // Only before the first session: once a debuggee connected,
                    // a dropped connection is reported instead of retried.
                    co_await Reactor::instance().next_update();
                    continue;
                }

                state->socket.emplace(std::move(socket));
                state->session_alive = true;
                state->established   = true;
                log_debug("socket channel connected");
                spawn_loop(state, read_loop);
                spawn_loop(state, write_loop);
                co_return;
            }
        }

        asio::awaitable<void> accept_loop(std::shared_ptr<State> state) {
            for (;;) {
                if (state->closed || !state->acceptor) {
                    co_return;
                }
                stream_socket peer(state->ctx);
                boost::system::error_code ec;
                co_await state->acceptor->async_accept(peer, asio::redirect_error(asio::use_awaitable, ec));
                if (state->closed) {
                    boost::system::error_code ignored;
                    peer.close(ignored);
                    co_return;
                }
                if (ec) {
                    if (ec == boost::asio::error::operation_aborted) {
                        co_return;
                    }
                    state->fail();
                    co_return;
                }
                if (state->session_alive) {
                    // One debuggee per listener: a second connection is closed.
                    boost::system::error_code ignored;
                    peer.close(ignored);
                    continue;
                }
                apply_no_sigpipe(peer);
                state->socket.emplace(std::move(peer));
                state->session_alive = true;
                state->established   = true;
                log_debug("socket channel accepted a peer");
                spawn_loop(state, read_loop);
                spawn_loop(state, write_loop);
            }
        }

        // --- facade ----------------------------------------------------------

        class SocketChannel final : public Channel {
        public:
            explicit SocketChannel(AddressSpec spec)
                : spec_(std::move(spec)) {
            }

            ~SocketChannel() override {
                close();
            }

            bool start(std::string& error);

            void send(const Json& message) override {
                if (!state_ || state_->closed) {
                    return;
                }
                std::string frame = encode_frame_dap(message);
                if (frame.empty()) {
                    return;
                }
                state_->debug = debug;
                state_->outbox.push_back(std::move(frame));
                state_->notify_writer();
            }

            void flush() override;

            bool recv(Json& out, std::string& error) override {
                if (!state_) {
                    return false;
                }
                if (!state_->protocol_error.empty()) {
                    error = state_->protocol_error;
                    state_->protocol_error.clear();
                    return false;
                }
                if (state_->inbox.empty()) {
                    return false;
                }
                out = std::move(state_->inbox.front());
                state_->inbox.pop_front();
                return true;
            }

            void update(int timeout_ms) override {
                Reactor::instance().update(timeout_ms);
            }

            bool closed() const override {
                return !state_ || state_->closed;
            }

            bool connected() const override {
                return state_ && state_->session_alive;
            }

            void close() override {
                if (!state_ || state_->closed) {
                    return;
                }
                // Stop retrying, cancel everything pending and report once.
                state_->finish_close();
            }

            void set_on_close(std::function<void()> callback) override {
                if (state_) {
                    state_->on_close_cb = std::move(callback);
                }
            }

        private:
            static std::string encode_frame_dap(const Json& message) {
                return encode_frame(message);
            }

            bool start_listen(std::string& error);
            bool start_connect(std::string& error);

            AddressSpec spec_;
            std::shared_ptr<State> state_;
        };

        bool SocketChannel::start(std::string& error) {
            log_debug("socket channel %s %s", spec_.mode.c_str(), spec_.host.c_str());
            state_       = std::make_shared<State>(Reactor::instance().context());
            state_->spec = spec_;

            if (spec_.proto == AddressSpec::protocol::unix_ && !unix_domain_sockets_supported()) {
                // Windows without the AF_UNIX provider: the address file carries
                // the localhost TCP port of the listener instead.
                state_->emulated_unix = true;
                resolve_unix_path(spec_, state_->emulated_path);
                if (state_->emulated_path.empty()) {
                    error = "Invalid address.";
                    return false;
                }
            }

            if (spec_.mode == "connect") {
                return start_connect(error);
            }
            if (spec_.mode == "listen") {
                return start_listen(error);
            }
            error = "Invalid address.";
            return false;
        }

        bool SocketChannel::start_connect(std::string& error) {
            (void)error;
            // Nothing is validated up front: the first attempt runs in the
            // background and retries once per update round.
            spawn_loop(state_, connect_loop);
            return true;
        }

        bool SocketChannel::start_listen(std::string& error) {
            asio::generic::stream_protocol::endpoint endpoint;
            if (state_->emulated_unix) {
#if defined(_WIN32)
                boost::system::error_code ec;
                auto address = asio::ip::make_address("127.0.0.1", ec);
                if (ec) {
                    error = "socket create failed";
                    return false;
                }
                endpoint = asio::generic::stream_protocol::endpoint(asio::ip::tcp::endpoint(address, 0));
#else
                error = "emulated unix sockets are a Windows-only path";
                return false;
#endif
            } else {
                std::string resolve_error;
                if (!resolve_endpoint(*state_, endpoint, resolve_error)) {
                    error = "invalid address: " + spec_.host;
                    return false;
                }
                if (spec_.proto == AddressSpec::protocol::unix_) {
                    // A leftover socket file from a previous run would make bind
                    // fail; the listener owns the path.
                    std::string path;
                    if (resolve_unix_path(spec_, path)) {
                        remove_file(path);
                    }
                }
            }

            boost::system::error_code ec;
            stream_acceptor acceptor(Reactor::instance().context());
            if (acceptor.open(endpoint.protocol(), ec) || acceptor.bind(endpoint, ec) || acceptor.listen(asio::socket_base::max_listen_connections, ec)) {
                error = "invalid address: " + spec_.host;
                if (acceptor.is_open()) {
                    boost::system::error_code ignored;
                    acceptor.close(ignored);
                }
                return false;
            }
            if (state_->emulated_unix) {
#if defined(_WIN32)
                const std::uint16_t port = local_port(static_cast<SOCKET>(acceptor.native_handle()));
                if (port == 0 || !write_port_file(state_->emulated_path, port)) {
                    error = "invalid address: " + spec_.host;
                    boost::system::error_code ignored;
                    acceptor.close(ignored);
                    return false;
                }
#else
                error = "emulated unix sockets are a Windows-only path";
                return false;
#endif
            }

            state_->acceptor.emplace(std::move(acceptor));
            spawn_loop(state_, accept_loop);
            return true;
        }

        void SocketChannel::flush() {
            if (!state_ || state_->closed || !state_->usable()) {
                return;
            }
            if (Reactor::instance().in_update()) {
                // Documented precondition: flush() is an owner-thread call made
                // outside the pump. Doing a synchronous send from inside a
                // completion would race the operation that is being completed.
                return;
            }
            if (!state_->session_alive) {
                // Nothing is connected: the frames stay queued for the first peer.
                return;
            }

            // Take the write side away from the coroutine: cancelling is the only
            // way a synchronous send may touch this socket, and the cancelled
            // completion is ignored by write_loop().
            if (state_->write_in_flight) {
                state_->flush_took_over = true;
                boost::system::error_code ignored;
                state_->socket->cancel(ignored);
            }

            while (!state_->outbox.empty()) {
                auto& frame = state_->outbox.front();
                boost::system::error_code ec;
                const std::size_t written = state_->socket->send(asio::buffer(frame), 0, ec);
                if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
                    break;
                }
                if (ec) {
                    state_->fail();
                    return;
                }
                if (written == 0) {
                    break;
                }
                if (written < frame.size()) {
                    frame.erase(0, written);
                    break;
                }
                state_->outbox.pop_front();
            }
        }

    }  // namespace

    bool parse_address(std::string_view text, AddressSpec& out, std::string& error) {
        auto colon = text.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            error = "Invalid address.";
            return false;
        }
        for (std::size_t i = 0; i < colon; ++i) {
            char c = text[i];
            if (c < 'a' || c > 'z') {
                error = "Invalid address.";
                return false;
            }
        }
        std::string mode(text.substr(0, colon));
        std::string_view address = text.substr(colon + 1);
        if (address.empty()) {
            error = "Invalid address.";
            return false;
        }

        out      = AddressSpec {};
        out.mode = std::move(mode);

        if (address.front() == '@') {
            std::string path(address.substr(1));
            replace_all(path, "$tmp", collapse_temp_directory());
            out.proto = AddressSpec::protocol::unix_;
            out.host  = std::move(path);
            return true;
        }

        // [ipv6]:port
        if (address.front() == '[') {
            auto close = address.find(']');
            if (close != std::string_view::npos && close + 1 < address.size() && address[close + 1] == ':') {
                auto host = address.substr(1, close - 1);
                auto port = address.substr(close + 2);
                if (is_ipv6_literal(host)) {
                    char* end = nullptr;
                    std::string port_text(port);
                    long value = std::strtol(port_text.c_str(), &end, 10);
                    if (end && *end == '\0' && value >= 0 && value <= 65535) {
                        out.proto = AddressSpec::protocol::tcp6;
                        out.host  = std::string(host);
                        out.port  = static_cast<std::uint16_t>(value);
                        return true;
                    }
                }
            }
            error = "Invalid address.";
            return false;
        }

        // ipv4:port
        auto last = address.rfind(':');
        if (last != std::string_view::npos) {
            auto host = address.substr(0, last);
            auto port = address.substr(last + 1);
            if (is_ipv4_literal(host)) {
                char* end = nullptr;
                std::string port_text(port);
                long value = std::strtol(port_text.c_str(), &end, 10);
                if (end && *end == '\0' && value >= 0 && value <= 65535) {
                    out.proto = AddressSpec::protocol::tcp;
                    out.host  = std::string(host);
                    out.port  = static_cast<std::uint16_t>(value);
                    return true;
                }
            }
        }

        error = "Invalid address.";
        return false;
    }

    std::unique_ptr<Channel> open_socket_channel(std::string_view spec, std::string& error) {
        AddressSpec address;
        if (!parse_address(spec, address, error)) {
            return nullptr;
        }
        auto channel = std::make_unique<SocketChannel>(std::move(address));
        if (!channel->start(error)) {
            return nullptr;
        }
        return channel;
    }

}  // namespace luadebug::debugger::transport
