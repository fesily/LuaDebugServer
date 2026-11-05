#pragma once

// Base64 for the DAP memory requests (and anything else that needs binary in
// JSON). Standard alphabet with padding, no line breaks.

#include <string>
#include <string_view>

namespace luadebug::debugger {

    std::string base64_encode(std::string_view data);
    bool base64_decode(std::string_view text, std::string& out);

}  // namespace luadebug::debugger
