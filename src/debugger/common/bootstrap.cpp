#include "common/bootstrap.h"

#include <nlohmann/json.hpp>

#include "common/vm_version.h"

namespace luadebug::debugger {

    namespace {

        using Json = nlohmann::json;

        // A decoded NUL cannot travel through any transport we use (C strings on
        // the frida side, Lua strings and getenv on the target side), so reject it
        // at the edge instead of silently truncating a path.
        bool has_embedded_nul(std::string_view text) {
            return text.find('\0') != std::string_view::npos;
        }

        bool read_string(const Json& doc, const char* key, bool required, std::string& out, std::string& error) {
            auto it = doc.find(key);
            if (it == doc.end()) {
                if (required) {
                    error = std::string("`") + key + "` is required";
                    return false;
                }
                return true;
            }
            if (!it->is_string()) {
                error = std::string("`") + key + "` must be a string";
                return false;
            }
            const auto& text = it->get_ref<const std::string&>();
            if (text.empty()) {
                error = std::string("`") + key + "` must not be empty";
                return false;
            }
            if (has_embedded_nul(text)) {
                error = std::string("`") + key + "` must not contain NUL";
                return false;
            }
            out = text;
            return true;
        }

        bool read_bool(const Json& doc, const char* key, bool& out, std::string& error) {
            auto it = doc.find(key);
            if (it == doc.end()) {
                return true;
            }
            if (!it->is_boolean()) {
                error = std::string("`") + key + "` must be a boolean";
                return false;
            }
            out = it->get<bool>();
            return true;
        }

    }  // namespace

    bool parse_bootstrap(std::string_view text, BootstrapConfig& out, std::string& error) {
        if (text.empty()) {
            error = "bootstrap config is empty";
            return false;
        }
        auto parsed = Json::parse(text.begin(), text.end(), nullptr, false);
        if (parsed.is_discarded()) {
            error = "bootstrap config is not valid json";
            return false;
        }
        if (!parsed.is_object()) {
            error = "bootstrap config must be an object";
            return false;
        }

        BootstrapConfig config;

        if (auto it = parsed.find("mode"); it != parsed.end()) {
            if (!it->is_string()) {
                error = "`mode` must be a string";
                return false;
            }
            const auto& mode = it->get_ref<const std::string&>();
            if (mode == "launch") {
                config.mode = BootstrapMode::launch;
            } else if (mode == "attach") {
                config.mode = BootstrapMode::attach;
            } else if (mode.empty()) {
                error = "`mode` must not be empty";
                return false;
            } else {
                error = "`mode` must be \"launch\" or \"attach\", got \"" + mode + "\"";
                return false;
            }
        }

        if (!read_string(parsed, "root", false, config.root, error) || !read_string(parsed, "address", false, config.address, error) || !read_string(parsed, "luaVersion", false, config.lua_version, error) || !read_string(parsed, "platform", false, config.platform, error) || !read_bool(parsed, "client", config.client, error) || !read_bool(parsed, "ansi", config.ansi, error)) {
            return false;
        }

        // `auto` is what a "let the target decide" setting expands to; it is not
        // a profile, so it clears the constraint instead of being rejected by the
        // binding later. Anything else must name a real ABI: an unknown name would
        // otherwise be indistinguishable from "unspecified" and silently downgrade
        // an explicit constraint into auto-detection.
        if (config.lua_version == "auto") {
            config.lua_version.clear();
        } else if (!config.lua_version.empty() && profile_from_name(config.lua_version) == kProfileUnknown) {
            error = "`luaVersion` names an unknown Lua ABI: \"" + config.lua_version + "\" (expected auto, lua51..lua55, lua-latest or luajit)";
            return false;
        }

        if (!read_string(parsed, "luaBinary", false, config.lua_binary, error) || !read_string(parsed, "luaSymbolsFile", false, config.lua_symbols_file, error)) {
            return false;
        }
        if (auto it = parsed.find("luaSymbols"); it != parsed.end()) {
            std::vector<SymbolEntry> symbols;
            if (!parse_symbols(*it, symbols, error)) {
                return false;
            }
            config.lua_symbols = std::move(symbols);
        }
        if (!config.lua_symbols.empty() && !config.lua_symbols_file.empty()) {
            error = "`luaSymbols` and `luaSymbolsFile` are mutually exclusive";
            return false;
        }

        out = std::move(config);
        error.clear();
        return true;
    }

    std::string dump_bootstrap(const BootstrapConfig& config) {
        Json doc    = Json::object();
        doc["mode"] = config.mode == BootstrapMode::launch ? "launch" : "attach";
        if (!config.address.empty()) {
            doc["address"] = config.address;
        }
        if (config.client) {
            doc["client"] = true;
        }
        if (!config.lua_version.empty()) {
            doc["luaVersion"] = config.lua_version;
        }
        if (config.ansi) {
            doc["ansi"] = true;
        }
        if (!config.platform.empty()) {
            doc["platform"] = config.platform;
        }
        if (!config.root.empty()) {
            doc["root"] = config.root;
        }
        if (!config.lua_binary.empty()) {
            doc["luaBinary"] = config.lua_binary;
        }
        if (!config.lua_symbols.empty()) {
            doc["luaSymbols"] = dump_symbols(config.lua_symbols);
        }
        if (!config.lua_symbols_file.empty()) {
            doc["luaSymbolsFile"] = config.lua_symbols_file;
        }
        auto text = doc.dump(-1, ' ', false, Json::error_handler_t::replace);
        if (text.size() > kMaxBootstrapBytes) {
            return {};
        }
        // Round-trip through the parser both carriers use: a field that cannot be
        // delivered (an embedded NUL, for instance) must fail here instead of being
        // injected and silently rejected by the launcher.
        BootstrapConfig check;
        std::string error;
        if (!parse_bootstrap(text, check, error)) {
            return {};
        }
        return text;
    }

}  // namespace luadebug::debugger
