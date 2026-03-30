param(
    [string]$LuaDllPath = "C:\gitProjects\lmaobox-context-protocol\automations\bin\lua\lua54.dll"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$cargo = Join-Path $env:USERPROFILE ".cargo\bin\cargo.exe"
$inferno = Join-Path $env:USERPROFILE ".cargo\bin\inferno-flamegraph.exe"
$analyzerExe = Join-Path $repoRoot "target\release\lbox_analyzer.exe"
$smokeTestExe = Join-Path $repoRoot "smoke_test.exe"
$profilerDll = Join-Path $repoRoot "profiler.dll"
$localLuaDll = Join-Path $repoRoot "lua54.dll"
$vendorLuaDll = Join-Path $repoRoot "vendor\lua54-min\bin\lua54.dll"
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

if (-not (Test-Path $cargo)) {
    throw "cargo.exe was not found at '$cargo'."
}

if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat was not found at '$vcvars'."
}

& (Join-Path $PSScriptRoot "prepare_local_lua_sdk.ps1") -LuaDllPath $LuaDllPath

& $cargo build --release
if ($LASTEXITCODE -ne 0) {
    throw "cargo build --release failed."
}

$buildCmd = "call `"$vcvars`" >nul && `"$repoRoot\build.bat`" && cl.exe /nologo /MT /O2 /W4 /EHsc /std:c++17 /I`"$repoRoot\vendor\lua54-min\include`" `"$repoRoot\smoke_test.cpp`" /Fe:`"$repoRoot\smoke_test.exe`" /link /LIBPATH:`"$repoRoot\vendor\lua54-min\lib`" /LIBPATH:`"$repoRoot`" lua54.lib profiler.lib"
& cmd.exe /c $buildCmd
if ($LASTEXITCODE -ne 0) {
    throw "C++ smoke test build failed."
}

Copy-Item -LiteralPath $vendorLuaDll -Destination $localLuaDll -Force

Remove-Item -LiteralPath (Join-Path $repoRoot "lbox_flamegraph_data.txt") -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $repoRoot "final_profile.svg") -ErrorAction SilentlyContinue

$analyzer = Start-Process -FilePath $analyzerExe -WorkingDirectory $repoRoot -PassThru
try {
    Start-Sleep -Milliseconds 500

    & $smokeTestExe
    if ($LASTEXITCODE -ne 0) {
        throw "smoke_test.exe failed."
    }

    $analyzer.WaitForExit(15000) | Out-Null
    if (-not $analyzer.HasExited) {
        throw "Analyzer did not exit after the smoke test."
    }
}
finally {
    if ($analyzer -and -not $analyzer.HasExited) {
        Stop-Process -Id $analyzer.Id -Force
    }
}

$flamegraphInput = Join-Path $repoRoot "lbox_flamegraph_data.txt"
if (-not (Test-Path $flamegraphInput)) {
    throw "Smoke test finished, but lbox_flamegraph_data.txt was not generated."
}

if (Test-Path $inferno) {
    $svgPath = Join-Path $repoRoot "final_profile.svg"
    & $inferno $flamegraphInput | Set-Content -LiteralPath $svgPath
    if ($LASTEXITCODE -ne 0) {
        throw "inferno-flamegraph failed."
    }
    Write-Host "Generated flame graph: $svgPath"
}

Write-Host "Generated folded stacks: $flamegraphInput"
