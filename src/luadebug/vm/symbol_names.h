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
//   * the two pseudo names that stand for the launcher's discovery points,
//   * version evidence, read as data (`lua_ident`) or looked for as a function.
//
// The launcher's per-point watch names (`luaD_precall`, `lj_vm_call`, ...) are
// deliberately NOT here: an image that exports nothing is described by the two
// roles below, not by a name per hook.

#include "raw_table.h"

// Two pseudo names for the launcher's discovery points. An image that exports
// nothing has no symbol to point at, so the table names the *role* instead:
//   `lua_debug_server_newstate` - creation entry; the return value is the new
//       `lua_State*`. Hooked on return and kept armed, so a VM created later is
//       still found.
//   `lua_debug_server_call` - per-call entry; the first argument is a `lua_State*`.
//       Hooked on entry and released when the discovery window closes.
// Neither ever exists in an image, so they are never filled from one.
#define LDB_LAUNCH_POINT_NAMES(X) \
    X(lua_debug_server_newstate)  \
    X(lua_debug_server_call)

// Version evidence. `lua_ident` is read as data; the rest are only looked for,
// but a table must be able to name them: they are the only positive evidence
// that separates 5.4 from 5.5 and 5.3, and a stripped image has no other way to
// report them.
#define LDB_VERSION_EVIDENCE_NAMES(X) \
    X(lua_ident)                      \
    X(lua_numbertocstring)            \
    X(luaL_openselectedlibs)          \
    X(lua_resetthread)                \
    X(lua_newuserdatauv)

// `RAW(member, symbol)` receives the two-argument raw operations,
// `ONE(name)` the single-name entries.
#define LDB_EXTERNAL_SYMBOL_NAMES(RAW, ONE) \
    LDB_RAW_OPS(RAW)                        \
    LDB_LAUNCH_POINT_NAMES(ONE)             \
    LDB_VERSION_EVIDENCE_NAMES(ONE)
