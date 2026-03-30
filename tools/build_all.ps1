param(
    [string]$LuaDllPath = "C:\gitProjects\lmaobox-context-protocol\automations\bin\lua\lua54.dll"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$cargo = Join-Path $env:USERPROFILE ".cargo\bin\cargo.exe"
$vendorLuaDll = Join-Path $repoRoot "vendor\lua54-min\bin\lua54.dll"
$localLuaDll = Join-Path $repoRoot "lua54.dll"
$vcvarsCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\17\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
)

if (-not (Test-Path $cargo)) {
    throw "cargo.exe was not found at '$cargo'."
}

$vcvars = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($vcvars)) {
    throw "Could not locate vcvars64.bat. Install Visual Studio C++ build tools."
}

& (Join-Path $PSScriptRoot "prepare_local_lua_sdk.ps1") -LuaDllPath $LuaDllPath

& $cargo build --release
if ($LASTEXITCODE -ne 0) {
    throw "cargo build --release failed."
}

$cmd =
    "call `"$vcvars`" >nul && " +
    "call `"$repoRoot\build.bat`" && " +
    "cl.exe /nologo /MT /O2 /W4 /EHsc /std:c++17 /I`"$repoRoot\vendor\lua54-min\include`" `"$repoRoot\script_profile_host.cpp`" /Fe:`"$repoRoot\script_profile_host.exe`" /link /LIBPATH:`"$repoRoot\vendor\lua54-min\lib`" /LIBPATH:`"$repoRoot`" lua54.lib profiler.lib && " +
    "cl.exe /nologo /MT /O2 /W4 /EHsc /std:c++17 `"$repoRoot\profiler_runner.cpp`" /Fe:`"$repoRoot\profiler_runner.exe`" /link shell32.lib user32.lib gdi32.lib"

& cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "Native build failed."
}

Copy-Item -LiteralPath $vendorLuaDll -Destination $localLuaDll -Force

Write-Host "Build complete."
Write-Host " - Analyzer: target\release\lbox_analyzer.exe"
Write-Host " - DLL: profiler.dll"
Write-Host " - Host: script_profile_host.exe"
Write-Host " - Runner UI: profiler_runner.exe"
