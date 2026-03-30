#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <windows.h>

extern "C" {
#include "lauxlib.h"
#include "lualib.h"
}

#include "profiler_api.h"

static bool ReadFileToString(const char* path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

static bool FileExists(const char* path) {
    if (!path || !path[0]) {
        return false;
    }
    const DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool RunScript(lua_State* L, const std::string& script_source, int run_index) {
    if (luaL_loadstring(L, script_source.c_str()) != LUA_OK) {
        std::fprintf(stderr, "luaL_loadstring failed (run %d): %s\n", run_index, lua_tostring(L, -1));
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        std::fprintf(stderr, "lua_pcall failed (run %d): %s\n", run_index, lua_tostring(L, -1));
        return false;
    }
    return true;
}

static bool CallEntry(lua_State* L, const std::string& call_source, int run_index) {
    if (luaL_loadstring(L, call_source.c_str()) != LUA_OK) {
        std::fprintf(stderr, "luaL_loadstring failed for entry function (run %d): %s\n", run_index, lua_tostring(L, -1));
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        std::fprintf(stderr, "lua_pcall failed for entry function (run %d): %s\n", run_index, lua_tostring(L, -1));
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(
            stderr,
            "Usage: script_profile_host.exe <script_path.lua> [repeat_count] [entry_function_name|'-'] [stop_file_path]\n");
        return 2;
    }

    const char* script_path = argv[1];
    int repeat_count = 1;
    if (argc >= 3) {
        repeat_count = std::atoi(argv[2]);
        if (repeat_count < 1) {
            repeat_count = 1;
        }
    }
    const char* raw_entry_function = (argc >= 4) ? argv[3] : nullptr;
    const char* stop_file_path = (argc >= 5) ? argv[4] : nullptr;
    const char* entry_function =
        (raw_entry_function && raw_entry_function[0] && std::strcmp(raw_entry_function, "-") != 0)
            ? raw_entry_function
            : nullptr;

    std::string script_source;
    if (!ReadFileToString(script_path, script_source)) {
        std::fprintf(stderr, "Failed to read script: %s\n", script_path);
        return 1;
    }

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

    int total_runs = 0;

    if (entry_function && entry_function[0]) {
        if (luaL_loadstring(L, script_source.c_str()) != LUA_OK) {
            std::fprintf(stderr, "luaL_loadstring failed while loading script: %s\n", lua_tostring(L, -1));
            StopProfiler();
            lua_close(L);
            return 1;
        }
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            std::fprintf(stderr, "lua_pcall failed while loading script: %s\n", lua_tostring(L, -1));
            StopProfiler();
            lua_close(L);
            return 1;
        }

        std::string call_source = entry_function;
        if (call_source.find('(') == std::string::npos) {
            call_source += "()";
        }
        int run_index = 0;
        if (stop_file_path && stop_file_path[0]) {
            while (!FileExists(stop_file_path)) {
                for (int i = 0; i < repeat_count; ++i) {
                    ++run_index;
                    ++total_runs;
                    if (!CallEntry(L, call_source, run_index)) {
                        StopProfiler();
                        lua_close(L);
                        return 1;
                    }
                }
            }
        } else {
            for (int i = 0; i < repeat_count; ++i) {
                ++run_index;
                ++total_runs;
                if (!CallEntry(L, call_source, run_index)) {
                    StopProfiler();
                    lua_close(L);
                    return 1;
                }
            }
        }
    } else {
        int run_index = 0;
        if (stop_file_path && stop_file_path[0]) {
            while (!FileExists(stop_file_path)) {
                for (int i = 0; i < repeat_count; ++i) {
                    ++run_index;
                    ++total_runs;
                    if (!RunScript(L, script_source, run_index)) {
                        StopProfiler();
                        lua_close(L);
                        return 1;
                    }
                }
            }
        } else {
            for (int i = 0; i < repeat_count; ++i) {
                ++run_index;
                ++total_runs;
                if (!RunScript(L, script_source, run_index)) {
                    StopProfiler();
                    lua_close(L);
                    return 1;
                }
            }
        }
    }

    StopProfiler();
    lua_close(L);

    if (entry_function && entry_function[0]) {
        std::printf(
            "Profiled %s via %s() (%d run%s).\n",
            script_path,
            entry_function,
            total_runs,
            total_runs == 1 ? "" : "s");
    } else {
        std::printf("Profiled %s (%d run%s).\n", script_path, total_runs, total_runs == 1 ? "" : "s");
    }
    return 0;
}
