#pragma once

#include <stddef.h>

struct lua_State;

#ifdef __cplusplus
#define PROFILER_EXTERN extern "C"
#else
#define PROFILER_EXTERN extern
#endif

#ifdef PROFILER_BUILD
#define PROFILER_API PROFILER_EXTERN __declspec(dllexport)
#else
#define PROFILER_API PROFILER_EXTERN __declspec(dllimport)
#endif

enum ProfilerStatus {
    PROFILER_OK = 0,
    PROFILER_INVALID_ARGUMENT = 1,
    PROFILER_ALREADY_RUNNING = 2,
    PROFILER_PIPE_CONNECT_FAILED = 3,
    PROFILER_HANDSHAKE_FAILED = 4,
    PROFILER_THREAD_FAILED = 5,
};

PROFILER_API int __stdcall StartProfiler(lua_State* L);
PROFILER_API void __stdcall StopProfiler();
PROFILER_API void __stdcall GetLastProfilerErrorA(char* buffer, size_t capacity);
PROFILER_API int __stdcall IsProfilerRunning();
