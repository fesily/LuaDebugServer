#include "common/vm_source.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "common/path_utf8.h"
#include "luadebug/vm/symbol_names.h"

namespace luadebug::debugger {

    namespace {

        using Json = nlohmann::json;

        const char* const kSymbolNames[] = {
#define LDB_SYMBOL_NAME_RAW(member, symbol) #symbol,
#define LDB_SYMBOL_NAME_ONE(name) #name,
            LDB_EXTERNAL_SYMBOL_NAMES(LDB_SYMBOL_NAME_RAW, LDB_SYMBOL_NAME_ONE)
#undef LDB_SYMBOL_NAME_RAW
#undef LDB_SYMBOL_NAME_ONE
        };

        // Hexadecimal, unsigned, no sign, no radix prefix, no trailing garbage.
        // Decimal is deliberately not accepted: `0x1000` and `0X1000` are the
        // documented forms, and `"4096"` meaning a different number depending on
        // the reader is exactly the kind of ambiguity that silently binds the
        // wrong address.
        bool parse_hex(std::string_view text, std::uint64_t& out) {
            if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
                return false;
            }
            std::uint64_t value = 0;
            for (std::size_t i = 2; i < text.size(); ++i) {
                const char c   = text[i];
                unsigned digit = 0;
                if (c >= '0' && c <= '9') {
                    digit = static_cast<unsigned>(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    digit = static_cast<unsigned>(c - 'a') + 10u;
                } else if (c >= 'A' && c <= 'F') {
                    digit = static_cast<unsigned>(c - 'A') + 10u;
                } else {
                    return false;
                }
                // Reject anything that would wrap: a value above the pointer
                // width is a producer bug, not a large address.
                if (value > (UINT64_MAX >> 4)) {
                    return false;
                }
                value = (value << 4) | digit;
            }
            out = value;
            return true;
        }

        bool read_rule_string(const Json& entry, const char* key, std::string& out, std::string& error, const std::string& name) {
            auto it = entry.find(key);
            if (it == entry.end()) {
                return false;
            }
            if (!it->is_string()) {
                error = "symbol `" + name + "`: `" + key + "` must be a string";
                return false;
            }
            const auto& text = it->get_ref<const std::string&>();
            if (text.empty()) {
                error = "symbol `" + name + "`: `" + key + "` must not be empty";
                return false;
            }
            if (text.find('\0') != std::string::npos) {
                error = "symbol `" + name + "`: `" + key + "` must not contain NUL";
                return false;
            }
            out = text;
            return true;
        }

        bool parse_rule(const Json& entry, const std::string& name, SymbolRule& rule, std::string& error) {
            if (!entry.is_object()) {
                error = "symbol `" + name + "` must be an object";
                return false;
            }
            std::string address;
            std::string offset;
            std::string pattern;
            const bool has_address = read_rule_string(entry, "address", address, error, name);
            if (!error.empty()) {
                return false;
            }
            const bool has_offset = read_rule_string(entry, "offset", offset, error, name);
            if (!error.empty()) {
                return false;
            }
            const bool has_pattern = read_rule_string(entry, "pattern", pattern, error, name);
            if (!error.empty()) {
                return false;
            }
            const int chosen = (has_address ? 1 : 0) + (has_offset ? 1 : 0) + (has_pattern ? 1 : 0);
            if (chosen == 0) {
                error = "symbol `" + name + "` must name exactly one of `address`, `offset` or `pattern`";
                return false;
            }
            if (chosen > 1) {
                error = "symbol `" + name + "` names more than one of `address`, `offset` and `pattern`";
                return false;
            }
            if (has_pattern) {
                rule.kind    = SymbolKind::pattern;
                rule.pattern = pattern;
                return true;
            }
            const std::string& text = has_address ? address : offset;
            std::uint64_t value     = 0;
            if (!parse_hex(text, value)) {
                error = "symbol `" + name + "`: `" + (has_address ? "address" : "offset") + "` must be a hexadecimal string such as \"0x1000\"";
                return false;
            }
            rule.kind  = has_address ? SymbolKind::address : SymbolKind::offset;
            rule.value = value;
            return true;
        }

    }  // namespace

    bool symbol_name_known(std::string_view name) {
        for (const char* candidate : kSymbolNames) {
            if (name == candidate) {
                return true;
            }
        }
        return false;
    }

