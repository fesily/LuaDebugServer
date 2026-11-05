#include "thunk/thunk.cpp"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
struct HHI {
    int a = 0x1;
};

int add(HHI* h, void* a, void* b) {
    h->a = 2;
    assert(a == (void*)0x1);
    assert(b == (void*)0x2);
    return 1;
}

void* add1(HHI* h, void* a, size_t b, size_t c) {
    h->a = 3;
    assert(a == (void*)0x1);
    assert(b == 2);
    assert(c == 3);
    return (void*)4;
}

namespace {

    int failures = 0;

    // `assert` compiles to nothing under `-DNDEBUG`, which is how these tests are
    // built, so every check reports through here and the exit status is the signal.
    void check(bool condition, const char* what) {
        if (!condition) {
            printf("FAIL: %s\n", what);
            ++failures;
        }
    }

    // The allocator thunk's real use (`eventfree::create`): the generated code
    // replaces the first argument with the debug context and forwards the rest to a
    // Lua allocator. Routing a real `lua_State`'s allocations through it is what
    // exposed a misaligned call into glibc's malloc on x86_64 - the synthetic
    // argument checks cannot see a stack alignment fault.
    struct trace {
        lua_Alloc original    = nullptr;
        void* original_ud     = nullptr;
        unsigned long calls   = 0;
        unsigned long frees   = 0;
    };

    // Used by the x86_64 alignment probe (declared with C linkage below).
    void* probe_target_impl(void* ctx, void* ptr, size_t osize, size_t nsize) {
        auto* state = static_cast<trace*>(ctx);
        ++state->calls;
        return state->original(state->original_ud, ptr, osize, nsize);
    }

    void* traced_alloc(void* ctx, void* ptr, size_t osize, size_t nsize) {
        auto* state = static_cast<trace*>(ctx);
        ++state->calls;
        if (ptr != nullptr && nsize == 0) {
            ++state->frees;
        }
        return state->original(state->original_ud, ptr, osize, nsize);
    }

    int run_chunk(lua_State* L, const char* chunk) {
        if (luaL_loadbuffer(L, chunk, strlen(chunk), "=(thunk)") != 0) {
            printf("FAIL: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            return 1;
        }
        if (lua_pcall(L, 0, 1, 0) != 0) {
            printf("FAIL: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            return 1;
        }
        lua_pop(L, 1);
        return 0;
    }

} // namespace

#if defined(__x86_64__)
// Deterministic check of the generated allocator thunk's calling convention: SysV
// enters a function with `rsp % 16 == 8`, and a thunk that aligned the stack for its
// own call must restore that invariant for the callee. A toy allocator tolerates the
// violation, a real one (glibc's malloc) does not always - which is how it surfaced
// in CI as a segfault on x86_64 only. C linkage and no namespace: the asm below
// names these symbols literally. Naked, so `rsp` is read before any prologue.
// Assembler symbol names are literal: Mach-O prefixes C symbols with `_`, ELF does
// not, so the probe has to spell the platform's form.
#if defined(__APPLE__)
#    define THUNK_ASM_SYMBOL(name) "_" name
#else
#    define THUNK_ASM_SYMBOL(name) name
#endif

extern "C" {
unsigned char thunk_test_entry_aligned = 0; // NOLINT: written from asm
void* thunk_test_probe_impl(void* ctx, void* ptr, size_t osize, size_t nsize);

void* thunk_test_probe_impl(void* ctx, void* ptr, size_t osize, size_t nsize) {
    return probe_target_impl(ctx, ptr, osize, nsize);
}

__attribute__((naked)) void* thunk_test_probe(void*, void*, size_t, size_t) {
    __asm__ volatile("movq %rsp, %rax\n\t"
                     "andq $15, %rax\n\t"
                     "cmpq $8, %rax\n\t"
                     "sete " THUNK_ASM_SYMBOL("thunk_test_entry_aligned") "(%rip)\n\t"
                     "jmp " THUNK_ASM_SYMBOL("thunk_test_probe_impl") "\n\t");
}
}
#endif

#undef THUNK_ASM_SYMBOL

int main() {
    HHI hhi;
    auto* thunk = thunk_create_hook((intptr_t)&hhi, (intptr_t)&add);
    check(thunk != nullptr, "hook thunk is created");
    // `lua_Hook` is void: the hook thunk restores the register it used for stack
    // alignment, so the callee's return value is deliberately discarded. Only the
    // side effect on the forwarded context is observable.
    (void)((int (*)(void* a, void* b))thunk->data)((void*)0x1, (void*)0x2);
    check(hhi.a == 2, "hook thunk replaced the context register");

    auto* thunk1 = thunk_create_allocf((intptr_t)&hhi, (intptr_t)&add1);
    check(thunk1 != nullptr, "allocator thunk is created");
    auto ret1 = ((void* (*)(void* ud, void* a, unsigned long b, unsigned long c))thunk1->data)(
        (void*)0x99 /* ud: replaced by the thunk */, (void*)0x1, 2, 3);
    check(hhi.a == 3, "allocator thunk replaced the context register");
    check(ret1 == (void*)4, "allocator thunk returned through");

    // Drive real allocations through the allocator thunk.
    lua_State* L = luaL_newstate();
    check(L != nullptr, "a Lua state is created");
    if (L != nullptr) {
        // `string.rep` (and the string metatable behind `'x':rep`) comes from the
        // standard library, and the allocator must be replaced *after* it is open so
        // the library loading itself does not race the install.
        luaL_openlibs(L);
    }
    if (L != nullptr) {
        trace state;
        state.original = lua_getallocf(L, &state.original_ud);
        auto* live = thunk_create_allocf((intptr_t)&state, (intptr_t)&traced_alloc);
        check(live != nullptr, "a live allocator thunk is created");
        if (live != nullptr) {
            lua_setallocf(L, reinterpret_cast<lua_Alloc>(live->data), state.original_ud);
            const int status = run_chunk(L, "local t = {} "
                                            "for i = 1, 4000 do t[i] = ('x'):rep(64) .. i end "
                                            "for i = 1, 2000 do t[i] = nil end "
                                            "collectgarbage() "
                                            "return #t");
            check(status == 0, "the script ran with the allocator thunk installed");
            check(state.calls > 0, "the allocator thunk was used");
#if defined(__x86_64__)
            // Same allocation path, but through a callee that records the stack
            // alignment it was entered with.
            thunk_test_entry_aligned = 0;
            auto* probe = thunk_create_allocf((intptr_t)&state, (intptr_t)&thunk_test_probe);
            if (probe != nullptr) {
                lua_setallocf(L, reinterpret_cast<lua_Alloc>(probe->data), state.original_ud);
                (void)run_chunk(L, "local t = {} for i = 1, 500 do t[i] = ('y'):rep(32) .. i end "
                                   "return #t");
                lua_setallocf(L, state.original, state.original_ud);
                delete probe;
            }
            check(thunk_test_entry_aligned == 1,
                  "the allocator thunk called its target with a 16-byte aligned stack");
#endif
            // Restore before closing: teardown must not call through a thunk whose
            // context is about to disappear.
            lua_setallocf(L, state.original, state.original_ud);
            delete live;
        }
        lua_close(L);
    }

    if (failures == 0) {
        printf("thunk: all checks passed\n");
        return 0;
    }
    printf("thunk: %d failure(s)\n", failures);
    return 1;
}