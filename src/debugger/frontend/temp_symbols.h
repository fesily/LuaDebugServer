#pragma once

// Session temp files for symbol tables that do not fit in the bootstrap
// document.
//
// frida-core's `data` argument (and `LUA_DEBUG_CONFIG`, which shares its
// 4095-byte ceiling) cannot carry a full table, so the adapter writes the table
// to a file and sends its path instead. The target reads the snapshot once,
// during binding, so the file only has to survive the session.
//
// Ownership is one-way: a file created here belongs to the adapter and is
// deleted at exit. A path a user supplied through `luaSymbolsFile` is not ours
// and is never removed.

#include <string>
#include <vector>

#include "common/vm_source.h"

namespace luadebug::debugger::frontend {

    // Absolute path of a private file holding `symbols` as a `luaSymbols`
    // object. Byte-identical tables share one file: the gated launch path builds
    // its bootstrap document twice (once for the preload/spawn plan, once inside
    // the injector), and both documents must name the same snapshot.
    bool temp_symbols_file(const std::vector<SymbolEntry>& symbols, std::string& path, std::string& error);

}  // namespace luadebug::debugger::frontend
