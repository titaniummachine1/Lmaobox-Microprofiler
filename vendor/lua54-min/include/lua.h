#pragma once

#include <stddef.h>
#include "luaconf.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define LUA_API __declspec(dllimport)
#else
#define LUA_API extern
#endif

#define LUA_OK 0
#define LUA_MULTRET (-1)

#define LUA_HOOKCALL 0
#define LUA_HOOKRET 1
#define LUA_HOOKLINE 2
#define LUA_HOOKCOUNT 3
#define LUA_HOOKTAILCALL 4

#define LUA_MASKCALL (1 << LUA_HOOKCALL)
#define LUA_MASKRET (1 << LUA_HOOKRET)
#define LUA_MASKLINE (1 << LUA_HOOKLINE)
#define LUA_MASKCOUNT (1 << LUA_HOOKCOUNT)

typedef struct lua_State lua_State;
typedef struct CallInfo CallInfo;

typedef int (*lua_CFunction)(lua_State* L);
typedef int (*lua_KFunction)(lua_State* L, int status, lua_KContext ctx);

typedef struct lua_Debug {
    int event;
    const char* name;
    const char* namewhat;
    const char* what;
    const char* source;
    size_t srclen;
    int currentline;
    int linedefined;
    int lastlinedefined;
    unsigned char nups;
    unsigned char nparams;
    char isvararg;
    char istailcall;
    unsigned short ftransfer;
    unsigned short ntransfer;
    char short_src[LUA_IDSIZE];
    struct CallInfo* i_ci;
} lua_Debug;

typedef void (*lua_Hook)(lua_State* L, lua_Debug* ar);

LUA_API void lua_close(lua_State* L);
LUA_API int lua_getinfo(lua_State* L, const char* what, lua_Debug* ar);
LUA_API int lua_pcallk(lua_State* L, int nargs, int nresults, int errfunc, lua_KContext ctx, lua_KFunction k);
LUA_API void lua_sethook(lua_State* L, lua_Hook func, int mask, int count);
LUA_API const char* lua_tolstring(lua_State* L, int idx, size_t* len);

#define lua_pcall(L, nargs, nresults, errfunc) \
    lua_pcallk((L), (nargs), (nresults), (errfunc), 0, NULL)

#define lua_tostring(L, idx) lua_tolstring((L), (idx), NULL)

#ifdef __cplusplus
}
#endif
