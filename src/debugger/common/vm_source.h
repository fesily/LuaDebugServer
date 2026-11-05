#pragma once

// An externally supplied description of the debuggee's Lua implementation.
//
// The bridge has always identified the target VM by the module that called into
// it. That fails when the binary exports nothing (statically linked interpreter,
// stripped or hidden symbols): there is no module handle to search, so there is
// nothing to identify. This contract lets the outside name the C API directly -
// three optional fields, all of them independent of module exports:
//
//   luaBinary      loaded image path or file name; selects *where* to look,
//   luaVersion     ABI constraint ("lua54", ...; "auto" means unspecified),
//   luaSymbols     symbol name -> address|offset|pattern, inline,
//   luaSymbolsFile the same object in a UTF-8 JSON file the target can read.
//
// `luaSymbols` and `luaSymbolsFile` are mutually exclusive. Absent means "not
// provided"; an explicit empty string, a wrong type, an embedded NUL or a
// conflicting pair is an error. Nothing here silently falls back to a guess.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace luadebug::debugger {

    enum class SymbolKind {
        // Absolute address in the target's current address space.
        address,
        // Non-negative RVA from the loaded module's base (not a file offset).
        offset,
        // Gum pattern; the single hit is the function entry.
        pattern,
    };

    struct SymbolRule {
        SymbolKind kind = SymbolKind::address;
        // `address` and `offset` travel as hexadecimal text so a JavaScript
        // producer cannot lose precision on a large pointer.
        std::uint64_t value = 0;
        std::string pattern;
    };

    struct SymbolEntry {
        std::string name;
        SymbolRule rule;
    };

    // True when `name` is a symbol some consumer actually reads. Rejecting
    // unknown names keeps a typo from binding nothing and looking like success.
    bool symbol_name_known(std::string_view name);

    // Parses one `luaSymbols` object. Each entry names exactly one of
    // `address`/`offset`/`pattern`.
    bool parse_symbols(const nlohmann::json& doc, std::vector<SymbolEntry>& out, std::string& error);

    // Serializes `symbols` back into a `luaSymbols` object. Only entries that came
    // from `parse_symbols` are guaranteed to round-trip: an unknown name or an empty
    // pattern would produce a document the parser (and therefore
    // `dump_bootstrap`'s own re-parse) rejects.
    nlohmann::json dump_symbols(const std::vector<SymbolEntry>& symbols);

    // Reads and parses a `luaSymbolsFile`. The file holds the object itself, not
    // a bootstrap document.
    bool load_symbols_file(const std::string& path, std::vector<SymbolEntry>& out, std::string& error);

    // Writes `symbols` as a `luaSymbols` object. Used by the adapter to move a
    // large inline table out of the bootstrap document.
    bool save_symbols_file(const std::string& path, const std::vector<SymbolEntry>& symbols, std::string& error);

}  // namespace luadebug::debugger
