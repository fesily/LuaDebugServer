#pragma once

// Every symbol name an external source may describe.
//
// An outside symbol table is accepted only for names some consumer actually
// reads: otherwise a typo (`lua_gtetop`) would bind nothing, the binding would
// fall back to whatever the module exports, and the failure would look like a
// working session with one silently wrong entry. The list is therefore closed.
//
// Three groups, all reached through the same `LDB_EXTERNAL_SYMBOL_NAMES` macro:
//   * the target C API the shared core consumes (`LDB_RAW_OPS`, two arguments
//     per entry because the member name and the symbol name differ),
//   * the launcher's internal watch points, which an archived build may not
//     export and an external table is the only way to name,
//   * version evidence read as data (`lua_ident`).

#include "raw_table.h"

// Internal helpers the launcher hooks, plus the version marker.
#define LDB_INTERNAL_SYMBOL_NAMES(X) \
    X(lua_newstate)                  \
    X(lua_close)                     \
    X(luaL_openlibs)                 \
    X(luaD_precall)                  \
    X(luaD_poscall)                  \
    X(luaV_gettable)                 \
    X(luaV_settable)                 \
    X(luaV_finishget)                \
    X(luaH_finishset)                \
    X(lj_dispatch_ins)               \
    X(lj_dispatch_call)              \
    X(lj_vm_call)                    \
    X(lj_vm_pcall)                   \
    X(lj_vm_cpcall)                  \
    X(lj_vm_resume)                  \
    X(lj_meta_tget)                  \
    X(lj_dispatch_update)            \
    X(lj_dispatch_stitch)            \
    X(lua_ident)

// Version evidence that is a function rather than a data marker. They are never
// called, only looked for, but a table must be able to name them: they are the
// only positive evidence that separates 5.4 from 5.5 and 5.3, and a stripped
// image has no other way to report them.
#define LDB_VERSION_EVIDENCE_NAMES(X) \
    X(lua_numbertocstring)            \
    X(luaL_openselectedlibs)          \
    X(lua_resetthread)                \
    X(lua_newuserdatauv)

// `RAW(member, symbol)` receives the two-argument raw operations,
// `ONE(name)` the single-name entries.
#define LDB_EXTERNAL_SYMBOL_NAMES(RAW, ONE) \
    LDB_RAW_OPS(RAW)                        \
    LDB_INTERNAL_SYMBOL_NAMES(ONE)          \
    LDB_VERSION_EVIDENCE_NAMES(ONE)
