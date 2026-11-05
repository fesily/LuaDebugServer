#include "common/path_utf8.h"

#include <cstdint>

#if defined(_WIN32)
#    include <windows.h>
#endif

namespace luadebug::debugger {

#if defined(_WIN32)

    // UTF-8 <-> UTF-16 that keeps unpaired surrogates intact (WTF-8): Windows
    // names and command lines are not required to be well formed, and both
    // WideCharToMultiByte and MultiByteToWideChar would replace them with U+FFFD.
    namespace {

        constexpr std::uint32_t kSurrogateHighStart = 0xD800;
        constexpr std::uint32_t kSurrogateHighEnd   = 0xDBFF;
        constexpr std::uint32_t kSurrogateLowStart  = 0xDC00;
        constexpr std::uint32_t kSurrogateLowEnd    = 0xDFFF;

        bool is_high_surrogate(std::uint32_t value) {
            return value >= kSurrogateHighStart && value <= kSurrogateHighEnd;
        }

        bool is_low_surrogate(std::uint32_t value) {
            return value >= kSurrogateLowStart && value <= kSurrogateLowEnd;
        }

        void append_utf8(std::string& out, std::uint32_t cp) {
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        void append_utf16(std::wstring& out, std::uint32_t cp) {
            if (cp < 0x10000) {
                out.push_back(static_cast<wchar_t>(cp));
                return;
            }
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }

    }  // namespace

    std::string wide_to_utf8(const wchar_t* text, std::size_t length) {
        std::string out;
        out.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            std::uint32_t unit = static_cast<std::uint32_t>(static_cast<std::uint16_t>(text[i]));
            if (is_high_surrogate(unit) && i + 1 < length) {
                std::uint32_t next = static_cast<std::uint32_t>(static_cast<std::uint16_t>(text[i + 1]));
                if (is_low_surrogate(next)) {
                    append_utf8(out, 0x10000 + ((unit - kSurrogateHighStart) << 10) + (next - kSurrogateLowStart));
                    ++i;
                    continue;
                }
            }
            append_utf8(out, unit);
        }
        return out;
    }

    std::wstring utf8_to_wide(std::string_view text) {
        std::wstring out;
        out.reserve(text.size());
        std::size_t i = 0;
        while (i < text.size()) {
            unsigned char lead = static_cast<unsigned char>(text[i]);
            std::uint32_t cp   = 0;
            std::size_t extra  = 0;
            if (lead < 0x80) {
                cp = lead;
            } else if ((lead & 0xE0) == 0xC0) {
                cp    = lead & 0x1F;
                extra = 1;
            } else if ((lead & 0xF0) == 0xE0) {
                cp    = lead & 0x0F;
                extra = 2;
            } else if ((lead & 0xF8) == 0xF0) {
                cp    = lead & 0x07;
                extra = 3;
            } else {
                return {};
            }
            if (i + extra >= text.size()) {
                return {};
            }
            for (std::size_t k = 1; k <= extra; ++k) {
                unsigned char continuation = static_cast<unsigned char>(text[i + k]);
                if ((continuation & 0xC0) != 0x80) {
                    return {};
                }
                cp = (cp << 6) | (continuation & 0x3F);
            }
            i += extra + 1;
            append_utf16(out, cp);
        }
        return out;
    }

    std::string path_to_utf8(const fs::path& path) {
        const std::wstring& wide = path.native();
        return wide_to_utf8(wide.data(), wide.size());
    }

    fs::path path_from_utf8(std::string_view text) {
        return fs::path(utf8_to_wide(text));
    }

#else

    // POSIX paths are byte strings; they are already the UTF-8 the protocols want.
    std::string path_to_utf8(const fs::path& path) {
        return path.native();
    }

    fs::path path_from_utf8(std::string_view text) {
        return fs::path(std::string(text));
    }

    std::string wide_to_utf8(const wchar_t* text, std::size_t length) {
        std::string out;
        out.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            out.push_back(static_cast<char>(text[i]));
        }
        return out;
    }

    std::wstring utf8_to_wide(std::string_view text) {
        std::wstring out;
        out.reserve(text.size());
        for (char c : text) {
            out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        return out;
    }

#endif

}  // namespace luadebug::debugger
