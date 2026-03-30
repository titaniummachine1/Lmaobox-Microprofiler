# Lbox Lua MicroProfiler MVP

This repository contains the two safe, reusable halves of the profiler pipeline:

- `lbox_analyzer`: a Rust named-pipe server that aggregates sampled Lua call data into `lbox_flamegraph_data.txt`.
- `profiler.dll`: a Windows DLL that installs a `lua_sethook` callback on a Lua 5.4 state, batches events through a lock-free SPSC queue, and streams them to the analyzer.

The target-specific state discovery step is intentionally not part of this repo. The DLL expects a trusted caller to supply a valid `lua_State*`.

## Repo Layout

- `Cargo.toml`: Rust analyzer package definition.
- `src/main.rs`: named-pipe server, event parser, and flamegraph serializer.
- `dllmain.cpp`: profiler DLL core.
- `profiler_api.h`: exported functions for host integration.
- `build.bat`: DLL build helper for the Visual Studio x64 tools prompt.
- `tools/prepare_local_lua_sdk.ps1`: creates a local `lua54.lib` import library from an existing `lua54.dll`.
- `smoke_test.cpp`: local test host that creates a Lua state, starts profiling, runs a workload, and stops profiling.
- `tools/quick_test.ps1`: one-command local smoke test that builds everything and renders a flame graph.
- `script_profile_host.cpp`: host executable that profiles a chosen `.lua` file.
- `tools/profile_lua_script.ps1`: profiles a selected script from `%LOCALAPPDATA%\lua` (or a custom root).
- `integration/host_profiler_bridge_template.cpp`: drop-in host bridge template showing exactly how to call the profiler API once `lua_State*` is available.

## Prerequisites

1. Install Rust with `rustup`.
2. Install the Visual Studio C++ build tools and use the `x64 Native Tools Command Prompt for VS`.
3. Install Inferno:

```powershell
cargo install inferno
```

If you already have a `lua54.dll` somewhere on disk, the repo can generate a local import library from it. The default quick-test path uses:

`C:\gitProjects\lmaobox-context-protocol\automations\bin\lua\lua54.dll`

## Build

One-command build for the full MVP (analyzer + DLL + host + runner UI):

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_all.ps1
```

### Analyzer

```powershell
cargo build --release
```

This produces `target\release\lbox_analyzer.exe`.

### DLL

1. If you are not using the local shim SDK, edit `build.bat` and update `LUA_INC` and `LUA_LIB`.
2. Run `build.bat` from the Visual Studio x64 tools prompt.

This produces `profiler.dll`.

## Runner UI (Start/Stop)

After `tools\build_all.ps1`, launch:

```powershell
.\profiler_runner.exe
```

What it does:

1. Lists detected `tf_win64.exe` processes.
2. Lists scripts from `%LOCALAPPDATA%\lua` for the current user.
3. Starts profiling for the selected script when you click `Start`.
4. Stops cleanly and flushes outputs when you click `Stop`.

Current MVP behavior: `profiler_runner.exe` does not inject into external processes. It runs scripts through the local Lua host (`script_profile_host.exe`) and uses the TF2 list as visibility for your running game instances.

Outputs are written to the repo root:

- `lbox_flamegraph_data.txt`
- `final_profile.svg`

## Quick Test

This repo now includes a full local smoke test that does not require an injector or game process:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\quick_test.ps1
```

That script will:

1. Generate a local `lua54.lib` from your existing `lua54.dll`.
2. Build `lbox_analyzer.exe`.
3. Build `profiler.dll`.
4. Build `smoke_test.exe`.
5. Run the analyzer and smoke test together.
6. Generate `lbox_flamegraph_data.txt`.
7. Render `final_profile.svg`.

## Profile A Real Script

List scripts in the current user's Lua folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\profile_lua_script.ps1 -List
```

Profile one script by name (username-agnostic path via `%LOCALAPPDATA%\lua`):

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\profile_lua_script.ps1 -Script "my_script.lua" -Repeat 3
```

If the script defines a global entry function, load once and profile only that function:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\profile_lua_script.ps1 -Script "my_script.lua" -EntryFunction "Main" -Repeat 20
```

`-EntryFunction` expects a callable global expression (for example `Main` or `App.Run`). If the script keeps functions local/module-scoped, run without `-EntryFunction`.

You can also pass a full path:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\profile_lua_script.ps1 -Script "C:\Users\<user>\AppData\Local\lua\my_script.lua"
```

The command writes:

- `lbox_flamegraph_data.txt` and `final_profile.svg` (latest run)
- timestamped copies: `profile_<script>_<timestamp>.folded.txt` and `profile_<script>_<timestamp>.svg`

## Execution Roadmap

1. Launch `target\release\lbox_analyzer.exe`.
2. Load `profiler.dll` into the target process you control.
3. When the target Lua VM is initialized, call `StartProfiler(lua_State* L)` from a safe context on the owning thread.
4. Run your workload.
5. Call `StopProfiler()` before unloading the DLL or closing the target process.
6. The analyzer writes `lbox_flamegraph_data.txt`.
7. Render the SVG:

```powershell
inferno-flamegraph < lbox_flamegraph_data.txt > final_profile.svg
```

The analyzer now also attempts to auto-render `final_profile.svg` if `inferno-flamegraph` is available.

## Integration Contract

The exported API is intentionally small:

```cpp
int __stdcall StartProfiler(lua_State* L);
void __stdcall StopProfiler();
void __stdcall GetLastProfilerErrorA(char* buffer, size_t capacity);
int __stdcall IsProfilerRunning();
```

Recommended host-side flow:

```cpp
char error_text[256] = {};
const int status = StartProfiler(L);
if (status != PROFILER_OK) {
    GetLastProfilerErrorA(error_text, sizeof(error_text));
    // Surface error_text in your UI or log.
}
```

## Notes

- The Rust analyzer is single-session and exits after one stream completes.
- The profiler writes no allocations on the hot Lua hook path.
- If you see connection errors, start the analyzer first.
- Call `StopProfiler()` before the DLL is unloaded. Cleanup is intentionally not done from `DllMain`.
- If you need automatic `lua_State*` discovery, add that behind your own approved integration boundary and keep the exported profiler API unchanged.
- Call labels now include source context (`function@source:line`) for more readable flame graphs.

## Last Mile Checklist

The only required integration point is:

1. Obtain a valid `lua_State*` from your supported host/plugin API.
2. Call `StartProfiler(L)` when your Start button is pressed.
3. Call `StopProfiler()` when your Stop button is pressed.

You can use `integration/host_profiler_bridge_template.cpp` as the implementation skeleton.
