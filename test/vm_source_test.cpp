// Rules that decide *what* a VM source means, tested without any target process.
//
// Two independent halves:
//
//   * pattern syntax, which must be rejected before a scan because Gum answers
//     an empty hit list for both "malformed" and "nothing matched";
//   * ABI identification, which decides whether a symbol table may be bound or
//     whether an explicit version has to be asked for.
//
// Address/RVA/pattern *resolution* needs a live process and is covered by the
// e2e `vm-source` suite; this test pins the decisions that no process can change.

#include <common/bootstrap.h>
#include <common/vm_version.h>
#include <resolver/gum_pattern.h>
#include <vm/mapping.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#    include <sys/mman.h>
#    include <unistd.h>
#endif

namespace {

    int failures = 0;

    void check(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    using luadebug::gum_pattern_valid;
    namespace p = luadebug::debugger;

    void pattern_rules() {
        check(gum_pattern_valid("FD 7B ?? A9"), "typical arm64 prologue");
        check(gum_pattern_valid("55 48 89 e5"), "lowercase hex");
        check(gum_pattern_valid("4? ?? 00"), "single nibble wildcard");

        check(!gum_pattern_valid(""), "empty pattern");
        check(!gum_pattern_valid("FD"), "one byte is not identifying");
        check(!gum_pattern_valid("FD 7B "), "trailing space");
        check(!gum_pattern_valid("FD  7B"), "double space");
        check(!gum_pattern_valid("FD 7 B"), "single-digit token");
        check(!gum_pattern_valid("FD 7G"), "non-hex digit");
        check(!gum_pattern_valid("FD-7B"), "unseparated bytes");
        check(!gum_pattern_valid("FD 7B\tA9"), "tab separator");
        check(gum_pattern_valid("?? ??"), "wildcards are syntactically valid");
        check(!gum_pattern_valid("0xFD 0x7B"), "radix prefix");
    }

    void name_rules() {
        check(p::profile_from_name("lua51") == p::kProfileLua51, "lua51 name");
        check(p::profile_from_name("lua55") == p::kProfileLua55, "lua55 name");
        check(p::profile_from_name("luajit") == p::kProfileLuajit, "luajit name");
        check(p::profile_from_name("lua-latest") == p::kProfileLua55,
              "lua-latest shares the 5.5 ABI");
        // `auto` is what a "let the target decide" setting expands to.
        check(p::profile_from_name("auto") == p::kProfileUnknown, "auto is unspecified");
        check(p::profile_from_name("") == p::kProfileUnknown, "empty is unspecified");
        check(p::profile_from_name("lua56") == p::kProfileUnknown, "unknown name");
        check(p::profile_from_name("lua5.4") == p::kProfileUnknown, "spelling is exact");
        check(std::string(p::profile_id_name(p::kProfileLua54)) == "lua54", "id name");
        check(std::string(p::profile_id_name(p::kProfileUnknown)) == "unknown", "unknown id name");
    }

    void ident_rules() {
        using namespace std::string_view_literals;
        check(p::profile_from_lua_ident("$Lua: Lua 5.1.5  Copyright (C) 1994-2012 Lua.org, PUC-Rio $")
                  == p::kProfileLua51,
              "5.1 ident");
        check(p::profile_from_lua_ident("$LuaVersion: Lua 5.2.4  Copyright (C) 1994-2015 $")
                  == p::kProfileLua52,
              "5.2 ident");
        check(p::profile_from_lua_ident("$LuaVersion: Lua 5.3.6  Copyright (C) 1994-2020 $")
                  == p::kProfileLua53,
              "5.3 ident");
        check(p::profile_from_lua_ident("$LuaVersion: Lua 5.4.6  Copyright (C) 1994-2023 $")
                  == p::kProfileLua54,
              "5.4 ident");
        check(p::profile_from_lua_ident("$LuaVersion: Lua 5.5.0  Copyright (C) 1994-2025 $")
                  == p::kProfileLua55,
              "5.5 ident");
        // LuaJIT defines no `lua_ident`, so a LuaJIT-looking string must not be
        // read as one.
        check(p::profile_from_lua_ident("$LuaJITVersion: LuaJIT 2.1.0-beta3")
                  == p::kProfileUnknown,
              "no LuaJIT ident exists");
        check(p::profile_from_lua_ident("hello") == p::kProfileUnknown, "foreign data");
        check(p::profile_from_lua_ident("$LuaVersion: Lua 5.X") == p::kProfileUnknown,
              "non-numeric minor");
        check(p::profile_from_lua_ident("$Lua") == p::kProfileUnknown, "truncated ident");
    }

    void evidence_rules() {
        // LuaJIT answers the 5.1 ABI but has its own VM: the marker decides on its
        // own, whatever else the table happens to contain.
        {
            p::ProfileEvidence e;
            e.luajit    = true;
            e.lua52_plus = true;  // LuaJIT exports luaL_loadbufferx
            check(p::profile_from_evidence(e) == p::kProfileLuajit, "luajit marker wins");
        }
        // 5.5-only symbols are decisive.
        {
            p::ProfileEvidence e;
            e.lua55_only = true;
            check(p::profile_from_evidence(e) == p::kProfileLua55, "5.5-only marker");
        }
        // 5.4 markers alone cannot separate 5.4 from 5.5 in a partial table.
        {
            p::ProfileEvidence e;
            e.lua54_plus = true;
            check(p::profile_from_evidence(e) == p::kProfileUnknown,
                  "partial 5.4 evidence is ambiguous");
        }
        // The same evidence from a complete image does decide: a 5.5 image would
        // have answered a 5.5-only symbol.
        {
            p::ProfileEvidence e;
            e.lua54_plus = true;
            e.complete   = true;
            check(p::profile_from_evidence(e) == p::kProfileLua54, "complete 5.4 evidence");
        }
        {
            p::ProfileEvidence e;
            e.lua53_plus = true;
            e.complete   = true;
            check(p::profile_from_evidence(e) == p::kProfileLua53, "complete 5.3 evidence");
        }
        {
            p::ProfileEvidence e;
            e.lua52_plus = true;
            e.complete   = true;
            check(p::profile_from_evidence(e) == p::kProfileLua52, "complete 5.2 evidence");
        }
        {
            p::ProfileEvidence e;
            e.complete = true;
            check(p::profile_from_evidence(e) == p::kProfileLua51, "complete 5.1 evidence");
        }
        // A partial table with only shared API proves nothing at all.
        {
            p::ProfileEvidence e;
            e.lua52_plus = true;
            check(p::profile_from_evidence(e) == p::kProfileUnknown,
                  "partial 5.2 evidence is ambiguous");
        }
    }

    // Real markers, taken verbatim from the vendored sources. An earlier round of
    // tests fabricated short strings, which hid the fact that the reader's bound and
    // its control-character policy rejected every genuine `lua_ident`.
    void ident_markers() {
        // `3rd/lua/lua54/lapi.c` builds this; the string is exactly 128 bytes, so
        // the NUL sits outside any 128-byte window.
        const std::string lua54 =
            "$LuaVersion: Lua 5.4.6  Copyright (C) 1994-2023 Lua.org, PUC-Rio $"
            "$LuaAuthors: R. Ierusalimschy, L. H. de Figueiredo, W. Celes $";
        // 128 bytes exactly: the NUL falls outside a 128-byte window, which is why
        // the reader's bound has to be larger.
        check(lua54.size() >= 128, "the 5.4 marker is at least 128 bytes");
        check(p::profile_from_lua_ident(lua54) == p::kProfileLua54, "5.4 marker");
        check(p::profile_from_lua_ident(lua54.substr(0, 32)) == p::kProfileLua54,
              "the prefix alone decides");

        // 5.1 is three newline-separated sections and 140 bytes long.
        const std::string lua51 =
            "$Lua: Lua 5.1.5  Copyright (C) 1994-2012 Lua.org, PUC-Rio $\n"
            "$Authors: R. Ierusalimschy, L. H. de Figueiredo & W. Celes $\n"
            "$URL: www.lua.org $\n";
        check(lua51.size() > 128 && lua51.find('\n') != std::string::npos,
              "the 5.1 marker is long and newline-separated");
        check(p::profile_from_lua_ident(lua51) == p::kProfileLua51, "5.1 marker");
        // A bare `$Lua:` is not enough: another 5.x could use the same shape.
        check(p::profile_from_lua_ident("$Lua: Lua 5.0.3 something")
                  == p::kProfileUnknown,
              "a 5.0 marker is not 5.1");
        check(p::profile_from_lua_ident("$Lua: garbage") == p::kProfileUnknown,
              "an unrecognised legacy marker stays unknown");
    }

    // The reader must accept a genuine marker (long, possibly newline-terminated)
    // and refuse what it cannot safely read.
    void marker_reader() {
        using namespace luadebug::vm;
        const std::string lua51 =
            "$Lua: Lua 5.1.5  Copyright (C) 1994-2012 Lua.org, PUC-Rio $\n"
            "$Authors: R. Ierusalimschy, L. H. de Figueiredo & W. Celes $\n"
            "$URL: www.lua.org $\n";
        std::string out;
        check(read_cstring(lua51.c_str(), kLuaIdentBytes, out), "a real marker is readable");
        check(!out.empty() && out[0] == '$', "the marker text comes back");
        check(out.find('\n') == std::string::npos,
              "the newline terminates the marker instead of being copied");
        check(p::profile_from_lua_ident(out) == p::kProfileLua51,
              "the reader hands the parser something it can decide");

        // A non-printable byte before any terminator is not a version marker.
        const char binary[] = { '$', 'L', 'u', 'a', '\x01', '\0' };
        check(!read_cstring(binary, sizeof(binary), out), "control bytes are refused");
        // An unterminated buffer inside the bound is refused.
        const char no_end[4] = { 'a', 'b', 'c', 'd' };
        check(!read_cstring(no_end, sizeof(no_end), out), "no terminator is refused");
        check(!read_cstring(nullptr, 16, out), "a null address is refused");

#if !defined(_WIN32)
        // Residency is not permission: a resident PROT_NONE page must not be
        // readable, or the caller faults instead of getting an error.
        const long page = ::sysconf(_SC_PAGESIZE);
        void* guard = ::mmap(nullptr, static_cast<std::size_t>(page), PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(guard != MAP_FAILED, "a mapping can be created");
        if (guard != MAP_FAILED) {
            check(mapping_readable(guard, 16), "a writable mapping is readable");
            ::mprotect(guard, static_cast<std::size_t>(page), PROT_NONE);
            check(!mapping_readable(guard, 16), "a PROT_NONE page is not readable");
            ::mprotect(guard, static_cast<std::size_t>(page), PROT_READ);
            check(mapping_readable(guard, 16), "a read-only mapping is readable");
            ::munmap(guard, static_cast<std::size_t>(page));
        }
#endif
    }

    // Every name the version logic looks for must be namable by an external table,
    // and every name the bridge resolves must be accepted too.
    void symbol_whitelist() {
        const char* required[] = {
            // evidence
            "lua_ident", "lua_numbertocstring", "luaL_openselectedlibs", "lua_resetthread",
            "lua_newuserdatauv", "lua_isinteger", "lua_rawlen", "lua_callk",
            "luaL_loadbufferx", "luaJIT_setmode",
            // launcher discovery points, by role
            "lua_debug_server_newstate", "lua_debug_server_call",
        };
        // The launcher's discovery points are named by two pseudo symbols, not
        // by one name per hook: a table must be able to give those two roles,
        // and must not be able to claim an internal helper.
        check(!p::symbol_name_known("luaD_precall"), "a watch point is not a table name");
        check(!p::symbol_name_known("lua_newstate"), "the creation point is not a table name");
        check(!p::symbol_name_known("luaL_openlibs"), "library setup is not a table name");
        for (const char* name : required) {
            if (!p::symbol_name_known(name)) {
                std::fprintf(stderr, "FAIL: the whitelist rejects `%s`\n", name);
                ++failures;
            }
        }
        check(!p::symbol_name_known("lua_gtetop"), "a typo stays rejected");
        check(!p::symbol_name_known(""), "an empty name stays rejected");
    }

    // The parser must not depend on the caller's previous error string.
    void parsing_is_stateless() {
        luadebug::debugger::BootstrapConfig config;
        std::string error   = "stale message from an earlier call";
        std::vector<luadebug::debugger::SymbolEntry> symbols;
        const auto doc = nlohmann::json::parse(
            R"({"lua_gettop":{"address":"0x1000"},"lua_settop":{"pattern":"FD 7B"}})");
        check(luadebug::debugger::parse_symbols(doc, symbols, error),
              "a valid table parses over a reused error string");
        check(symbols.size() == 2, "both entries came back");

        // A present `null` is a wrong type, not "absent".
        error = {};
        check(!luadebug::debugger::parse_symbols(nlohmann::json(), symbols, error),
              "a null table is rejected");
        check(!error.empty(), "the null table explains itself");
    }

} // namespace

namespace {

    // Verifies an artefact the no-export host produced from its own function
    // pointers: it must parse, and every name must be one the contract accepts.
    // Without this, the host's `symbols` output could be an unusable document that
    // nothing ever fed back.
    int check_symbols_file(const char* path) {
        std::vector<luadebug::debugger::SymbolEntry> symbols;
        std::string error;
        if (!luadebug::debugger::load_symbols_file(path, symbols, error)) {
            std::fprintf(stderr, "FAIL: %s: %s\n", path, error.c_str());
            return 1;
        }
        if (symbols.empty()) {
            std::fprintf(stderr, "FAIL: %s is empty\n", path);
            return 1;
        }
        for (const auto& entry : symbols) {
            if (!luadebug::debugger::symbol_name_known(entry.name)) {
                std::fprintf(stderr, "FAIL: %s names unsupported symbol `%s`\n", path,
                             entry.name.c_str());
                return 1;
            }
            if (entry.rule.kind != luadebug::debugger::SymbolKind::address) {
                std::fprintf(stderr, "FAIL: %s entry `%s` is not an address\n", path,
                             entry.name.c_str());
                return 1;
            }
        }
        std::printf("symbol artefact: %zu usable entries in %s\n", symbols.size(), path);
        return 0;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--check-symbols") {
        return check_symbols_file(argv[2]);
    }
    pattern_rules();
    name_rules();
    ident_rules();
    evidence_rules();
    ident_markers();
    marker_reader();
    symbol_whitelist();
    parsing_is_stateless();
    if (failures == 0) {
        std::printf("vm source rules: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "vm source rules: %d failure(s)\n", failures);
    return 1;
}
