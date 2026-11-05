#include <resolver/gum_pattern.h>

namespace luadebug {
    namespace {
        bool is_hex_digit(char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }
    }  // namespace

    bool gum_pattern_valid(std::string_view pattern) {
        if (pattern.empty()) {
            return false;
        }
        std::size_t tokens = 0;
        std::size_t i      = 0;
        while (i < pattern.size()) {
            const std::size_t start = i;
            while (i < pattern.size() && pattern[i] != ' ') {
                ++i;
            }
            const std::string_view token = pattern.substr(start, i - start);
            if (token.size() != 2) {
                return false;
            }
            for (char c : token) {
                if (c != '?' && !is_hex_digit(c)) {
                    return false;
                }
            }
            // A token needs at least one known nibble: `?x` pins the low half,
            // `??` is a full wildcard byte, and a token with no digit at all is
            // only the wildcard.
            if (token != "??" && !is_hex_digit(token[0]) && !is_hex_digit(token[1])) {
                return false;
            }
            ++tokens;
            // Exactly one separator; a double space is a typo, not a wildcard, and
            // Gum would read the pattern differently. A separator at the very end
            // is equally wrong.
            if (i < pattern.size()) {
                ++i;
                if (i == pattern.size() || pattern[i] == ' ') {
                    return false;
                }
            }
        }
        // A one-byte pattern matches everywhere, which would make the uniqueness
        // rule meaningless.
        return tokens >= 2;
    }

}  // namespace luadebug
