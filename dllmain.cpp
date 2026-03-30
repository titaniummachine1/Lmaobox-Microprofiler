#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define PROFILER_BUILD

#include <windows.h>
#include <intrin.h>
#include <emmintrin.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "profiler_api.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4324)
#endif

#pragma pack(push, 1)
struct SessionHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t tsc_freq;
    char build_tag[32];
};

struct ProfileEvent {
    uint64_t timestamp;
    uint8_t event_type;
    char func_name[63];
};
#pragma pack(pop)

constexpr const char* kPipeName = R"(\\.\pipe\LboxProfiler)";
constexpr uint32_t kSessionMagic = 0x4C423058;
constexpr uint32_t kSessionVersion = 1;
constexpr size_t kRingSize = 16384;
constexpr size_t kRingMask = kRingSize - 1;
constexpr size_t kFlushBatch = 512;

static_assert((kRingSize & (kRingSize - 1)) == 0, "kRingSize must be a power of two");

struct SPSCQueue {
    alignas(64) std::atomic<size_t> write_head{0};
    alignas(64) std::atomic<size_t> read_head{0};
    alignas(64) std::array<ProfileEvent, kRingSize> buffer{};

    bool try_push(const ProfileEvent& event) {
        const size_t write = write_head.load(std::memory_order_relaxed);
        const size_t next_write = (write + 1) & kRingMask;
        if (next_write == read_head.load(std::memory_order_acquire)) {
            return false;
        }

        buffer[write] = event;
        write_head.store(next_write, std::memory_order_release);
        return true;
    }

    bool try_pop(ProfileEvent& out) {
        const size_t read = read_head.load(std::memory_order_relaxed);
        if (read == write_head.load(std::memory_order_acquire)) {
            return false;
        }

        out = buffer[read];
        read_head.store((read + 1) & kRingMask, std::memory_order_release);
        return true;
    }
};

namespace {
SPSCQueue g_queue;
HANDLE g_pipe = INVALID_HANDLE_VALUE;
HANDLE g_flush_thread = nullptr;
std::atomic<bool> g_running{false};
lua_State* g_lua_state = nullptr;
SRWLOCK g_error_lock = SRWLOCK_INIT;
std::array<char, 256> g_last_error{};

void SetLastProfilerError(const char* message) {
    AcquireSRWLockExclusive(&g_error_lock);
    strncpy_s(g_last_error.data(), g_last_error.size(), message ? message : "", _TRUNCATE);
    ReleaseSRWLockExclusive(&g_error_lock);
}

void SetLastProfilerErrorFromSystem(const char* prefix) {
    char system_message[160] = {};
    const DWORD error = GetLastError();
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        system_message,
        static_cast<DWORD>(sizeof(system_message)),
        nullptr);

    char combined[256] = {};
    if (length > 0) {
        _snprintf_s(
            combined,
            sizeof(combined),
            _TRUNCATE,
            "%s (Win32=%lu): %s",
            prefix,
            static_cast<unsigned long>(error),
            system_message);
    } else {
        _snprintf_s(
            combined,
            sizeof(combined),
            _TRUNCATE,
            "%s (Win32=%lu)",
            prefix,
            static_cast<unsigned long>(error));
    }

    SetLastProfilerError(combined);
}

void ClearLastProfilerError() {
    SetLastProfilerError("");
}

bool HasInvariantTSC() {
    int regs[4] = {};
    __cpuid(regs, 0x80000007);
    return ((regs[3] >> 8) & 1) != 0;
}

uint64_t CalibrateTSCFrequency() {
    LARGE_INTEGER qpc_freq = {};
    LARGE_INTEGER start_qpc = {};
    LARGE_INTEGER current_qpc = {};
    QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&start_qpc);

    const LONGLONG wait_ticks = qpc_freq.QuadPart / 20;
    const uint64_t start_tsc = __rdtsc();
    do {
        QueryPerformanceCounter(&current_qpc);
    } while ((current_qpc.QuadPart - start_qpc.QuadPart) < wait_ticks);
    const uint64_t end_tsc = __rdtsc();

    const double elapsed_seconds =
        static_cast<double>(current_qpc.QuadPart - start_qpc.QuadPart) / static_cast<double>(qpc_freq.QuadPart);
    return static_cast<uint64_t>((end_tsc - start_tsc) / elapsed_seconds);
}

bool ConnectPipe() {
    for (int attempt = 0; attempt < 20; ++attempt) {
        g_pipe = CreateFileA(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (g_pipe != INVALID_HANDLE_VALUE) {
            return true;
        }

        if (!WaitNamedPipeA(kPipeName, 250)) {
            Sleep(250);
        }
    }

    SetLastProfilerError("Could not connect to \\\\.\\pipe\\LboxProfiler. Start lbox_analyzer.exe first.");
    return false;
}

bool SendHandshake() {
    SessionHeader header{};
    header.magic = kSessionMagic;
    header.version = kSessionVersion;
    header.tsc_freq = CalibrateTSCFrequency();
    strncpy_s(header.build_tag, sizeof(header.build_tag), "lua54-profiler-mvp", _TRUNCATE);

    DWORD written = 0;
    if (!WriteFile(g_pipe, &header, sizeof(header), &written, nullptr) || written != sizeof(header)) {
        SetLastProfilerErrorFromSystem("Failed to send profiler session header");
        return false;
    }

    return true;
}

void ClosePipe() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
}

