#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

extern "C" {
#include "lua.h"
}

#include "profiler_api.h"

// Replace this with your legitimate host/plugin API lookup.
static lua_State* GetLuaStateFromSupportedHostApi() {
    return nullptr;
}

class ProfilerBridge {
public:
    bool initialize(const char* dll_path) {
        module_ = LoadLibraryA(dll_path);
        if (!module_) {
            std::fprintf(stderr, "LoadLibraryA failed for %s\n", dll_path);
            return false;
        }

        start_ = reinterpret_cast<StartFn>(GetProcAddress(module_, "StartProfiler"));
        stop_ = reinterpret_cast<StopFn>(GetProcAddress(module_, "StopProfiler"));
        get_error_ = reinterpret_cast<GetLastErrorFn>(GetProcAddress(module_, "GetLastProfilerErrorA"));
        is_running_ = reinterpret_cast<IsRunningFn>(GetProcAddress(module_, "IsProfilerRunning"));

        if (!start_ || !stop_ || !get_error_ || !is_running_) {
            std::fprintf(stderr, "Profiler DLL exports are missing.\n");
            shutdown();
            return false;
        }

        return true;
    }

    bool start() {
        if (!start_) {
            return false;
        }

        lua_State* L = GetLuaStateFromSupportedHostApi();
        if (!L) {
            std::fprintf(stderr, "No lua_State* available from supported host API.\n");
            return false;
        }

        const int status = start_(L);
        if (status != PROFILER_OK) {
            char error_text[256] = {};
            get_error_(error_text, sizeof(error_text));
            std::fprintf(stderr, "StartProfiler failed (%d): %s\n", status, error_text);
            return false;
        }

        return true;
    }

    void stop() {
        if (stop_) {
            stop_();
        }
    }

    bool is_running() const {
        return is_running_ && is_running_() != 0;
    }

    void shutdown() {
        if (module_) {
            FreeLibrary(module_);
            module_ = nullptr;
        }

        start_ = nullptr;
        stop_ = nullptr;
        get_error_ = nullptr;
        is_running_ = nullptr;
    }

    ~ProfilerBridge() {
        shutdown();
    }

private:
    using StartFn = int(__stdcall*)(lua_State*);
    using StopFn = void(__stdcall*)();
    using GetLastErrorFn = void(__stdcall*)(char*, size_t);
    using IsRunningFn = int(__stdcall*)();

    HMODULE module_ = nullptr;
    StartFn start_ = nullptr;
    StopFn stop_ = nullptr;
    GetLastErrorFn get_error_ = nullptr;
    IsRunningFn is_running_ = nullptr;
};