    bool parse_symbols(const Json& doc, std::vector<SymbolEntry>& out, std::string& error) {
        out.clear();
        // `parse_rule` reports an absent key by not touching `error`, so a stale
        // message from an earlier call would make every rule look invalid.
        error.clear();
        if (!doc.is_object()) {
            error = "`luaSymbols` must be an object";
            return false;
        }
        std::vector<SymbolEntry> parsed;
        parsed.reserve(doc.size());
        for (auto it = doc.begin(); it != doc.end(); ++it) {
            const std::string name = it.key();
            if (name.empty()) {
                error = "`luaSymbols` must not contain an empty symbol name";
                return false;
            }
            if (name.find('\0') != std::string::npos) {
                error = "`luaSymbols` must not contain NUL";
                return false;
            }
            if (!symbol_name_known(name)) {
                error = "`luaSymbols` names unknown symbol `" + name + "`";
                return false;
            }
            SymbolEntry entry;
            entry.name = name;
            if (!parse_rule(it.value(), name, entry.rule, error)) {
                return false;
            }
            parsed.push_back(std::move(entry));
        }
        // No duplicate check: `nlohmann::json` is a map, so a repeated key is
        // collapsed while parsing (last wins) and cannot reach this function.
        out = std::move(parsed);
        error.clear();
        return true;
    }

    nlohmann::json dump_symbols(const std::vector<SymbolEntry>& symbols) {
        Json doc = Json::object();
        for (const SymbolEntry& entry : symbols) {
            const std::string hex = "0x" + [&] {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(entry.rule.value));
                return std::string(buffer);
            }();
            switch (entry.rule.kind) {
            case SymbolKind::address:
                doc[entry.name] = { { "address", hex } };
                break;
            case SymbolKind::offset:
                doc[entry.name] = { { "offset", hex } };
                break;
            case SymbolKind::pattern:
                doc[entry.name] = { { "pattern", entry.rule.pattern } };
                break;
            }
        }
        return doc;
    }

    bool load_symbols_file(const std::string& path, std::vector<SymbolEntry>& out, std::string& error) {
        // The path comes from outside and the read happens *inside the debuggee*:
        // an unbounded `rdbuf()` on a FIFO blocks it forever and on a huge file
        // exhausts it. A symbol table is kilobytes, so a few MiB is generous.
        constexpr std::size_t kMaxSymbolsFileBytes = 4u * 1024u * 1024u;
        std::string text;
        {
            const fs::path file_path = path_from_utf8(path);
            std::error_code ec;
            if (file_path.empty() || !fs::is_regular_file(file_path, ec) || ec) {
                error = "`luaSymbolsFile` " + path + " is not a readable regular file";
                return false;
            }
            const auto size = fs::file_size(file_path, ec);
            if (ec) {
                error = "cannot stat `luaSymbolsFile` " + path;
                return false;
            }
            if (size > kMaxSymbolsFileBytes) {
                error = "`luaSymbolsFile` " + path + " is larger than " + std::to_string(kMaxSymbolsFileBytes) + " bytes";
                return false;
            }
            std::ifstream file(file_path, std::ios::binary);
            if (!file) {
                error = "cannot read `luaSymbolsFile` " + path;
                return false;
            }
            text.resize(static_cast<std::size_t>(size));
            file.read(text.data(), static_cast<std::streamsize>(text.size()));
            if (static_cast<std::size_t>(file.gcount()) != text.size()) {
                error = "cannot read `luaSymbolsFile` " + path;
                return false;
            }
        }
        if (text.find('\0') != std::string::npos) {
            error = "`luaSymbolsFile` " + path + " contains NUL";
            return false;
        }
        auto parsed = Json::parse(text.begin(), text.end(), nullptr, false);
        if (parsed.is_discarded()) {
            error = "`luaSymbolsFile` " + path + " is not valid json";
            return false;
        }
        return parse_symbols(parsed, out, error);
    }

    bool save_symbols_file(const std::string& path, const std::vector<SymbolEntry>& symbols, std::string& error) {
        const std::string text = dump_symbols(symbols).dump(-1, ' ', false, Json::error_handler_t::replace);
        std::ofstream file(path_from_utf8(path), std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot write `luaSymbolsFile` " + path;
            return false;
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        const bool written = static_cast<bool>(file);
        file.close();
        if (!written || file.fail()) {
            // A truncated file would only surface later as "not valid json" inside
            // the target, so the failure belongs here.
            error = "cannot write `luaSymbolsFile` " + path;
            return false;
        }
        return true;
    }

}  // namespace luadebug::debugger
