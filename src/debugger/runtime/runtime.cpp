// `luadebug_runtime` — the single per-platform/arch session hub.
//
// The target-side `luadebug` bridge and the adapter executable load this
// library and talk to it through the C ABI in runtime/abi.h. Nothing here
// includes a Lua header: the runtime must stay independent of the debuggee's
// Lua version.

#include "runtime/abi.h"

#include <cstring>
#include <string>
#include <vector>

#include "common/json.h"
#include "common/log.h"
#include "session/session_hub.h"

using namespace luadebug::debugger;

namespace {

    LdStatus api_start_session(LdSlice config_json, uint32_t attach_only) {
        if (!config_json.data && config_json.size != 0) {
            return LD_STATUS_INVALID_ARGUMENT;
        }
        std::string_view config(config_json.data ? config_json.data : "", config_json.size);
        return session::SessionHub::instance().start_session(config, attach_only != 0);
    }

    LdStatus api_register_worker(const LdBridgeApi* bridge, LdWorkerId* out) {
        return session::SessionHub::instance().register_worker(bridge, out);
    }

    LdStatus api_unregister_worker(LdWorkerId worker) {
        return session::SessionHub::instance().unregister_worker(worker);
    }

    LdStatus api_pump_worker(LdWorkerId worker, uint32_t wait_ms) {
        return session::SessionHub::instance().pump_worker(worker, wait_ms);
    }

    LdStatus api_post_event(LdWorkerId worker, LdSlice event_json) {
        if (!event_json.data && event_json.size != 0) {
            return LD_STATUS_INVALID_ARGUMENT;
        }
        return session::SessionHub::instance().post_event(
            worker, std::string_view(event_json.data ? event_json.data : "", event_json.size)
        );
    }

}  // namespace

extern "C" {

LD_RUNTIME_EXPORT LdStatus luadebug_runtime_get_api(uint32_t version, LdRuntimeApi* out) {
    if (!out) {
        return LD_STATUS_INVALID_ARGUMENT;
    }
    // The caller states the table size/version it built against; we refuse
    // to write beyond it instead of guessing.
    if (version != LD_RUNTIME_ABI_VERSION || out->version != LD_RUNTIME_ABI_VERSION || out->size != sizeof(LdRuntimeApi)) {
        return LD_STATUS_ABI_MISMATCH;
    }
    out->version           = LD_RUNTIME_ABI_VERSION;
    out->start_session     = &api_start_session;
    out->register_worker   = &api_register_worker;
    out->unregister_worker = &api_unregister_worker;
    out->pump_worker       = &api_pump_worker;
    out->post_event        = &api_post_event;
    return LD_STATUS_OK;
}

}  // extern "C"
