#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

constexpr int kIdScripts = 1001;
constexpr int kIdProcesses = 1002;
constexpr int kIdStart = 1003;
constexpr int kIdStop = 1004;
constexpr int kIdRefresh = 1005;
constexpr int kIdOpenOutput = 1006;
constexpr int kIdStatus = 1007;
constexpr UINT_PTR kTimerId = 1;

struct Tf2ProcessInfo {
    DWORD pid;
    std::wstring display;
};

struct AppState {
    HWND hwnd = nullptr;
    HWND scripts_list = nullptr;
    HWND process_combo = nullptr;
    HWND start_button = nullptr;
    HWND stop_button = nullptr;
    HWND status_edit = nullptr;

    std::wstring exe_dir;
    std::wstring repo_root;
    std::wstring lua_root;
    std::wstring stop_file;

    std::vector<std::wstring> script_paths;
    std::vector<Tf2ProcessInfo> tf2_processes;

    PROCESS_INFORMATION analyzer_pi{};
    PROCESS_INFORMATION host_pi{};
    bool running = false;
};

AppState g_state;

HMENU MenuId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

std::wstring GetModuleDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        return L".";
    }

    fs::path p(buffer);
    return p.parent_path().wstring();
}

std::wstring GetLocalAppDataLuaRoot() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L".\\lua";
    }

    fs::path p(buffer);
    p /= L"lua";
    return p.wstring();
}

std::wstring GetTempStopFilePath() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD len = GetTempPathW(MAX_PATH, buffer);
    if (len == 0 || len >= MAX_PATH) {
        return L".\\lbox_profiler_stop.flag";
    }

    fs::path p(buffer);
    p /= L"lbox_profiler_stop.flag";
    return p.wstring();
}

std::wstring QuoteArg(const std::wstring& input) {
    if (input.find_first_of(L" \t\"") == std::wstring::npos) {
        return input;
    }

    std::wstring out = L"\"";
    for (wchar_t c : input) {
        if (c == L'"') {
            out += L"\\\"";
        } else {
            out.push_back(c);
        }
    }
    out += L"\"";
    return out;
}

void AppendStatus(const std::wstring& line) {
    if (!g_state.status_edit) {
        return;
    }

    int len = GetWindowTextLengthW(g_state.status_edit);
    SendMessageW(g_state.status_edit, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));

    std::wstring text = line;
    text += L"\r\n";
    SendMessageW(g_state.status_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void CloseProcessInfo(PROCESS_INFORMATION& pi) {
    if (pi.hThread) {
        CloseHandle(pi.hThread);
        pi.hThread = nullptr;
    }
    if (pi.hProcess) {
        CloseHandle(pi.hProcess);
        pi.hProcess = nullptr;
    }
    pi.dwProcessId = 0;
    pi.dwThreadId = 0;
}

bool FileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool CreateStopFile(const std::wstring& path) {
    HANDLE h = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    const char* text = "stop";
    DWORD written = 0;
    WriteFile(h, text, 4, &written, nullptr);
    CloseHandle(h);
    return true;
}

bool LaunchProcess(
    const std::wstring& exe_path,
    const std::wstring& args,
    const std::wstring& working_dir,
    PROCESS_INFORMATION& out_pi) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);

    std::wstring cmdline = QuoteArg(exe_path);
    if (!args.empty()) {
        cmdline.push_back(L' ');
        cmdline += args;
    }

    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    ZeroMemory(&out_pi, sizeof(out_pi));
    const BOOL ok = CreateProcessW(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        working_dir.empty() ? nullptr : working_dir.c_str(),
        &si,
        &out_pi);

    return ok == TRUE;
}

std::vector<std::wstring> EnumerateScripts(const std::wstring& lua_root) {
    std::vector<std::wstring> scripts;
    std::error_code ec;

    if (!fs::exists(lua_root, ec)) {
        return scripts;
    }

    for (fs::recursive_directory_iterator it(lua_root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            continue;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }

        std::wstring ext = it->path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
        if (ext == L".lua") {
            scripts.push_back(it->path().wstring());
        }
    }

    std::sort(scripts.begin(), scripts.end());
    return scripts;
}

std::vector<Tf2ProcessInfo> EnumerateTf2Processes() {
    std::vector<Tf2ProcessInfo> out;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return out;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"tf_win64.exe") == 0) {
                Tf2ProcessInfo info{};
                info.pid = pe.th32ProcessID;
                info.display = L"tf_win64.exe (PID ";
                info.display += std::to_wstring(pe.th32ProcessID);
                info.display += L")";
                out.push_back(std::move(info));
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return out;
}