DWORD WINAPI FlushThreadProc(LPVOID) {
    ProfileEvent batch[kFlushBatch]{};
    int spin_count = 0;

    while (g_running.load(std::memory_order_relaxed) ||
           g_queue.read_head.load(std::memory_order_acquire) != g_queue.write_head.load(std::memory_order_acquire)) {
        size_t count = 0;
        while (count < kFlushBatch && g_queue.try_pop(batch[count])) {
            ++count;
        }

        if (count == 0) {
            if (++spin_count < 64) {
                _mm_pause();
            } else {
                SwitchToThread();
                spin_count = 0;
            }
            continue;
        }

        spin_count = 0;

        DWORD written = 0;
        const DWORD bytes_to_write = static_cast<DWORD>(count * sizeof(ProfileEvent));
        if (!WriteFile(g_pipe, batch, bytes_to_write, &written, nullptr) || written != bytes_to_write) {
            SetLastProfilerErrorFromSystem("Failed to flush profiler events to analyzer");
            g_running.store(false, std::memory_order_relaxed);
            break;
        }
    }

    return 0;
}
}  // namespace

static void lua_profiler_hook(lua_State* L, lua_Debug* ar) {
    if (!g_running.load(std::memory_order_relaxed)) {
        return;
    }

    if (ar->event != LUA_HOOKCALL && ar->event != LUA_HOOKTAILCALL && ar->event != LUA_HOOKRET) {
        return;
    }

    ProfileEvent event{};
    event.timestamp = __rdtsc();

    if (ar->event == LUA_HOOKRET) {
        event.event_type = 1;
        event.func_name[0] = '\0';
        g_queue.try_push(event);
        return;
    }

    event.event_type = 0;
    lua_getinfo(L, "nSl", ar);

    const char* src = ar->short_src[0] ? ar->short_src : "?";
    int line = (ar->linedefined > 0) ? ar->linedefined : ar->currentline;
    if (line < 0) {
        line = 0;
    }

    if (ar->name && ar->name[0]) {
        _snprintf_s(event.func_name, sizeof(event.func_name), _TRUNCATE, "%s@%s:%d", ar->name, src, line);
    } else {
        _snprintf_s(event.func_name, sizeof(event.func_name), _TRUNCATE, "%s:%d", src, line);
    }

    g_queue.try_push(event);
}

int __stdcall StartProfiler(lua_State* L) {
    if (L == nullptr) {
        SetLastProfilerError("StartProfiler requires a valid lua_State*.");
        return PROFILER_INVALID_ARGUMENT;
    }

    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        SetLastProfilerError("Profiler is already running.");
        return PROFILER_ALREADY_RUNNING;
    }

    ClearLastProfilerError();
    g_lua_state = L;

    if (!HasInvariantTSC()) {
        OutputDebugStringA("[LboxProfiler] WARNING: CPU does not advertise Invariant TSC.\n");
    }

    if (!ConnectPipe()) {
        g_running.store(false, std::memory_order_release);
        g_lua_state = nullptr;
        return PROFILER_PIPE_CONNECT_FAILED;
    }

    if (!SendHandshake()) {
        ClosePipe();
        g_running.store(false, std::memory_order_release);
        g_lua_state = nullptr;
        return PROFILER_HANDSHAKE_FAILED;
    }

    lua_sethook(g_lua_state, lua_profiler_hook, LUA_MASKCALL | LUA_MASKRET, 0);

    g_flush_thread = CreateThread(nullptr, 0, FlushThreadProc, nullptr, 0, nullptr);
    if (g_flush_thread == nullptr) {
        SetLastProfilerErrorFromSystem("Failed to start flush thread");
        lua_sethook(g_lua_state, nullptr, 0, 0);
        ClosePipe();
        g_running.store(false, std::memory_order_release);
        g_lua_state = nullptr;
        return PROFILER_THREAD_FAILED;
    }

    return PROFILER_OK;
}

void __stdcall StopProfiler() {
    g_running.store(false, std::memory_order_release);

    if (g_lua_state != nullptr) {
        lua_sethook(g_lua_state, nullptr, 0, 0);
    }

    if (g_flush_thread != nullptr) {
        WaitForSingleObject(g_flush_thread, 3000);
        CloseHandle(g_flush_thread);
        g_flush_thread = nullptr;
    }

    ClosePipe();
    g_lua_state = nullptr;
}

void __stdcall GetLastProfilerErrorA(char* buffer, size_t capacity) {
    if (buffer == nullptr || capacity == 0) {
        return;
    }

    AcquireSRWLockShared(&g_error_lock);
    strncpy_s(buffer, capacity, g_last_error.data(), _TRUNCATE);
    ReleaseSRWLockShared(&g_error_lock);
}

int __stdcall IsProfilerRunning() {
    return g_running.load(std::memory_order_relaxed) ? 1 : 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
