#pragma once

// Names of the target C API entry points the bridge resolves per module.
//
// Kept in one place so the resolver and the `Raw` table cannot drift apart: the
// macro takes the member name and the exported symbol name, and the spelling
// difference (`typename_`) stays local to the field.

#define LDB_RAW_OPS(X)                          \
    X(gettop, lua_gettop)                       \
    X(settop, lua_settop)                       \
    X(pushvalue, lua_pushvalue)                 \
    X(absindex, lua_absindex)                   \
    X(rotate, lua_rotate)                       \
    X(insert, lua_insert)                       \
    X(remove, lua_remove)                       \
    X(replace, lua_replace)                     \
    X(xmove, lua_xmove)                         \
    X(checkstack, lua_checkstack)               \
    X(pushnil, lua_pushnil)                     \
    X(pushboolean, lua_pushboolean)             \
    X(pushinteger, lua_pushinteger)             \
    X(pushnumber, lua_pushnumber)               \
    X(pushlstring, lua_pushlstring)             \
    X(pushstring, lua_pushstring)               \
    X(pushlightuserdata, lua_pushlightuserdata) \
    X(pushcclosure, lua_pushcclosure)           \
    X(newthread, lua_newthread)                 \
    X(createtable, lua_createtable)             \
    X(type, lua_type)                           \
    X(typename_, lua_typename)                  \
    X(toboolean, lua_toboolean)                 \
    X(tointegerx, lua_tointegerx)               \
    X(tonumberx, lua_tonumberx)                 \
    X(tointeger, lua_tointeger)                 \
    X(tonumber, lua_tonumber)                   \
    X(isnumber, lua_isnumber)                   \
    X(isstring, lua_isstring)                   \
    X(tolstring, lua_tolstring)                 \
    X(touserdata, lua_touserdata)               \
    X(topointer, lua_topointer)                 \
    X(tocfunction, lua_tocfunction)             \
    X(rawlen, lua_rawlen)                       \
    X(isinteger, lua_isinteger)                 \
    X(rawequal, lua_rawequal)                   \
    X(objlen, lua_objlen)                       \
    X(getglobal, lua_getglobal)                 \
    X(setglobal, lua_setglobal)                 \
    X(getfield, lua_getfield)                   \
    X(setfield, lua_setfield)                   \
    X(gettable, lua_gettable)                   \
    X(settable, lua_settable)                   \
    X(rawget, lua_rawget)                       \
    X(rawset, lua_rawset)                       \
    X(rawgeti, lua_rawgeti)                     \
    X(rawseti, lua_rawseti)                     \
    X(next, lua_next)                           \
    X(getmetatable, lua_getmetatable)           \
    X(setmetatable, lua_setmetatable)           \
    X(getfenv, lua_getfenv)                     \
    X(setfenv, lua_setfenv)                     \
    X(getuservalue, lua_getuservalue)           \
    X(getiuservalue, lua_getiuservalue)         \
    X(setuservalue, lua_setuservalue)           \
    X(setiuservalue, lua_setiuservalue)         \
    X(getupvalue, lua_getupvalue)               \
    X(setupvalue, lua_setupvalue)               \
    X(call, lua_call)                           \
    X(callk, lua_callk)                         \
    X(pcall, lua_pcall)                         \
    X(pcallk, lua_pcallk)                       \
    X(error, lua_error)                         \
    X(loadbufferx, luaL_loadbufferx)            \
    X(loadbuffer, luaL_loadbuffer)              \
    X(sethook, lua_sethook)                     \
    X(gethook, lua_gethook)                     \
    X(gethookmask, lua_gethookmask)             \
    X(gethookcount, lua_gethookcount)           \
    X(getallocf, lua_getallocf)                 \
    X(setallocf, lua_setallocf)                 \
    X(getstack, lua_getstack)                   \
    X(getinfo, lua_getinfo)                     \
    X(getlocal, lua_getlocal)                   \
    X(setlocal, lua_setlocal)                   \
    X(getfuncname, lua_getfuncname)             \
    X(stringtonumber, lua_stringtonumber)       \
    X(setmode, luaJIT_setmode)
