// The bootstrap document is built by the adapter and parsed by the launcher, so
// the same rules must hold on both sides. Both use common/bootstrap.cpp; this
// test pins the contract: what travels, what is rejected, and what the defaults
// are when a carrier is empty.

#include "common/bootstrap.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if !defined(_WIN32)
#    include <unistd.h>
#else
#    include <process.h>
#    define getpid _getpid
#endif

namespace {

    int failures = 0;

    void check(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    using luadebug::debugger::BootstrapConfig;
    using luadebug::debugger::BootstrapMode;

    bool parse(const std::string& text, BootstrapConfig& out, std::string& error) {
        return luadebug::debugger::parse_bootstrap(text, out, error);
    }

    void expect_ok(const char* text, const char* what) {
        BootstrapConfig config;
        std::string error;
        check(parse(text, config, error), what);
    }

    void expect_rejected(const char* text, const char* what) {
        BootstrapConfig config;
        std::string error;
        check(!parse(text, config, error), what);
        check(!error.empty(), what);
    }

} // namespace

int main() {
    // Defaults: an empty object is a valid document meaning "attach, pid socket,
    // auto-detected Lua version".
    {
        BootstrapConfig config;
        std::string error;
        check(parse("{}", config, error), "empty object is valid");
        check(config.mode == BootstrapMode::attach, "mode defaults to attach");
        check(config.address.empty(), "address has no default here");
        check(!config.client, "client defaults to false");
        check(config.lua_version.empty(), "lua version has no default");
        check(!config.ansi, "ansi defaults to false");
        check(config.platform.empty(), "platform has no default");
    }

    // Full document round-trips, and only the fields that were set are emitted.
    {
        BootstrapConfig config;
        config.mode        = BootstrapMode::launch;
        config.address     = "@$tmp/luadbg_42";
        config.client      = true;
        config.lua_version = "lua54";
        config.ansi        = true;
        config.platform    = "darwin-x64";
        config.root        = "/opt/publish";
        auto text          = luadebug::debugger::dump_bootstrap(config);
        check(!text.empty(), "dump produces a document");

        BootstrapConfig parsed;
        std::string error;
        check(parse(text, parsed, error), "own document parses");
        check(parsed.mode == BootstrapMode::launch, "mode round-trips");
        check(parsed.address == config.address, "address round-trips");
        check(parsed.client, "client round-trips");
        check(parsed.lua_version == config.lua_version, "lua version round-trips");
        check(parsed.ansi, "ansi round-trips");
        check(parsed.platform == config.platform, "platform round-trips");
        check(parsed.root == config.root, "root round-trips");
    }

    // Explicit false must survive as a value, not disappear into a default.
    {
        BootstrapConfig config;
        std::string error;
        check(parse(R"({"client":false,"ansi":false})", config, error),
              "explicit false is accepted");
        check(!config.client && !config.ansi, "explicit false stays false");
    }

    expect_rejected("", "empty carrier is not a document");
    expect_rejected("not json", "malformed json");
    expect_rejected("[]", "array document");
    expect_rejected("42", "scalar document");
    expect_rejected(R"({"mode":"boot"})", "unknown mode");
    expect_rejected(R"({"mode":""})", "empty mode");
    expect_rejected(R"({"mode":1})", "non-string mode");
    expect_rejected(R"({"address":""})", "empty address");
    expect_rejected(R"({"address":42})", "non-string address");
    expect_rejected(R"({"luaVersion":true})", "non-string lua version");
    expect_rejected(R"({"luaVersion":""})", "empty lua version");
    expect_rejected(R"({"luaVersion":"lua56"})", "unknown lua ABI name");
    expect_rejected(R"({"luaVersion":"lua-5.4-custom"})", "non-ABI version name");
    expect_rejected(R"({"platform":""})", "empty platform");
    expect_rejected(R"({"client":"yes"})", "non-boolean client");
    expect_rejected(R"({"ansi":1})", "non-boolean ansi");
    expect_rejected(R"({"root":0})", "non-integer root");
    // A decoded NUL cannot travel through a C string, a Lua string or getenv.
    expect_rejected(R"({"address":"a\u0000b"})", "NUL in address");
    expect_rejected(R"({"luaVersion":"a\u0000b"})", "NUL in lua version");

    // Unknown fields are not forwarded, and do not invalidate the document.
    {
        BootstrapConfig config;
        std::string error;
        check(parse(R"({"address":"@x","program":"/tmp/a.lua","breakpoints":[1]})", config, error),
              "unknown fields are ignored");
        check(config.address == "@x", "known field still parsed");
    }

    // The frida entry point receives at most 4095 bytes on Darwin
    // (entrypoint_data_storage), so an oversized document must be refused
    // instead of being silently truncated by the injector.
    {
        BootstrapConfig config;
        config.address = std::string(4100, 'a');
        check(luadebug::debugger::dump_bootstrap(config).empty(),
              "oversized document is refused");
    }

    // The serializer round-trips through the parser, so a field the launcher would
    // reject can never be delivered in the first place.
    {
        BootstrapConfig config;
        config.lua_version = std::string("lua54\0x", 7);
        check(luadebug::debugger::dump_bootstrap(config).empty(),
              "a field with an embedded NUL is not delivered");
    }
    {
        BootstrapConfig config;
        config.address = "";
        check(!luadebug::debugger::dump_bootstrap(config).empty(),
              "empty optional fields are simply omitted");
    }

    expect_ok(R"({"mode":"launch","address":"@$tmp/x","client":true,"luaVersion":"luajit"})",
              "typical launch document");

    // --- optional VM source ------------------------------------------------
    // `auto` is a setting value, not a profile: it means "unspecified" and must
    // not reach the binding as a version constraint.
    {
        BootstrapConfig config;
        std::string error;
        check(parse(R"({"luaVersion":"auto"})", config, error), "auto version parses");
        check(config.lua_version.empty(), "auto version clears the constraint");
    }
    {
        BootstrapConfig config;
        std::string error;
        check(parse(R"({"luaBinary":"private-vm.dylib"})", config, error),
              "luaBinary parses");
        check(config.lua_binary == "private-vm.dylib", "luaBinary round-trips");
        check(config.has_vm_source(), "luaBinary counts as a source");
        check(config.lua_symbols.empty() && config.lua_symbols_file.empty(),
              "luaBinary alone adds no symbols");
    }
    {
        BootstrapConfig config;
        std::string error;
        check(parse(R"({"luaSymbols":{"lua_gettop":{"offset":"0x12340"},)"
                    R"("lua_settop":{"pattern":"FD 7B ?? A9"},)"
                    R"("lua_pushvalue":{"address":"0x0000000101235000"}}})",
                    config, error),
              "three rule forms parse");
        check(config.lua_symbols.size() == 3, "every entry is kept");
        for (const auto& entry : config.lua_symbols) {
            if (entry.name == "lua_gettop") {
                check(entry.rule.kind == luadebug::debugger::SymbolKind::offset,
                      "offset rule kind");
                check(entry.rule.value == 0x12340, "offset value");
            } else if (entry.name == "lua_settop") {
                check(entry.rule.kind == luadebug::debugger::SymbolKind::pattern,
                      "pattern rule kind");
                check(entry.rule.pattern == "FD 7B ?? A9", "pattern text");
            } else if (entry.name == "lua_pushvalue") {
                check(entry.rule.kind == luadebug::debugger::SymbolKind::address,
                      "address rule kind");
                check(entry.rule.value == 0x101235000ull, "address value");
            }
        }
    }
    {
        // A table that names one symbol twice cannot be expressed as JSON, but a
        // file may hold anything a producer writes; the parser is the boundary.
        BootstrapConfig config;
        std::string error;
        check(!luadebug::debugger::parse_symbols(
                   nlohmann::json::parse(R"({"lua_gettop":{"address":"0x1"},)"
                                         R"("lua_gtetop":{"address":"0x1"}})"),
                   config.lua_symbols, error),
              "unknown symbol name is rejected");
        check(!error.empty(), "unknown symbol name reports why");
    }
    expect_rejected(R"({"luaSymbols":{"lua_gettop":{"address":"0x1","offset":"0x2"}}})",
                    "two rules for one symbol");
    expect_rejected(R"({"luaSymbols":{"lua_gettop":{}}})", "no rule for a symbol");
    expect_rejected(R"({"luaSymbols":{"lua_gettop":{"address":"4096"}}})",
                    "decimal address");
    expect_rejected(R"({"luaSymbols":{"lua_gettop":{"address":"0x"}}})", "empty hex digits");
    expect_rejected(R"({"luaSymbols":{"lua_gettop":{"address":"0x10000000000000000"}}})",
                    "address wider than the pointer");
    expect_rejected(R"({"luaSymbols":{"lua_gettop":{"offset":"-0x10"}}})", "negative offset");
    // A number is a wrong type; the document itself must stay parseable, or the
    // rejection would come from the JSON parser instead of the field check.
    expect_rejected(R"({"luaSymbols":{"lua_gettop":{"address":4096}}})", "numeric address");
    expect_rejected(R"({"luaSymbols":{"lua_gettop":{"pattern":"a\u0000b"}}})",
                    "NUL in pattern");
    expect_rejected(R"({"luaSymbols":[]})", "symbols table is not an array");
    expect_rejected(R"({"luaSymbols":{"":{"address":"0x1"}}})", "empty symbol name");
    expect_rejected(R"({"luaSymbols":{"lua_gettop":{"address":"0x1"}},)"
                    R"("luaSymbolsFile":"/tmp/x.json"})",
                    "inline and file symbols are exclusive");
    expect_rejected(R"({"luaSymbolsFile":""})", "empty symbols file");
    expect_rejected(R"({"luaBinary":0})", "non-string luaBinary");

    // The version-evidence names are part of the accepted set: a table that only
    // names shared API cannot decide 5.4 from 5.5, and these are its only way to
    // say which one it is.
    {
        BootstrapConfig config;
        std::string error;
        check(parse(R"({"luaSymbols":{"lua_numbertocstring":{"address":"0x1"},)"
                    R"("lua_resetthread":{"address":"0x2"},)"
                    R"("lua_newuserdatauv":{"address":"0x3"},)"
                    R"("luaL_openselectedlibs":{"address":"0x4"}}})",
                    config, error),
              "version-evidence symbols are accepted");
        check(config.lua_symbols.size() == 4, "all four evidence entries are kept");
    }
    expect_rejected(R"({"luaSymbols":null})", "null symbol table");

    // An empty inline object means "not provided", not "an empty table that
    // suppresses module lookup".
    {
        BootstrapConfig config;
        std::string error;
        check(parse(R"({"luaSymbols":{}})", config, error), "empty symbol table parses");
        check(!config.has_vm_source(), "empty symbol table is not a source");
    }

    // Symbols survive a dump/parse round trip.
    {
        BootstrapConfig config;
        config.lua_version = "lua54";
        config.lua_binary  = "libcustom.dylib";
        luadebug::debugger::SymbolEntry entry;
        entry.name       = "lua_gettop";
        entry.rule.kind  = luadebug::debugger::SymbolKind::offset;
        entry.rule.value = 0x4f2a0;
        config.lua_symbols.push_back(entry);

        auto text = luadebug::debugger::dump_bootstrap(config);
        check(!text.empty(), "document with a small table is deliverable");
        BootstrapConfig parsed;
        std::string error;
        check(parse(text, parsed, error), "document with a small table parses");
        check(parsed.lua_symbols.size() == 1, "symbol survives the round trip");
        check(parsed.lua_symbols[0].rule.value == 0x4f2a0, "rule value survives");
        check(parsed.lua_binary == "libcustom.dylib", "luaBinary survives");
        check(parsed.lua_version == "lua54", "explicit version survives");
    }

    // A table too large for the carrier must be refused so the caller falls back
    // to `luaSymbolsFile` instead of losing entries to truncation.
    {
        BootstrapConfig config;
        for (int i = 0; i < 200; ++i) {
            luadebug::debugger::SymbolEntry entry;
            entry.name       = std::string("lua_gettop") + std::to_string(i);
            entry.rule.kind  = luadebug::debugger::SymbolKind::pattern;
            entry.rule.pattern = "FD 7B BF A9";
            config.lua_symbols.push_back(entry);
        }
        // Unknown names are rejected, so the document is refused either way; the
        // size limit is what the adapter relies on for a *valid* large table.
        check(luadebug::debugger::dump_bootstrap(config).empty(),
              "oversized inline table is refused");
    }

    // The file carrier holds the same object; reading it back must produce the
    // same rules, and an unreadable or malformed file must be a hard error.
    {
        const auto path = std::filesystem::temp_directory_path()
                        / ("luadebug-symbols-" + std::to_string(::getpid()) + ".json");
        std::vector<luadebug::debugger::SymbolEntry> written;
        luadebug::debugger::SymbolEntry a;
        a.name       = "lua_gettop";
        a.rule.kind  = luadebug::debugger::SymbolKind::address;
        a.rule.value = 0x102030400ull;
        written.push_back(a);
        luadebug::debugger::SymbolEntry b;
        b.name         = "lua_settop";
        b.rule.kind    = luadebug::debugger::SymbolKind::pattern;
        b.rule.pattern = "FD 7B ?? A9";
        written.push_back(b);

        std::string error;
        check(luadebug::debugger::save_symbols_file(path.string(), written, error),
              "symbols file is written");
        std::vector<luadebug::debugger::SymbolEntry> read_back;
        check(luadebug::debugger::load_symbols_file(path.string(), read_back, error),
              "symbols file is read back");
        check(read_back.size() == 2, "both entries come back");
        check(read_back[0].rule.value == 0x102030400ull, "address survives the file");
        check(read_back[1].rule.pattern == "FD 7B ?? A9", "pattern survives the file");

        std::filesystem::remove(path);
        check(!luadebug::debugger::load_symbols_file(path.string(), read_back, error),
              "a missing symbols file is an error");
        check(!error.empty(), "a missing symbols file reports why");
    }

    if (failures == 0) {
        std::printf("bootstrap config: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "bootstrap config: %d failure(s)\n", failures);
    return 1;
}
