#pragma once
#include "vm/vm.h"

namespace luadebug::eventfree {
    typedef void (*notify)(void* ud, void* ptr);
    void* create(vm::State* L, notify cb, void* ud);
    void destroy(vm::State* L, void* handle);
}
