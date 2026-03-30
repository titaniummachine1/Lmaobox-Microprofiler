param(
    [string]$LuaDllPath = "C:\gitProjects\lmaobox-context-protocol\automations\bin\lua\lua54.dll"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$vendorRoot = Join-Path $repoRoot "vendor\lua54-min"
$includeDir = Join-Path $vendorRoot "include"
$libDir = Join-Path $vendorRoot "lib"
$binDir = Join-Path $vendorRoot "bin"

if (-not (Test-Path $LuaDllPath)) {
    throw "lua54.dll not found at '$LuaDllPath'. Pass -LuaDllPath with a valid runtime DLL path."
}

New-Item -ItemType Directory -Force -Path $includeDir, $libDir, $binDir | Out-Null

$copiedDllPath = Join-Path $binDir "lua54.dll"
Copy-Item -LiteralPath $LuaDllPath -Destination $copiedDllPath -Force

$defPath = Join-Path $libDir "lua54.def"
$defContent = @"
LIBRARY lua54.dll
EXPORTS
    luaL_loadstring
    luaL_newstate
    luaL_openlibs
    lua_close
    lua_getinfo
    lua_pcallk
    lua_sethook
    lua_tolstring
"@
Set-Content -LiteralPath $defPath -Value $defContent -NoNewline

$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at '$vcvars'."
}

$libPath = Join-Path $libDir "lua54.lib"
$cmd = "call `"$vcvars`" >nul && lib /nologo /def:`"$defPath`" /machine:x64 /name:lua54.dll /out:`"$libPath`""
& cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "Failed to generate lua54.lib from '$LuaDllPath'."
}

Write-Host "Prepared local Lua SDK shim:"
Write-Host "  DLL: $copiedDllPath"
Write-Host "  LIB: $libPath"
