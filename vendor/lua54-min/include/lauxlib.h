#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

LUA_API int luaL_loadstring(lua_State* L, const char* s);
LUA_API lua_State* luaL_newstate(void);

#ifdef __cplusplus
}
#endif
