#include <cstdio>

extern "C" {
#include "lauxlib.h"
#include "lualib.h"
}

#include "profiler_api.h"

int main() {
    lua_State* L = luaL_newstate();
    if (L == nullptr) {
        std::fprintf(stderr, "luaL_newstate failed.\n");
        return 1;
    }

    luaL_openlibs(L);

    const int start_status = StartProfiler(L);
    if (start_status != PROFILER_OK) {
        char error_text[256] = {};
        GetLastProfilerErrorA(error_text, sizeof(error_text));
        std::fprintf(stderr, "StartProfiler failed (%d): %s\n", start_status, error_text);
        lua_close(L);
        return 1;
    }

    const char* script = R"(
local function leaf(n)
    local acc = 0
    for i = 1, n do
        acc = acc + math.sqrt(i)
    end
    return acc
end

local function branch(depth, width)
    if depth <= 0 then
        return leaf(width * 2500)
    end

    local total = 0
    for i = 1, width do
        total = total + branch(depth - 1, width - 1)
    end
    return total
end

for _ = 1, 6 do
    branch(4, 5)
end
)";

    if (luaL_loadstring(L, script) != LUA_OK) {
        std::fprintf(stderr, "luaL_loadstring failed: %s\n", lua_tostring(L, -1));
        StopProfiler();
        lua_close(L);
        return 1;
    }

    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        std::fprintf(stderr, "lua_pcall failed: %s\n", lua_tostring(L, -1));
        StopProfiler();
        lua_close(L);
        return 1;
    }

    StopProfiler();
    lua_close(L);

    std::puts("Smoke test completed.");
    return 0;
}
