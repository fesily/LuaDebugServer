#pragma once

// C ABI of the bridge for native hosts (the launcher and embedders).
//
// The descriptor names the target VM explicitly. Every part is optional except
// the state: a caller that can hand over a symbol table needs no module handle,
// no binary name, and no exported target symbol at all, which is the only way to
// debug a statically linked or stripped interpreter. Nothing here is C++ and
// nothing implies a process-wide symbol search.

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#    define LDB_BRIDGE_EXPORT __declspec(dllexport)
#else
#    define LDB_BRIDGE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LDB_BRIDGE_ABI_VERSION 2

// One entry of an externally supplied symbol table.
typedef struct LdBridgeSymbol {
    const char* name;
    void* address;
} LdBridgeSymbol;

typedef struct LdBridgeDescriptor {
    uint32_t size;         // sizeof(LdBridgeDescriptor)
    uint32_t abi_version;  // LDB_BRIDGE_ABI_VERSION
    // ABI/layout profile, or -1 to detect it from `symbols`/`module`.
    // Detection needs evidence: a table that only names the API both 5.4 and
    // 5.5 share cannot pick between them, and the call fails asking for an
    // explicit profile rather than guessing.
    int32_t profile;
    uint32_t reserved;
    // Target lua_State*. Required.
    void* state;

    // --- where the C API comes from (at least one of the two) -------------
    // Symbol table. Entries win over anything `module` would answer, and the
    // bridge copies what it needs before returning; the array, the names and
    // `resolver_user` do not have to outlive the call.
    const LdBridgeSymbol* symbols;
    uint32_t symbol_count;
    // Optional module that implements the Lua C API for `state`. Only a
    // source identifier for export lookups: it is never required, and the
    // bridge never falls back to the first module that exports lua_*.
    void* module;
    // Optional symbol lookup. Called as `resolver(resolver_user, name)`; the
    // context is the caller's, never `module` implicitly, so a resolver that
    // does not use the module handle keeps working with `module` unset.
    void* (*resolver)(void* resolver_user, const char* name);
    void* resolver_user;
} LdBridgeDescriptor;

typedef struct LdBridgeOps {
    uint32_t size;
    uint32_t abi_version;
    // Registers the worker and joins the session. Returns 1 on success.
    int (*start)(void* ctx, const char* config_json, uint32_t size, uint32_t attach_only);
    // Drains queued commands; 0 once the worker is gone.
    int (*pump)(void* ctx, uint32_t wait_ms);
    // Leaves the session (the VM keeps running).
    void (*stop)(void* ctx);
    int (*running)(void* ctx);
    uint64_t (*worker_id)(void* ctx);
    // Drops the context. The target VM is not closed.
    void (*release)(void* ctx);
    // Reason the last failed call failed (valid until the next call).
    const char* (*last_error)(void* ctx);
    // ABI/layout profile this context actually bound (-1 when none). A host
    // that let the bridge detect the profile reads it here.
    int32_t (*profile)(void* ctx);
} LdBridgeOps;

// Binds `descriptor` and returns its operation table. `*ctx` receives the
// opaque context every operation takes; the caller must pass it to
// `release` when done. Returns null on failure and `*error` then points at a
// human-readable reason (missing API entry, conflicting version, ...).
LDB_BRIDGE_EXPORT const LdBridgeOps* luadebug_bridge_bind(const LdBridgeDescriptor* descriptor, void** ctx, const char** error);

// Binds `descriptor` and pushes the bridge's module table onto `state` (the
// same table `luaopen_luadebug_<profile>` returns). This is how a host that
// already resolved the symbols hands the debugger to the target script
// without any exported target symbol. Returns 1 on success; on failure it
// returns 0, leaves `state` balanced, and `*error` describes the reason. No
// error crosses this boundary as a longjmp.
LDB_BRIDGE_EXPORT int luadebug_bridge_open(const LdBridgeDescriptor* descriptor, const char** error);

// Profile name of the running module, for diagnostics ("lua54", ...).
LDB_BRIDGE_EXPORT const char* luadebug_bridge_profile(void);

#ifdef __cplusplus
}
#endif
