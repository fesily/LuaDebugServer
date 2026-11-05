#include "common/json.h"

namespace luadebug::debugger {

    ParseStatus parse_json(std::string_view text, Json& out, std::string& error) {
        auto parsed = Json::parse(text.begin(), text.end(), nullptr, false);
        if (parsed.is_discarded()) {
            error = "invalid json";
            return ParseStatus::invalid;
        }
        if (!parsed.is_object() && !parsed.is_array()) {
            error = "json document must be an object or array";
            return ParseStatus::invalid;
        }
        out = std::move(parsed);
        return ParseStatus::ok;
    }

    std::string dump_json(const Json& value) {
        return value.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    bool has(const Json& value, std::string_view key) {
        return value.is_object() && value.contains(key);
    }

    std::string_view get_string(const Json& value, std::string_view key, std::string_view fallback) {
        if (!value.is_object()) {
            return fallback;
        }
        auto it = value.find(key);
        if (it == value.end() || !it->is_string()) {
            return fallback;
        }
        return it->get_ref<const std::string&>();
    }

    bool get_bool(const Json& value, std::string_view key, bool fallback) {
        if (!value.is_object()) {
            return fallback;
        }
        auto it = value.find(key);
        if (it == value.end() || !it->is_boolean()) {
            return fallback;
        }
        return it->get<bool>();
    }

    bool find_int(const Json& value, std::string_view key, json_int& out) {
        if (!value.is_object()) {
            return false;
        }
        auto it = value.find(key);
        if (it == value.end()) {
            return false;
        }
        if (it->is_number_integer()) {
            out = it->get<json_int>();
            return true;
        }
        if (it->is_number_unsigned()) {
            auto raw = it->get<std::uint64_t>();
            if (raw > static_cast<std::uint64_t>(INT64_MAX)) {
                return false;
            }
            out = static_cast<json_int>(raw);
            return true;
        }
        if (it->is_number_float()) {
            double d = it->get<double>();
            if (d != static_cast<double>(static_cast<json_int>(d))) {
                return false;
            }
            auto narrowed = static_cast<json_int>(d);
            if (static_cast<double>(narrowed) != d) {
                return false;
            }
            out = narrowed;
            return true;
        }
        return false;
    }

    json_int get_int(const Json& value, std::string_view key, json_int fallback) {
        json_int result = 0;
        if (!find_int(value, key, result)) {
            return fallback;
        }
        return result;
    }

}  // namespace luadebug::debugger
