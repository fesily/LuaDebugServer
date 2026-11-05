#include "util/refvalue.h"

#include <cassert>
#include <cstring>
#include <utility>

namespace luadebug::refvalue {
    // Reads a value the debugger itself is inspecting: hooks must not fire while
    // the debugger drives the target's own interpreter.
    static int debug_pcall(vm::State* hL, int nargs, int nresults, int errfunc) {
        const bool was_enabled = hL->allowhook();
        hL->set_allowhook(false);
        const int ok = hL->pcall(nargs, nresults, errfunc);
        if (was_enabled) {
            hL->set_allowhook(true);
        }
        return ok;
    }

    template <typename T>
    int eval(T&, vm::State*, value*);

    template <>
    int eval<FRAME_LOCAL>(FRAME_LOCAL& v, vm::State* hL, value*) {
        vm::FrameInfo ar;
        if (hL->getstack(v.frame, &ar) == 0)
            return vm::type::none;
        const char* name = hL->getlocal(&ar, v.n);
        if (name) {
            return hL->type(-1);
        }
        return vm::type::none;
    }

    template <>
    int eval<FRAME_FUNC>(FRAME_FUNC& v, vm::State* hL, value*) {
        vm::FrameInfo ar;
        if (hL->getstack(v.frame, &ar) == 0)
            return vm::type::none;
        if (hL->getinfo("f", &ar) == 0)
            return vm::type::none;
        return vm::type::function;
    }

    template <>
    int eval<GLOBAL>(GLOBAL& v, vm::State* hL, value*) {
        hL->pushvalue(vm::idx::globals);
        return vm::type::table;
    }

    template <>
    int eval<REGISTRY>(REGISTRY& v, vm::State* hL, value*) {
        switch (v.type) {
        case REGISTRY_TYPE::REGISTRY:
            hL->pushvalue(vm::idx::registry);
            return vm::type::table;
        case REGISTRY_TYPE::DEBUG_REF:
            return hL->getfield(vm::idx::registry, "__debugger_ref");
        case REGISTRY_TYPE::DEBUG_WATCH:
            return hL->getfield(vm::idx::registry, "__debugger_watch");
        default:
            std::unreachable();
        }
    }

    template <>
    int eval<STACK>(STACK& v, vm::State* hL, value*) {
        hL->pushvalue(v.index);
        return hL->type(-1);
    }

