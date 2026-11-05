#pragma once

// DAP byte channels. Two implementations exist:
//   * socket channel  - port of extension/script/common/socket.lua (connect
//                       with unbounded retry, listen, `@$tmp` unix paths,
//                       `connect:` / `listen:` prefixes, TCP/TCP6/UNIX).
//   * stdio channel   - port of extension/script/frontend/stdio.lua.
//
// Both expose the same 1:1 replacement for sendmsg/recvmsg/update.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "common/json.h"
#include "common/protocol.h"

namespace luadebug::debugger::transport {

    class Channel {
    public:
        virtual ~Channel() = default;

        // Queues one DAP message. Never blocks: the transport flushes on update.
        virtual void send(const Json& message) = 0;

        // Returns true and fills `out` when a complete message is available.
        // `error` is set (and the connection must be torn down) on protocol or
        // frame errors.
        virtual bool recv(Json& out, std::string& error) = 0;

        virtual void update(int timeout_ms) = 0;

        // Writes everything queued, as far as the peer accepts it, without
        // waiting for the next update(). Used when the process is about to end
        // and the queued events must be on the wire first.
        virtual void flush() = 0;

        virtual bool closed() const = 0;

        // True once a peer is actually on the other end (a listening socket is
        // open but not connected). Used to prove a launch really reached the
        // debugger rather than merely opening a socket.
        virtual bool connected() const = 0;

        virtual void close() = 0;

        // Invoked once when the peer goes away (EOF or protocol error). Installed
        // by the owner (the frontend proxy) right after the channel is created;
        // the implementation must call it at most once and must drop the callback
        // when the channel finishes, so a completion that runs after the owner is
        // gone cannot call into it.
        virtual void set_on_close(std::function<void()> callback) = 0;

        // Debug tracing of raw frames, as `m.debug(true)` in Lua.
        bool debug = false;
    };

    struct AddressSpec {
        enum class protocol {
            tcp,
            tcp6,
            unix_,
        };
        protocol proto = protocol::tcp;
        std::string host;
        std::uint16_t port = 0;
        std::string mode;  // "connect" or "listen"
    };

    // Parses `connect:127.0.0.1:4711`, `listen:[::1]:4711` or `connect:@$tmp/x`.
    bool parse_address(std::string_view text, AddressSpec& out, std::string& error);

    // Creates a socket channel for a `connect:`/`listen:` specification.
    // Returns nullptr with `error` for unusable input (invalid address,
    // failed bind/listen). Connect failures keep retrying in the background,
    // matching the Lua implementation.
    std::unique_ptr<Channel> open_socket_channel(std::string_view spec, std::string& error);

    // Creates the stdio channel (DAP over stdin/stdout).
    std::unique_ptr<Channel> open_stdio_channel(std::string& error);

}  // namespace luadebug::debugger::transport
