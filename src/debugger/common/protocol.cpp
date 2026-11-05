#include "common/protocol.h"

#include <cstdio>
#include <cstring>

namespace luadebug::debugger {

    namespace {
        constexpr std::string_view kHeaderPrefix = "Content-Length: ";
        constexpr std::string_view kHeaderEnd    = "\r\n\r\n";
        // A single DAP message larger than this is treated as a protocol error
        // rather than an unbounded allocation.
        constexpr std::size_t kMaxMessageSize = 256u * 1024u * 1024u;
    }  // namespace

    void FrameDecoder::reset() {
        buffer_.clear();
        pending_length_ = 0;
        have_length_    = false;
    }

    FrameStatus FrameDecoder::push(const char* data, std::size_t size, Json& out, std::string& error) {
        if (size != 0) {
            buffer_.append(data, size);
        }
        return next(out, error);
    }

    FrameStatus FrameDecoder::next(Json& out, std::string& error) {
        if (!have_length_) {
            auto pos = buffer_.find(kHeaderEnd);
            if (pos == std::string::npos) {
                return FrameStatus::need_more;
            }
            // The header block must be the fixed prefix followed by the length.
            if (pos <= kHeaderPrefix.size() - 1 || buffer_.compare(0, kHeaderPrefix.size(), kHeaderPrefix) != 0) {
                error = "Invalid protocol.";
                return FrameStatus::invalid;
            }
            std::size_t length = 0;
            for (std::size_t i = kHeaderPrefix.size(); i < pos; ++i) {
                char c = buffer_[i];
                if (c < '0' || c > '9') {
                    error = "Invalid protocol.";
                    return FrameStatus::invalid;
                }
                length = length * 10 + static_cast<std::size_t>(c - '0');
                if (length > kMaxMessageSize) {
                    error = "Invalid protocol.";
                    return FrameStatus::invalid;
                }
            }
            buffer_.erase(0, pos + kHeaderEnd.size());
            pending_length_ = length;
            have_length_    = true;
        }
        if (buffer_.size() < pending_length_) {
            return FrameStatus::need_more;
        }
        std::string body = buffer_.substr(0, pending_length_);
        buffer_.erase(0, pending_length_);
        pending_length_ = 0;
        have_length_    = false;
        if (parse_json(body, out, error) != ParseStatus::ok) {
            return FrameStatus::invalid;
        }
        return FrameStatus::ok;
    }

    std::string encode_frame(const Json& message) {
        std::string payload = dump_json(message);
        std::string frame;
        frame.reserve(payload.size() + 32);
        char header[32];
        int n = std::snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", payload.size());
        frame.append(header, static_cast<std::size_t>(n));
        frame.append(payload);
        return frame;
    }

    void append_frame(const Json& message, std::string& out) {
        std::string payload = dump_json(message);
        char header[32];
        int n = std::snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", payload.size());
        out.append(header, static_cast<std::size_t>(n));
        out.append(payload);
    }

    void append_frame(const Json& message, std::vector<char>& out) {
        std::string payload = dump_json(message);
        char header[32];
        int n      = std::snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", payload.size());
        auto begin = out.size();
        out.resize(begin + static_cast<std::size_t>(n) + payload.size());
        std::memcpy(out.data() + begin, header, static_cast<std::size_t>(n));
        std::memcpy(out.data() + begin + static_cast<std::size_t>(n), payload.data(), payload.size());
    }

}  // namespace luadebug::debugger
