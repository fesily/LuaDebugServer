// Normalized C API for this family: the implementation is shared, the
// headers come from the profile.
#include "vm/backends/luajit/internal.h"
#include "vm/backends/api_impl.inc"

// Self-registration: the profile is pinned by the build so the shared core never
// has to name a backend it may not carry.
namespace {
    struct BackendRegistrar {
        BackendRegistrar() {
            ld_backend_register();
        }
    };
    BackendRegistrar g_backend_registrar;
}
