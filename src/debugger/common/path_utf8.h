#pragma once

// Paths cross every wire format in this project as UTF-8 bytes: DAP JSON, the
// bootstrap config, `lua_pushstring` into the target VM. `std::filesystem::path`
// is the type everywhere (one path type, no Boost.Filesystem dependency), and
// the conversion is explicit anyway: a narrow `path::string()` on Windows
// produces the process code page, which would silently mangle a UTF-8 path.
//
// These helpers are the only place that converts. They never consult or change
// the process locale.

#include <filesystem>
#include <string>
#include <string_view>

namespace luadebug::debugger {

    namespace fs = std::filesystem;

    // UTF-8 bytes of a path. POSIX keeps the stored bytes; Windows converts the
    // wide form, preserving unpaired surrogates (WTF-8) so a round trip through
    // the debugger cannot corrupt a name the OS accepted.
    std::string path_to_utf8(const fs::path& path);

    // Path from UTF-8 bytes. Undecodable sequences are rejected (empty path)
    // rather than replaced: a wrong path is worse than a reported failure.
    fs::path path_from_utf8(std::string_view text);

    // Same conversions without the path type, for argv/environment values.
    std::string wide_to_utf8(const wchar_t* text, std::size_t length);
    std::wstring utf8_to_wide(std::string_view text);

}  // namespace luadebug::debugger