void RefreshLists() {
    g_state.script_paths = EnumerateScripts(g_state.lua_root);
    g_state.tf2_processes = EnumerateTf2Processes();

    SendMessageW(g_state.scripts_list, LB_RESETCONTENT, 0, 0);
    for (const auto& script : g_state.script_paths) {
        fs::path full(script);
        std::wstring shown = script;
        std::error_code ec;
        fs::path relative = full.lexically_relative(g_state.lua_root);
        if (!relative.empty() && relative.native() != full.native()) {
            shown = relative.wstring();
        }

        SendMessageW(g_state.scripts_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(shown.c_str()));
    }

    SendMessageW(g_state.process_combo, CB_RESETCONTENT, 0, 0);
    if (g_state.tf2_processes.empty()) {
        const wchar_t* none = L"(No tf_win64.exe instances detected)";
        SendMessageW(g_state.process_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(none));
        SendMessageW(g_state.process_combo, CB_SETCURSEL, 0, 0);
    } else {
        for (const auto& proc : g_state.tf2_processes) {
            SendMessageW(g_state.process_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(proc.display.c_str()));
        }
        SendMessageW(g_state.process_combo, CB_SETCURSEL, 0, 0);
    }

    if (!g_state.script_paths.empty()) {
        SendMessageW(g_state.scripts_list, LB_SETCURSEL, 0, 0);
    }

    std::wstring status = L"Discovered ";
    status += std::to_wstring(g_state.script_paths.size());
    status += L" Lua scripts under ";
    status += g_state.lua_root;
    status += L".";
    AppendStatus(status);
}

void SetRunningUiState(bool running) {
    EnableWindow(g_state.start_button, running ? FALSE : TRUE);
    EnableWindow(g_state.stop_button, running ? TRUE : FALSE);
}

void StopProfiling(bool request_stop_signal) {
    if (!g_state.running) {
        return;
    }

    if (request_stop_signal && !g_state.stop_file.empty()) {
        if (!CreateStopFile(g_state.stop_file)) {
            AppendStatus(L"Warning: failed to create stop file signal.");
        }
    }

    if (g_state.host_pi.hProcess) {
        DWORD wait_host = WaitForSingleObject(g_state.host_pi.hProcess, 10000);
        if (wait_host == WAIT_TIMEOUT) {
            AppendStatus(L"Host did not stop in time. Terminating host.");
            TerminateProcess(g_state.host_pi.hProcess, 1);
            WaitForSingleObject(g_state.host_pi.hProcess, 3000);
        }
    }

    if (g_state.analyzer_pi.hProcess) {
        DWORD wait_analyzer = WaitForSingleObject(g_state.analyzer_pi.hProcess, 10000);
        if (wait_analyzer == WAIT_TIMEOUT) {
            AppendStatus(L"Analyzer did not stop in time. Terminating analyzer.");
            TerminateProcess(g_state.analyzer_pi.hProcess, 1);
            WaitForSingleObject(g_state.analyzer_pi.hProcess, 3000);
        }
    }

    CloseProcessInfo(g_state.host_pi);
    CloseProcessInfo(g_state.analyzer_pi);
    if (!g_state.stop_file.empty()) {
        DeleteFileW(g_state.stop_file.c_str());
    }

    g_state.running = false;
    KillTimer(g_state.hwnd, kTimerId);
    SetRunningUiState(false);

    AppendStatus(L"Profiling stopped.");
    AppendStatus(L"Latest outputs: lbox_flamegraph_data.txt and final_profile.svg (repo root).");
}

void StartProfiling() {
    if (g_state.running) {
        return;
    }

    const LRESULT script_index = SendMessageW(g_state.scripts_list, LB_GETCURSEL, 0, 0);
    if (script_index == LB_ERR || script_index < 0 || static_cast<size_t>(script_index) >= g_state.script_paths.size()) {
        AppendStatus(L"Select a Lua script first.");
        return;
    }

    const std::wstring script_path = g_state.script_paths[static_cast<size_t>(script_index)];

    const fs::path analyzer_path = fs::path(g_state.repo_root) / "target" / "release" / "lbox_analyzer.exe";
    const fs::path host_path = fs::path(g_state.repo_root) / "script_profile_host.exe";

    if (!FileExists(analyzer_path.wstring())) {
        AppendStatus(L"Missing target\\release\\lbox_analyzer.exe. Run tools\\build_all.ps1 first.");
        return;
    }
    if (!FileExists(host_path.wstring())) {
        AppendStatus(L"Missing script_profile_host.exe. Run tools\\build_all.ps1 first.");
        return;
    }

    g_state.stop_file = GetTempStopFilePath();
    DeleteFileW(g_state.stop_file.c_str());

    if (!LaunchProcess(analyzer_path.wstring(), L"", g_state.repo_root, g_state.analyzer_pi)) {
        AppendStatus(L"Failed to start analyzer process.");
        return;
    }

    Sleep(500);

    std::wstring host_args = QuoteArg(script_path);
    host_args += L" 1 - ";
    host_args += QuoteArg(g_state.stop_file);
    if (!LaunchProcess(host_path.wstring(), host_args, g_state.repo_root, g_state.host_pi)) {
        AppendStatus(L"Failed to start script_profile_host.exe.");
        TerminateProcess(g_state.analyzer_pi.hProcess, 1);
        CloseProcessInfo(g_state.analyzer_pi);
        return;
    }

    g_state.running = true;
    SetRunningUiState(true);
    SetTimer(g_state.hwnd, kTimerId, 500, nullptr);

    const LRESULT process_index = SendMessageW(g_state.process_combo, CB_GETCURSEL, 0, 0);
    if (!g_state.tf2_processes.empty() && process_index != CB_ERR && process_index >= 0 &&
        static_cast<size_t>(process_index) < g_state.tf2_processes.size()) {
        std::wstring msg = L"Detected target process selection: ";
        msg += g_state.tf2_processes[static_cast<size_t>(process_index)].display;
        AppendStatus(msg);
    }

    std::wstring msg = L"Started profiling script: ";
    msg += script_path;
    AppendStatus(msg);
    AppendStatus(L"Press Stop to finish and flush results.");
}

