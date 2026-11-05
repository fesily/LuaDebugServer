#pragma once

// Runtime ABI shared by `luadebug_runtime` (platform-wide session hub) and every
// target-side `luadebug` bridge.
//
// Rules enforced by both sides:
//   * Only C POD crosses the boundary: handles are uint64_t, contexts void*,
//     byte strings are borrowed LdSlice values.
//   * The caller fills `size`/`version` before calling get_api(); a mismatch is
//     rejected instead of writing past the caller's struct.
//   * No allocation produced here is freed on the other side: callback output is
//     consumed through the caller-provided sink.
//   * Command/event payloads are UTF-8 JSON byte strings owned by the caller for
//     the duration of the call.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LD_RUNTIME_ABI_VERSION 1u

// The runtime library hides everything except the single entry point.
#if defined(_WIN32)
#    define LD_RUNTIME_EXPORT __declspec(dllexport)
#else
#    define LD_RUNTIME_EXPORT __attribute__((visibility("default")))
#endif

typedef struct LdSlice {
    const char* data;
    size_t size;
} LdSlice;

typedef uint64_t LdWorkerId;

typedef int32_t LdStatus;

enum {
    LD_STATUS_OK         = 0,
    LD_STATUS_NO_SESSION = 1,
    // pump_worker() found nothing to dispatch: the worker stays idle instead of
    // reporting success for a no-op (the stop loop relies on the difference).
    LD_STATUS_IDLE             = 2,
    LD_STATUS_FAILED           = -1,
    LD_STATUS_ABI_MISMATCH     = -2,
    LD_STATUS_INVALID_ARGUMENT = -3,
    LD_STATUS_NOT_REGISTERED   = -4,
};

typedef void (*LdSink)(void* sink_ctx, LdSlice message);

typedef struct LdBridgeApi {
    uint32_t size;
    uint32_t version;
    void* ctx;
    // Executes one Worker command on the target thread. Responses (if any) are
    // delivered through `sink`; a negative status makes the runtime synthesize
    // the error response for that request.
    LdStatus (*dispatch)(void* ctx, LdSlice command, LdSink sink, void* sink_ctx);
} LdBridgeApi;

typedef struct LdRuntimeApi {
    uint32_t size;
    uint32_t version;
    // Copies config_json and starts (or reuses) the session. With attach_only=1
    // and no session it returns LD_STATUS_NO_SESSION and creates nothing.
    LdStatus (*start_session)(LdSlice config_json, uint32_t attach_only);
    LdStatus (*register_worker)(const LdBridgeApi* bridge, LdWorkerId* out);
    LdStatus (*unregister_worker)(LdWorkerId worker);
    // Must be called from the worker's own target thread. wait_ms=0 does not block.
    LdStatus (*pump_worker)(LdWorkerId worker, uint32_t wait_ms);
    LdStatus (*post_event)(LdWorkerId worker, LdSlice event_json);
} LdRuntimeApi;

// Single exported entry point. `version` must equal LD_RUNTIME_ABI_VERSION.
// out->size/out->version must be pre-filled by the caller and match this header.
LD_RUNTIME_EXPORT LdStatus luadebug_runtime_get_api(uint32_t version, LdRuntimeApi* out);

#ifdef __cplusplus
}
#endif
