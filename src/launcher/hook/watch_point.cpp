#include <hook/watch_point.h>

namespace luadebug::autoattach {
    watch_point::operator bool() const {
        return !!address;
    }
    bool watch_point::find_symbol(const lua::resolver& resolver) {
        // Exact: a watch point that resolved through a compatibility alias would
        // hook a launcher thunk that the target never calls.
        address = (void*)resolver.find_exact(funcname);
        return !!address;
    }
}
