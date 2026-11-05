#pragma once

// Turning an externally supplied VM description into addresses.
//
// This is the only place allowed to interpret `luaBinary`/`luaSymbols`: the
// result is an immutable table that every consumer (the launcher's own C API
// subset, its watch points, the bridge handoff) reads, so no rule is re-scanned
// per hook event or per VM.
//
// Rules are strict on purpose. A pattern that matches twice, an RVA outside the
// module it was measured from, an address in an unmapped page: each of those is
// reported and fails the binding. Taking the first hit would bind a plausible
// but wrong function and corrupt the debuggee later.

#include <common/bootstrap.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace luadebug {

    struct resolved_symbols {
        // Selected image, when the description named one that is loaded.
        std::string module_path;
        void* module_base       = nullptr;
        std::size_t module_size = 0;
        // name -> absolute address in this process.
        std::map<std::string, intptr_t, std::less<>> entries;
        // True when the image's own export/symbol list was consulted, which means
        // an absent symbol is information about the image rather than a hole in a
        // partial table (version detection depends on this distinction).
        bool complete_from_image = false;

        bool valid() const {
            return module_base != nullptr;
        }
        intptr_t find(std::string_view name) const {
            auto it = entries.find(name);
            return it == entries.end() ? 0 : it->second;
        }
        bool has(std::string_view name) const {
            return find(name) != 0;
        }
    };

    struct symbol_source_result {
        // `module_missing` distinguishes "the named image is not loaded yet" (the
        // existing late-load machinery may still succeed) from a real error.
        bool module_missing = false;
    };

    // Resolves `source` against the modules currently mapped in this process.
    // Returns false and fills `error` when a rule is invalid, a pattern is not
    // unique, an offset leaves its module, or a required symbol is absent.
    bool resolve_vm_source(const debugger::BootstrapConfig& config, resolved_symbols& out, std::string& error, symbol_source_result* result = nullptr);

    // Validates Gum pattern syntax without scanning. Exposed so a malformed
    // pattern is rejected before any module is walked.
    bool gum_pattern_valid(std::string_view pattern);

}  // namespace luadebug
