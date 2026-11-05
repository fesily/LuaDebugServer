#include <resolver/symbol_source.h>
#include <resolver/gum_pattern.h>

#include <gumpp.hpp>
#include <resolver/module_lookup.h>
#include <util/log.h>
#include <vm/mapping.h>
#include <vm/symbol_names.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace luadebug {
    namespace {

        struct module_info {
            const char* path = nullptr;
            const char* name = nullptr;
            void* base       = nullptr;
            std::size_t size = 0;
        };

        bool is_hex_digit(char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        // Data symbols are read, not called: `lua_ident` is a version marker.
        bool is_data_symbol(std::string_view name) {
            return name == "lua_ident";
        }

        std::string hex_of(std::uintptr_t value) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
            return std::string(buffer);
        }

        std::string path_of(const module_info& module) {
            return module.path ? std::string(module.path) : std::string();
        }

        // Exact path, else exact file name. Never a substring: "lua54" must not
        // select "lua54-vendor.so" or "liblua54.dylib" by accident.
        std::vector<module_info> select_modules(const std::string& binary) {
            std::vector<module_info> matches;
            Gum::Process::enumerate_modules([&](const Gum::Module& module) -> bool {
                const char* path = module.path();
                const char* name = module.name();
                const bool hit   = (path && binary == path) || (name && binary == name);
                if (!hit) {
                    return true;
                }
                const Gum::MemoryRange range = module.range();
                module_info info;
                info.path = path;
                info.name = name;
                info.base = range.base_address;
                info.size = range.size;
                matches.push_back(info);
                return true;
            });
            return matches;
        }

        std::vector<module_info> all_modules() {
            std::vector<module_info> modules;
            Gum::Process::enumerate_modules([&](const Gum::Module& module) -> bool {
                const Gum::MemoryRange range = module.range();
                module_info info;
                info.path = module.path();
                info.name = module.name();
                info.base = range.base_address;
                info.size = range.size;
                modules.push_back(info);
                return true;
            });
            return modules;
        }

        // Formats every candidate so the error names what the process *does*
        // have, instead of only what was asked for.
        std::string describe(const std::vector<module_info>& modules) {
            std::string text;
            for (const module_info& module : modules) {
                if (!text.empty()) {
                    text += ", ";
                }
                text += path_of(module);
            }
            return text;
        }

        bool resolve_absolute(const debugger::SymbolEntry& entry, resolved_symbols& out, std::string& error) {
            const std::uint64_t value = entry.rule.value;
            if (value > std::numeric_limits<std::uintptr_t>::max()) {
                error = "symbol `" + entry.name + "`: address " + hex_of(value) + " does not fit this process's pointer width";
                return false;
            }
            const auto address = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(value));
            if (is_data_symbol(entry.name)) {
                if (!vm::mapping_readable(address, 1)) {
                    error = "symbol `" + entry.name + "`: " + hex_of(value) + " is not in a readable mapping";
                    return false;
                }
            } else if (!vm::mapping_executable(address)) {
                error = "symbol `" + entry.name + "`: " + hex_of(value) +
                        " is not in an executable mapping (absolute addresses are process "
                        "specific: re-derive them after every restart)";
                return false;
            }
            out.entries[entry.name] = static_cast<intptr_t>(value);
            return true;
        }

        bool resolve_offset(const debugger::SymbolEntry& entry, const module_info& module, resolved_symbols& out, std::string& error) {
            const std::uint64_t base = reinterpret_cast<std::uintptr_t>(module.base);
            const std::uint64_t rva  = entry.rule.value;
            if (rva >= module.size) {
                error = "symbol `" + entry.name + "`: offset " + hex_of(rva) + " lies outside " + path_of(module) + " (" + hex_of(module.size) + " bytes)";
                return false;
            }
            if (std::numeric_limits<std::uint64_t>::max() - base < rva) {
                error = "symbol `" + entry.name + "`: offset " + hex_of(rva) + " overflows the module base";
                return false;
            }
            const auto address = base + rva;
            if (address > std::numeric_limits<std::uintptr_t>::max()) {
                error = "symbol `" + entry.name + "`: " + hex_of(address) + " does not fit this process's pointer width";
                return false;
            }
            // The base came from Gum, but the page may still be unmapped if the
            // image was partially unmapped after the enumeration.
            const auto* target = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address));
            const bool usable  = is_data_symbol(entry.name) ? vm::mapping_readable(target, 1)
                                                            : vm::mapping_executable(target);
            if (!usable) {
                error = "symbol `" + entry.name + "`: " + hex_of(address) + " is not a usable mapping";
                return false;
            }
            out.entries[entry.name] = static_cast<intptr_t>(address);
            return true;
        }

        bool resolve_pattern(const debugger::SymbolEntry& entry, const std::vector<module_info>& scope, resolved_symbols& out, std::string& error) {
            if (!gum_pattern_valid(entry.rule.pattern)) {
                error = "symbol `" + entry.name + "`: `" + entry.rule.pattern +
                        "` is not a valid Gum pattern (expected hex byte pairs or `??`, "
                        "separated by single spaces)";
                return false;
            }
            std::vector<void*> hits;
            for (const module_info& module : scope) {
                const std::string name = path_of(module);
                with_module(name, [&](const Gum::Module& gum_module) {
                    for (void* hit : Gum::search_module_function(gum_module, entry.rule.pattern.c_str())) {
                        if (std::find(hits.begin(), hits.end(), hit) == hits.end()) {
                            hits.push_back(hit);
                        }
                    }
                });
            }
            if (hits.empty()) {
                error = "symbol `" + entry.name + "`: pattern `" + entry.rule.pattern + "` matched nothing in " + (scope.empty() ? "any loaded module" : "the "
                                                                                                                                                         "selected image");
                return false;
            }
            if (hits.size() > 1) {
                std::string list;
                for (void* hit : hits) {
                    if (!list.empty()) {
                        list += ", ";
                    }
                    list += hex_of(reinterpret_cast<std::uintptr_t>(hit));
                }
                error = "symbol `" + entry.name + "`: pattern `" + entry.rule.pattern + "` matched " + std::to_string(hits.size()) + " locations (" + list + "); a pattern must identify one entry";
                return false;
            }
            out.entries[entry.name] = reinterpret_cast<intptr_t>(hits.front());
            return true;
        }

    }  // namespace

    bool resolve_vm_source(const debugger::BootstrapConfig& config, resolved_symbols& out, std::string& error, symbol_source_result* result) {
        out = resolved_symbols();

        std::vector<debugger::SymbolEntry> symbols = config.lua_symbols;
        if (!config.lua_symbols_file.empty()) {
            std::vector<debugger::SymbolEntry> from_file;
            if (!debugger::load_symbols_file(config.lua_symbols_file, from_file, error)) {
                return false;
            }
            symbols = std::move(from_file);
        }

        // Module selection. The description never loads an image: it selects one
        // that is already mapped, and "not loaded" keeps the late-load path open.
        std::vector<module_info> scope;
        module_info selected {};
        bool has_module = false;
        if (!config.lua_binary.empty()) {
            const std::vector<module_info> matches = select_modules(config.lua_binary);
            if (matches.empty()) {
                if (result) {
                    result->module_missing = true;
                }
                error = "`luaBinary` " + config.lua_binary + " is not loaded in the target process";
                return false;
            }
            if (matches.size() > 1) {
                error = "`luaBinary` " + config.lua_binary + " matches " + std::to_string(matches.size()) + " loaded modules (" + describe(matches) + "); use the full path so exactly one is selected";
                return false;
            }
            selected        = matches.front();
            has_module      = true;
            scope           = matches;
            out.module_path = path_of(selected);
            out.module_base = selected.base;
            out.module_size = selected.size;
        } else {
            scope = all_modules();
        }

        for (const debugger::SymbolEntry& entry : symbols) {
            switch (entry.rule.kind) {
            case debugger::SymbolKind::address:
                if (!resolve_absolute(entry, out, error)) {
                    return false;
                }
                break;
            case debugger::SymbolKind::offset:
                if (!has_module) {
                    error = "symbol `" + entry.name + "`: an offset needs a `luaBinary` to measure it from";
                    return false;
                }
                if (!resolve_offset(entry, selected, out, error)) {
                    return false;
                }
                break;
            case debugger::SymbolKind::pattern:
                if (!resolve_pattern(entry, scope, out, error)) {
                    return false;
                }
                break;
            }
        }

        // Names the description left out come from the selected image, and only
        // from it: a process-wide search would let one library's `lua_gettop`
        // stand in for another's.
        if (has_module) {
            const std::string path = path_of(selected);
            std::vector<std::string> missing;
            std::size_t answered = 0;
            auto fill            = [&](std::string_view name) {
                const std::string text(name);
                if (out.entries.find(text) != out.entries.end()) {
                    return;
                }
                void* address = nullptr;
                with_module(path, [&](const Gum::Module& gum_module) {
                    address = gum_module.find_export_by_name(text.c_str());
                    if (!address) {
                        address = gum_module.find_symbol_by_name(text.c_str());
                    }
                });
                if (address) {
                    out.entries.emplace(text, reinterpret_cast<intptr_t>(address));
                    ++answered;
                } else {
                    missing.emplace_back(text);
                }
            };
#define LDB_FILL_RAW(member, symbol) fill(#symbol);
#define LDB_FILL_ONE(name) fill(#name);
            LDB_EXTERNAL_SYMBOL_NAMES(LDB_FILL_RAW, LDB_FILL_ONE)
#undef LDB_FILL_RAW
#undef LDB_FILL_ONE
            // `complete` means the image *enumerated* its API, which is the only
            // case where an absent symbol tells us something about the version. A
            // stripped or hidden-visibility image has a valid Gum module and an
            // empty symbol list: treating that as complete would make the version
            // ladder fall back to the oldest ABI with confidence.
            out.complete_from_image = answered > 0;
            // Missing entries are not an error here: a description that names only
            // what it needs is legitimate, and the bridge reports the exact gap if
            // its own required set is not covered.
            if (!missing.empty()) {
                std::string list;
                for (const std::string& name : missing) {
                    if (!list.empty()) {
                        list += ", ";
                    }
                    list += name;
                }
                log::info("vm source: {} does not provide {}", path, list);
            }
        }

        return true;
    }

}  // namespace luadebug
