@echo off
setlocal

echo Building Lua MicroProfiler...

set "REPO_ROOT=%~dp0"
set "LOCAL_LUA_ROOT=%REPO_ROOT%vendor\lua54-min"

if exist "%LOCAL_LUA_ROOT%\include\lua.h" if exist "%LOCAL_LUA_ROOT%\lib\lua54.lib" (
    set "LUA_INC=%LOCAL_LUA_ROOT%\include"
    set "LUA_LIB=%LOCAL_LUA_ROOT%\lib"
) else (
    rem Update these paths to your local Lua 5.4 SDK if you are not using tools\prepare_local_lua_sdk.ps1.
    set "LUA_INC=C:\path\to\lua54\include"
    set "LUA_LIB=C:\path\to\lua54\lib"
)

if not exist "%LUA_INC%\lua.h" (
    echo [ERROR] Could not find "%LUA_INC%\lua.h"
    exit /b 1
)

if not exist "%LUA_LIB%\lua54.lib" (
    echo [ERROR] Could not find "%LUA_LIB%\lua54.lib"
    exit /b 1
)

cl.exe /nologo /MT /O2 /W4 /EHsc /LD /std:c++17 /I"%LUA_INC%" dllmain.cpp /Fe:profiler.dll /link /LIBPATH:"%LUA_LIB%" lua54.lib

if %ERRORLEVEL% EQU 0 (
    echo Build successful. profiler.dll is ready.
) else (
    echo Build failed.
)

endlocal
