#pragma once

#include <stddef.h>
#include <stdint.h>

struct thunk;
thunk* thunk_create_hook(intptr_t dbg, intptr_t hook);
thunk* thunk_create_allocf(intptr_t dbg, intptr_t allocf);

#if defined(_WIN32)
#    define THUNK_JIT 1
#else
#    if defined(__ia64__) || defined(__x86_64__)
#        define THUNK_JIT 1
#        if defined(__APPLE__)
#            include <TargetConditionals.h>
#            if TARGET_OS_IPHONE
// The iOS simulator runs x86_64 with a hardened runtime that forbids the
// RW->RX code pages `thunk_posix.inl` relies on.
#                undef THUNK_JIT
#            endif
#        endif
#    elif defined(__aarch64__)
#        if defined(__APPLE__)
#            include <TargetConditionals.h>
#            if !TARGET_OS_IPHONE
#                define THUNK_JIT 1
#            endif
#        else
#            define THUNK_JIT 1
#        endif
#    endif
#endif

#if defined(THUNK_JIT)
#    include "thunk_jit.h"
#else
#    include "thunk_nojit.h"
#endif
