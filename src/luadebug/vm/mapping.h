#pragma once

// Guarded reads of target memory.
//
// Reading version evidence (`lua_ident`) means dereferencing an address that came
// from outside - a symbol table entry, or a module's symbol list. A wrong entry
// must fail the binding with a message, not take the process down inside a
// `strlen`, so every read is bounded and checked against the process's mappings
// first.

#include <cstddef>
#include <string>

namespace luadebug::vm {

    // Enough for any real `lua_ident`: 5.4/5.5 place the NUL at byte 128, and
    // 5.1's marker is 140 bytes because it carries the authors and URL sections.
    // A smaller bound silently rejects every genuine ident, which would drop the
    // caller back to marker inference - exactly the degradation this avoids.
    constexpr std::size_t kLuaIdentBytes = 256;

    // True when [address, address+length) is mapped and readable. False for a
    // null address, a wrapped range, or an unmapped one.
    bool mapping_readable(const void* address, std::size_t length);

    // True when `address` lies in a mapping the process may execute. A symbol
    // table is caller data: a data address in a function slot would fault inside
    // the debuggee instead of failing the binding.
    bool mapping_executable(const void* address);

    // Reads a version marker: printable ASCII terminated by NUL or newline (5.1's
    // `lua_ident` is three newline-separated sections). Returns false when the
    // range is unreadable, when neither terminator appears within `capacity`, or
    // when a non-printable byte appears first.
    bool read_cstring(const void* address, std::size_t capacity, std::string& out);

}  // namespace luadebug::vm
