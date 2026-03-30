#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

LUA_API void luaL_openlibs(lua_State* L);

#ifdef __cplusplus
}
#endif
