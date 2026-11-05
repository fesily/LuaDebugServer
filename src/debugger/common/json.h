#pragma once

// Thin, exception-free wrapper around the vendored nlohmann/json (v3.11.3).
// Every decode returns a status instead of throwing, and no helper guesses
// container kinds: callers must build Json::array() / Json::object() explicitly
// so empty arrays and empty objects stay distinguishable on the wire.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace luadebug::debugger {

    using Json = nlohmann::json;

    // Wire-visible integers are 64-bit; anything wider is rejected at the edge.
    using json_int = std::int64_t;

    enum class ParseStatus {
        ok,
        invalid,
    };

    // Parses a complete JSON document. On failure `error` receives a short
    // message and `out` is left untouched.
    ParseStatus parse_json(std::string_view text, Json& out, std::string& error);

    // Serializes as UTF-8 (no \u escaping), replacing invalid UTF-8 sequences so
    // a broken target string can never abort a response.
    std::string dump_json(const Json& value);

    std::string_view get_string(const Json& value, std::string_view key, std::string_view fallback = {});
    bool has(const Json& value, std::string_view key);
    bool get_bool(const Json& value, std::string_view key, bool fallback = false);
    json_int get_int(const Json& value, std::string_view key, json_int fallback = 0);
    // Returns false when the value is missing, not integral, or out of range.
    bool find_int(const Json& value, std::string_view key, json_int& out);

}  // namespace luadebug::debugger
