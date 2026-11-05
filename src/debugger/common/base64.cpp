#include "common/base64.h"

#include <array>

namespace luadebug::debugger {

    namespace {
        constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        int decode_char(char c) {
            if (c >= 'A' && c <= 'Z') {
                return c - 'A';
            }
            if (c >= 'a' && c <= 'z') {
                return c - 'a' + 26;
            }
            if (c >= '0' && c <= '9') {
                return c - '0' + 52;
            }
            if (c == '+') {
                return 62;
            }
            if (c == '/') {
                return 63;
            }
            return -1;
        }
    }  // namespace

    std::string base64_encode(std::string_view data) {
        std::string out;
        out.reserve(((data.size() + 2) / 3) * 4);
        std::size_t i = 0;
        while (i + 2 < data.size()) {
            const unsigned int group = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) | static_cast<unsigned char>(data[i + 2]);
            out.push_back(kAlphabet[(group >> 18) & 0x3F]);
            out.push_back(kAlphabet[(group >> 12) & 0x3F]);
            out.push_back(kAlphabet[(group >> 6) & 0x3F]);
            out.push_back(kAlphabet[group & 0x3F]);
            i += 3;
        }
        const std::size_t rest = data.size() - i;
        if (rest == 1) {
            const unsigned int group = static_cast<unsigned char>(data[i]) << 16;
            out.push_back(kAlphabet[(group >> 18) & 0x3F]);
            out.push_back(kAlphabet[(group >> 12) & 0x3F]);
            out.append("==");
        } else if (rest == 2) {
            const unsigned int group = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8);
            out.push_back(kAlphabet[(group >> 18) & 0x3F]);
            out.push_back(kAlphabet[(group >> 12) & 0x3F]);
            out.push_back(kAlphabet[(group >> 6) & 0x3F]);
            out.push_back('=');
        }
        return out;
    }

    bool base64_decode(std::string_view text, std::string& out) {
        out.clear();
        out.reserve((text.size() / 4) * 3);
        unsigned int buffer = 0;
        int bits            = 0;
        for (char c : text) {
            if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
                continue;
            }
            const int value = decode_char(c);
            if (value < 0) {
                return false;
            }
            buffer = (buffer << 6) | static_cast<unsigned int>(value);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            }
        }
        return true;
    }

}  // namespace luadebug::debugger
