#pragma once

#include <resolver/lua_delayload.h>
#include <resolver/symbol_source.h>

#include <string_view>

namespace luadebug {
    struct lua_resolver : lua::resolver {
        intptr_t find(std::string_view name) const override;
        intptr_t find_export(std::string_view name) const;
        intptr_t find_symbol(std::string_view name) const;
        std::string_view module_name;
        // Externally supplied table, consulted before the module. It is the only
        // source that works when the image exports nothing, and it stays
        // immutable for the whole session: no rule is re-read per hook event.
        const resolved_symbols* external = nullptr;
    };
}