#pragma once

// Single native definition of the adapter's DAP capabilities.
// Replaces extension/script/common/capabilities.lua so the frontend and the
// session layer cannot drift apart.

#include "common/json.h"

namespace luadebug::debugger {

    const Json& capabilities();

}  // namespace luadebug::debugger
