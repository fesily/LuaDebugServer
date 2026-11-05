#pragma once

#include <stdint.h>

struct thunk {
    void* data = 0;
};
#include "vm/vm.h"

intptr_t thunk_get(luadebug::vm::State* L, void* key);
void thunk_set(luadebug::vm::State* L, void* key, intptr_t v);
#define LUADEBUG_DISABLE_THUNK 1
