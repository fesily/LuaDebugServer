#pragma once

// DAP wire framing: `Content-Length: N\r\n\r\n<json>`.
//
// The decoder keeps the historical strictness of extension/script/common/
// protocol.lua: the very first header must be exactly `Content-Length: `, the
// length must parse, and any deviation is a hard protocol error for that
// connection (never "no message yet").

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common/json.h"

namespace luadebug::debugger {

    enum class FrameStatus {
        ok,         // a complete message was decoded into `out`
        need_more,  // not enough bytes yet
        invalid,    // unrecoverable protocol error
    };

    class FrameDecoder {
    public:
        FrameStatus push(const char* data, std::size_t size, Json& out, std::string& error);
        // Drains a second complete message already buffered, if any.
        FrameStatus next(Json& out, std::string& error);

        void reset();
        bool empty() const noexcept {
            return buffer_.empty();
        }

    private:
        std::string buffer_;
        std::size_t pending_length_ = 0;
        bool have_length_           = false;
    };

    std::string encode_frame(const Json& message);

    // Encodes `message` and appends it to `out`; used by write queues so a
    // response and an event can be flushed in one call.
    void append_frame(const Json& message, std::string& out);

    void append_frame(const Json& message, std::vector<char>& out);

}  // namespace luadebug::debugger