    template <>
    int eval<UPVALUE>(UPVALUE& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t == vm::type::none)
            return vm::type::none;
        if (t != vm::type::function) {
            hL->pop(1);
            return vm::type::none;
        }
        if (hL->getupvalue(-1, v.n)) {
            hL->replace(-2);
            return hL->type(-1);
        } else {
            hL->pop(1);
            return vm::type::none;
        }
    }

    template <>
    int eval<METATABLE>(METATABLE& v, vm::State* hL, value* parent) {
        switch (v.type) {
        case vm::type::nil:
            hL->pushnil();
            break;
        case vm::type::boolean:
            hL->pushboolean(0);
            break;
        case vm::type::number:
            hL->pushinteger(0);
            break;
        case vm::type::string:
            hL->pushstring("");
            break;
        case vm::type::lightuserdata:
            hL->pushlightuserdata(NULL);
            break;
        case vm::type::table:
        case vm::type::userdata: {
            int t = eval(parent, hL);
            if (t == vm::type::none)
                return vm::type::none;
            if (t != vm::type::table && t != vm::type::userdata) {
                hL->pop(1);
                return vm::type::none;
            }
            break;
        }
        default:
            return vm::type::none;
        }
        if (hL->getmetatable(-1)) {
            hL->replace(-2);
            return hL->type(-1);
        } else {
            hL->pop(1);
            return vm::type::none;
        }
    }

    template <>
    int eval<USERVALUE>(USERVALUE& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t == vm::type::none)
            return vm::type::none;
        if (t != vm::type::userdata) {
            hL->pop(1);
            return vm::type::none;
        }
        t = hL->getiuservalue(-1, v.n);
        hL->replace(-2);
        return t;
    }

    template <>
    int eval<TABLE_ARRAY>(TABLE_ARRAY& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t == vm::type::none)
            return vm::type::none;
        if (t != vm::type::table) {
            hL->pop(1);
            return vm::type::none;
        }
        const void* tv = hL->topointer(-1);
        if (!tv) {
            hL->pop(1);
            return vm::type::none;
        }
        if (!hL->table_get_array(tv, v.index)) {
            hL->pop(1);
            return vm::type::none;
        }
        hL->replace(-2);
        return hL->type(-1);
    }

    template <>
    int eval<TABLE_KEY>(TABLE_KEY& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t == vm::type::none) {
            return vm::type::none;
        }
        if (t != vm::type::table && t != vm::type::userdata) {
            hL->pop(1);
            return vm::type::none;
        }
        hL->rawgeti(vm::idx::registry, v.key_ref);
        hL->gettable(-2);
        hL->replace(-2);
        return hL->type(-1);
    }

    template <>
    int eval<TABLE_HASH_KEY>(TABLE_HASH_KEY& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t == vm::type::none)
            return vm::type::none;
        if (t != vm::type::table) {
            hL->pop(1);
            return vm::type::none;
        }
        const void* tv = hL->topointer(-1);
        if (!tv) {
            hL->pop(1);
            return vm::type::none;
        }
        if (!hL->table_get_hash_k(tv, v.index)) {
            hL->pop(1);
            return vm::type::none;
        }
        hL->replace(-2);
        return hL->type(-1);
    }

    template <>
    int eval<TABLE_HASH_VAL>(TABLE_HASH_VAL& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t == vm::type::none)
            return vm::type::none;
        if (t != vm::type::table) {
            hL->pop(1);
            return vm::type::none;
        }
        const void* tv = hL->topointer(-1);
        if (!tv) {
            hL->pop(1);
            return vm::type::none;
        }
        if (!hL->table_get_hash_v(tv, v.index)) {
            hL->pop(1);
            return vm::type::none;
        }
        hL->replace(-2);
        return hL->type(-1);
    }

    template <>
    int eval<USERDATA_KEY>(USERDATA_KEY& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t == vm::type::none)
            return vm::type::none;
        if (t != vm::type::userdata) {
            hL->pop(1);
            return vm::type::none;
        }
        if (!hL->getmetatable(-1)) {
            hL->pop(1);
            return vm::type::none;
        }
        hL->getfield(-1, "__pairs");
        if (vm::type::function != hL->type(-1)) {
            hL->pop(3);
            return vm::type::none;
        }
        hL->remove(-2);
        hL->insert(-2);
        if (debug_pcall(hL, 1, 3, 0)) {
            hL->pop(1);
            return vm::type::none;
        }
        for (auto i = 0;; ++i) {
            // next, t, k
            hL->pushvalue(-3);
            hL->pushvalue(-3);
            hL->pushvalue(-3);
            hL->remove(-4);
            // next, t, next, t, k
            if (debug_pcall(hL, 2, 2, 0)) {
                hL->pop(3);
                return vm::type::none;
            }
            // next, t, k, v
            if (hL->isnoneornil(-2)) {
                hL->pop(4);
                return vm::type::none;
            }
            if (i == v.index) {
                hL->pop(1);
                hL->remove(-2);
                hL->remove(-2);
                return hL->type(-1);
            }
            hL->pop(1);
        }
    }

    template <>
    int eval<USERDATA_VAL>(USERDATA_VAL& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t == vm::type::none)
            return vm::type::none;
        if (t != vm::type::userdata) {
            hL->pop(1);
            return vm::type::none;
        }
        if (!hL->getmetatable(-1)) {
            hL->pop(1);
            return vm::type::none;
        }
        hL->getfield(-1, "__pairs");
        if (vm::type::function != hL->type(-1)) {
            hL->pop(3);
            return vm::type::none;
        }
        hL->remove(-2);
        hL->insert(-2);
        if (debug_pcall(hL, 1, 3, 0)) {
            hL->pop(1);
            return vm::type::none;
        }
        for (auto i = 0;; ++i) {
            // next, t, k
            hL->pushvalue(-3);
            hL->pushvalue(-3);
            hL->pushvalue(-3);
            hL->remove(-4);
            // next, t, next, t, k
            if (debug_pcall(hL, 2, 2, 0)) {
                hL->pop(3);
                return vm::type::none;
            }
            // next, t, k, v
            if (hL->isnoneornil(-2)) {
                hL->pop(4);
                return vm::type::none;
            }
            if (i == v.index) {
                hL->remove(-2);
                hL->remove(-2);
                hL->remove(-2);
                return hL->type(-1);
            }
            hL->pop(1);
        }
    }

    template <>
    int eval<FENV>(FENV& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t == vm::type::none)
            return vm::type::none;
        if (t != vm::type::function) {
            hL->pop(1);
            return vm::type::none;
        }
        if (!hL->getfenv(-1)) {
            hL->pop(1);
            return vm::type::none;
        }
        hL->replace(-2);
        return hL->type(-1);
    }

    int eval(value* v, vm::State* hL) {
        return visit([hL, v](auto&& arg) { return eval(arg, hL, v + 1); }, *v);
    }

    template <typename T>
    bool assign(T&, vm::State*, value*) {
        return false;
    }

    template <>
    bool assign<TABLE_KEY>(TABLE_KEY& v, vm::State* hL, value* parent) {
        // Stack on entry: [new value]. eval(parent) pushes the container, then
        // the key, and lua_settable consumes value, container and key.
        int t = eval(parent, hL);
        if (t == vm::type::none) {
            return false;
        }
        if (t != vm::type::table && t != vm::type::userdata) {
            hL->pop(1);
            return false;
        }
        hL->rawgeti(vm::idx::registry, v.key_ref);
        hL->insert(-3);
        hL->insert(-2);
        hL->settable(-3);
        hL->pop(1);
        return true;
    }

    template <>
    bool assign<FRAME_LOCAL>(FRAME_LOCAL& v, vm::State* hL, value*) {
        vm::FrameInfo ar;
        if (hL->getstack(v.frame, &ar) == 0) {
            return false;
        }
        if (hL->setlocal(&ar, v.n) != NULL) {
            return true;
        }
        return false;
    }

    template <>
    bool assign<UPVALUE>(UPVALUE& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t != vm::type::function)
            return false;
        hL->insert(-2);
        return hL->setupvalue(-2, v.n) != NULL;
    }

    template <>
    bool assign<METATABLE>(METATABLE& v, vm::State* hL, value* parent) {
        switch (v.type) {
        case vm::type::nil:
            hL->pushnil();
            break;
        case vm::type::boolean:
            hL->pushboolean(0);
            break;
        case vm::type::number:
            hL->pushinteger(0);
            break;
        case vm::type::string:
            hL->pushstring("");
            break;
        case vm::type::lightuserdata:
            hL->pushlightuserdata(NULL);
            break;
        case vm::type::table:
        case vm::type::userdata: {
            int t = eval(parent, hL);
            if (t != vm::type::table && t != vm::type::userdata) {
                return false;
            }
            break;
        }
        default:
            return false;
        }

        hL->insert(-2);
        int metattype = hL->type(-1);
        if (metattype != vm::type::nil && metattype != vm::type::table) {
            return false;
        }
        hL->setmetatable(-2);
        return true;
    }

    template <>
    bool assign<USERVALUE>(USERVALUE& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t != vm::type::userdata)
            return false;
        hL->insert(-2);
        hL->setiuservalue(-2, v.n);
        return true;
    }

    template <>
    bool assign<TABLE_ARRAY>(TABLE_ARRAY& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t != vm::type::table)
            return false;
        hL->insert(-2);
        const void* tv = hL->topointer(-2);
        if (!tv)
            return false;
        return hL->table_set_array(tv, v.index);
    }

    template <>
    bool assign<TABLE_HASH_VAL>(TABLE_HASH_VAL& v, vm::State* hL, value* parent) {
        int t = eval(parent, hL);
        if (t == vm::type::none)
            return false;
        if (t != vm::type::table)
            return false;
        hL->insert(-2);
        const void* tv = hL->topointer(-2);
        if (!tv)
            return false;
        return hL->table_set_hash_v(tv, v.index);
    }

    bool assign(value* v, vm::State* hL) {
        int top = hL->gettop();
        bool ok = visit([hL, v](auto&& arg) { return assign(arg, hL, v + 1); }, *v);
        hL->settop(top - 2);
        return ok;
    }

}
