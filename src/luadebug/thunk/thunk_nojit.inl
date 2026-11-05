// Fallback thunk layer for platforms without hook thunks: the context is kept in
// the target's registry, addressed through the bound VM.

#include "thunk_nojit.h"

intptr_t thunk_get(luadebug::vm::State* L, void* key) {
    L->rawgetp(luadebug::vm::idx::registry, key);
    if (L->type(-1) != luadebug::vm::type::lightuserdata) {
        L->pop(1);
        return 0;
    }
    intptr_t v = (intptr_t)L->touserdata(-1);
    L->pop(1);
    return v;
}

void thunk_set(luadebug::vm::State* L, void* key, intptr_t v) {
    L->pushlightuserdata((void*)v);
    L->rawsetp(luadebug::vm::idx::registry, key);
}

thunk* thunk_create_hook(intptr_t dbg, intptr_t hook) {
    thunk* t = new thunk;
    t->data  = (void*)hook;
    return t;
}
