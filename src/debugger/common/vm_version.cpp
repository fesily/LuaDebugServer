#include "common/vm_version.h"

#include <cstring>

namespace luadebug::debugger {

    namespace {

        struct NamedProfile {
            const char* name;
            int id;
        };

        constexpr NamedProfile kProfiles[] = {
            { "lua51", kProfileLua51 },
            { "lua52", kProfileLua52 },
            { "lua53", kProfileLua53 },
            { "lua54", kProfileLua54 },
            { "lua55", kProfileLua55 },
            { "luajit", kProfileLuajit },
        };

        bool starts_with(std::string_view text, std::string_view prefix) {
            return text.substr(0, prefix.size()) == prefix;
        }

        // `$LuaVersion: Lua 5.<minor>...`: one ASCII digit after the known prefix.
        int profile_from_minor_digit(char digit) {
            switch (digit) {
            case '1':
                return kProfileLua51;
            case '2':
                return kProfileLua52;
            case '3':
                return kProfileLua53;
            case '4':
                return kProfileLua54;
            case '5':
                return kProfileLua55;
            default:
                return kProfileUnknown;
            }
        }

    }  // namespace

    int profile_from_name(std::string_view name) {
        if (name.empty() || name == "auto") {
            return kProfileUnknown;
        }
        if (name == "lua-latest") {
            return kProfileLua55;
        }
        for (const NamedProfile& candidate : kProfiles) {
            if (name == candidate.name) {
                return candidate.id;
            }
        }
        return kProfileUnknown;
    }

    const char* profile_id_name(int profile) {
        for (const NamedProfile& candidate : kProfiles) {
            if (profile == candidate.id) {
                return candidate.name;
            }
        }
        return "unknown";
    }

    int profile_from_lua_ident(std::string_view ident) {
        if (!starts_with(ident, "$Lua")) {
            return kProfileUnknown;
        }
        // LuaJIT defines no `lua_ident`; it is recognized by its own symbols
        // (see ProfileEvidence::luajit), so nothing here can see one.
        constexpr std::string_view kModern = "$LuaVersion: Lua 5.";
        if (starts_with(ident, kModern)) {
            const std::size_t digit = kModern.size();
            if (ident.size() <= digit) {
                return kProfileUnknown;
            }
            return profile_from_minor_digit(ident[digit]);
        }
        // 5.1 spells it `$Lua: Lua 5.1.4 ...` (`$Lua: Lua 5.1 ...` in older
        // releases). The bare prefix is not enough: `$Lua: Lua 5.0.3` or
        // `$Lua: garbage` must stay unknown rather than be called 5.1, because the
        // caller acts on this id.
        constexpr std::string_view kLegacy = "$Lua: Lua 5.1";
        if (starts_with(ident, kLegacy)) {
            return kProfileLua51;
        }
        return kProfileUnknown;
    }

    int profile_from_evidence(const ProfileEvidence& evidence) {
        // LuaJIT answers the 5.1 ABI but keeps its own VM: no stock Lua has
        // `luaJIT_setmode`, so this is decisive and takes precedence over the
        // markers LuaJIT happens to share with 5.1+.
        if (evidence.luajit) {
            return kProfileLuajit;
        }
        if (evidence.lua55_only) {
            return kProfileLua55;
        }
        if (!evidence.complete) {
            // A partial table can only prove "5.4 or newer"; it cannot tell 5.4
            // from 5.5, and guessing would call the wrong layout.
            return kProfileUnknown;
        }
        if (evidence.lua54_plus) {
            // Complete evidence: a 5.5 image would have answered one of the
            // 5.5-only symbols above.
            return kProfileLua54;
        }
        if (evidence.lua53_plus) {
            return kProfileLua53;
        }
        if (evidence.lua52_plus) {
            return kProfileLua52;
        }
        // No marker at all, with complete evidence: 5.1 (or a 5.1-compatible
        // VM that already failed the LuaJIT check).
        return kProfileLua51;
    }

}  // namespace luadebug::debugger