void OpenOutputFolder() {
    ShellExecuteW(g_state.hwnd, L"open", g_state.repo_root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE: {
            g_state.hwnd = hwnd;
            HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

            CreateWindowW(L"STATIC", L"TF2 Instances (detected):", WS_CHILD | WS_VISIBLE, 12, 12, 220, 20, hwnd, nullptr, nullptr, nullptr);
            g_state.process_combo = CreateWindowW(
                L"COMBOBOX",
                L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                12,
                34,
                520,
                160,
                hwnd,
                MenuId(kIdProcesses),
                nullptr,
                nullptr);

            CreateWindowW(L"STATIC", L"Lua Scripts (%LOCALAPPDATA%\\lua):", WS_CHILD | WS_VISIBLE, 12, 68, 240, 20, hwnd, nullptr, nullptr, nullptr);
            g_state.scripts_list = CreateWindowW(
                L"LISTBOX",
                L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_BORDER,
                12,
                90,
                520,
                220,
                hwnd,
                MenuId(kIdScripts),
                nullptr,
                nullptr);

            g_state.start_button = CreateWindowW(
                L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 550, 34, 120, 32, hwnd, MenuId(kIdStart), nullptr, nullptr);
            g_state.stop_button = CreateWindowW(
                L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 550, 74, 120, 32, hwnd, MenuId(kIdStop), nullptr, nullptr);
            CreateWindowW(
                L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 550, 114, 120, 32, hwnd, MenuId(kIdRefresh), nullptr, nullptr);
            CreateWindowW(
                L"BUTTON",
                L"Open Output",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                550,
                154,
                120,
                32,
                hwnd,
                MenuId(kIdOpenOutput),
                nullptr,
                nullptr);

            g_state.status_edit = CreateWindowW(
                L"EDIT",
                L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY,
                12,
                322,
                658,
                130,
                hwnd,
                MenuId(kIdStatus),
                nullptr,
                nullptr);

            SendMessageW(g_state.process_combo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(g_state.scripts_list, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(g_state.start_button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(g_state.stop_button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(g_state.status_edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            SetRunningUiState(false);
            RefreshLists();
            AppendStatus(L"Ready. Start profiles selected Lua script in local Lua 5.4 host.");
            return 0;
        }

        case WM_COMMAND: {
            const int control_id = LOWORD(wparam);
            if (control_id == kIdStart && HIWORD(wparam) == BN_CLICKED) {
                StartProfiling();
                return 0;
            }
            if (control_id == kIdStop && HIWORD(wparam) == BN_CLICKED) {
                StopProfiling(true);
                return 0;
            }
            if (control_id == kIdRefresh && HIWORD(wparam) == BN_CLICKED) {
                RefreshLists();
                return 0;
            }
            if (control_id == kIdOpenOutput && HIWORD(wparam) == BN_CLICKED) {
                OpenOutputFolder();
                return 0;
            }
            return 0;
        }

        case WM_TIMER: {
            if (wparam == kTimerId && g_state.running && g_state.host_pi.hProcess) {
                if (WaitForSingleObject(g_state.host_pi.hProcess, 0) == WAIT_OBJECT_0) {
                    AppendStatus(L"Host process exited. Finalizing run.");
                    StopProfiling(false);
                }
            }
            return 0;
        }

        case WM_CLOSE:
            if (g_state.running) {
                StopProfiling(true);
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    g_state.exe_dir = GetModuleDirectory();
    g_state.repo_root = g_state.exe_dir;
    g_state.lua_root = GetLocalAppDataLuaRoot();

    const wchar_t* class_name = L"LboxProfilerRunnerWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = class_name;

    if (!RegisterClassW(&wc)) {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        class_name,
        L"Lbox Lua MicroProfiler Runner",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_SIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        700,
        520,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!hwnd) {
        return 1;
    }

    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
