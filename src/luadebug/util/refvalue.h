#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

#include "util/variant.h"
#include "vm/vm.h"

namespace luadebug::refvalue {
    struct FRAME_LOCAL {
        uint16_t frame;
        int16_t n;
    };
    struct FRAME_FUNC {
        uint16_t frame;
    };
    struct GLOBAL {};
    enum class REGISTRY_TYPE {
        REGISTRY,
        DEBUG_REF,
        DEBUG_WATCH,
    };
    struct REGISTRY {
        REGISTRY_TYPE type;
    };
    struct STACK {
        int index;
    };
    struct UPVALUE {
        int n;
    };
    struct METATABLE {
        int type;
    };
    struct USERVALUE {
        int n;
    };
    struct TABLE_ARRAY {
        unsigned int index;
    };
    // Table member addressed by a key kept in the target registry (owned by the
    // caller, e.g. target::ValueStore). Hash-slot paths are unstable across
    // mutations; this one is not.
    struct TABLE_KEY {
        int key_ref;
    };
    struct TABLE_HASH_KEY {
        unsigned int index;
    };
    struct TABLE_HASH_VAL {
        unsigned int index;
    };
    struct USERDATA_KEY {
        unsigned int index;
    };
    struct USERDATA_VAL {
        unsigned int index;
    };
    struct FENV {};
    using value = variant<
        FRAME_LOCAL,
        FRAME_FUNC,
        GLOBAL,
        REGISTRY,
        STACK,
        UPVALUE,
        METATABLE,
        USERVALUE,
        TABLE_ARRAY,
        TABLE_KEY,
        TABLE_HASH_KEY,
        TABLE_HASH_VAL,
        USERDATA_KEY,
        USERDATA_VAL,
        FENV>;
    static_assert(std::is_trivially_copyable_v<value>);

    template <typename T>
    struct allow_as_root : public std::false_type {};
    template <typename T>
    struct allow_as_child : public std::false_type {};

    template <>
    struct allow_as_root<FRAME_LOCAL> : public std::true_type {};
    template <>
    struct allow_as_root<FRAME_FUNC> : public std::true_type {};
    template <>
    struct allow_as_root<GLOBAL> : public std::true_type {};
    template <>
    struct allow_as_root<REGISTRY> : public std::true_type {};
    template <>
    struct allow_as_root<STACK> : public std::true_type {};
    template <>
    struct allow_as_root<METATABLE> : public std::true_type {};
    template <>
    struct allow_as_child<UPVALUE> : public std::true_type {};
    template <>
    struct allow_as_child<METATABLE> : public std::true_type {};
    template <>
    struct allow_as_child<USERVALUE> : public std::true_type {};
    template <>
    struct allow_as_child<TABLE_ARRAY> : public std::true_type {};
    template <>
    struct allow_as_child<TABLE_KEY> : public std::true_type {};
    template <>
    struct allow_as_child<TABLE_HASH_KEY> : public std::true_type {};
    template <>
    struct allow_as_child<TABLE_HASH_VAL> : public std::true_type {};
    template <>
    struct allow_as_child<USERDATA_KEY> : public std::true_type {};
    template <>
    struct allow_as_child<USERDATA_VAL> : public std::true_type {};
    template <>
    struct allow_as_child<FENV> : public std::true_type {};

    int eval(value* v, vm::State* hL);
    bool assign(value* v, vm::State* hL);
}
