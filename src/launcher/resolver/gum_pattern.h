#pragma once

// Validation of an externally supplied Gum pattern.
//
// `Gum::search_module_function` answers an empty list both for a malformed
// pattern and for a pattern that matches nothing, so the two cannot be told apart
// afterwards. Checking the syntax first is what lets "your pattern is wrong" and
// "your pattern found nothing" be different errors.
//
// Deliberately free of any frida-gum include: the rule is pure text and is
// tested without the library.

#include <string_view>

namespace luadebug {
    // True for two or more space-separated byte tokens, each `??` or a pair of
    // hex digits (a single `?` nibble is accepted, as Gum accepts it).
    bool gum_pattern_valid(std::string_view pattern);
}  // namespace luadebug
