#pragma once

#include <string>

namespace luadebug::autoattach {
    // Reports an image mapped later in this process; true means the caller consumed
    // it and the watch stops.
    using loaded_t = bool (*)(const std::string&);

    // Watches this process for images mapped after this call returns, invoking
    // `loaded` from a worker thread with each new image's path. `loaded` returning
    // true ends the watch: no further call happens until the next install.
    //
    // One watch exists per process: a later call replaces the callback and re-arms
    // a watch that ended. The loader subscription itself is permanent.
    //
    // Images mapped before the call are never reported, and an image mapped while
    // the watch is being installed can be absorbed by the registry's own
    // activation: the caller must re-run its enumeration after this returns true
    // rather than treat the watch as the only source. Returns false only when the
    // loader subscription could not be installed at all.
    bool wait_module(loaded_t loaded);
}
