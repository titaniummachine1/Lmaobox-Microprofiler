param(
    [string]$Script,
    [int]$Repeat = 1,
    [string]$EntryFunction = "",
    [string]$LuaRoot = "",
    [string]$LuaDllPath = "C:\gitProjects\lmaobox-context-protocol\automations\bin\lua\lua54.dll",
    [switch]$List
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($LuaRoot)) {
    $LuaRoot = Join-Path $env:LOCALAPPDATA "lua"
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$cargo = Join-Path $env:USERPROFILE ".cargo\bin\cargo.exe"
$analyzerExe = Join-Path $repoRoot "target\release\lbox_analyzer.exe"
$hostExe = Join-Path $repoRoot "script_profile_host.exe"
$vendorLuaDll = Join-Path $repoRoot "vendor\lua54-min\bin\lua54.dll"
$localLuaDll = Join-Path $repoRoot "lua54.dll"
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

if (-not (Test-Path $LuaRoot)) {
    throw "Lua root folder not found: '$LuaRoot'"
}

$luaFiles = Get-ChildItem -Path $LuaRoot -Recurse -Filter *.lua -File -ErrorAction SilentlyContinue

if ($List -or [string]::IsNullOrWhiteSpace($Script)) {
    if ($luaFiles.Count -eq 0) {
        Write-Host "No .lua files found under $LuaRoot"
        exit 0
    }

    Write-Host "Scripts under $LuaRoot`n"
    $luaFiles |
        Sort-Object FullName |
        ForEach-Object {
            $relative = $_.FullName.Substring($LuaRoot.Length).TrimStart('\')
            Write-Host " - $relative"
        }

    if ([string]::IsNullOrWhiteSpace($Script)) {
        Write-Host "`nUse -Script <name_or_path> to run profiling."
        exit 0
    }
}

function Resolve-ScriptPath {
    param(
        [string]$ScriptValue,
        [string]$Root,
        [System.IO.FileInfo[]]$Files
    )

    if (Test-Path $ScriptValue) {
        return (Resolve-Path $ScriptValue).Path
    }

    $candidates = $Files | Where-Object {
        $_.Name -ieq $ScriptValue -or
        $_.BaseName -ieq $ScriptValue -or
        $_.Name -ieq "$ScriptValue.lua"
    }

    if ($candidates.Count -eq 0) {
        $needle = $ScriptValue.ToLowerInvariant()
        $candidates = $Files | Where-Object {
            $_.Name.ToLowerInvariant().Contains($needle) -or $_.FullName.ToLowerInvariant().Contains($needle)
        }
    }

    if ($candidates.Count -eq 1) {
        return $candidates[0].FullName
    }

    if ($candidates.Count -gt 1) {
        Write-Host "Multiple scripts matched '$ScriptValue':"
        $candidates | Select-Object -First 20 | ForEach-Object {
            $relative = $_.FullName.Substring($Root.Length).TrimStart('\')
            Write-Host " - $relative"
        }
        throw "Please provide a more specific script name or full path."
    }

    throw "Could not find script '$ScriptValue' under '$Root'."
}

$scriptPath = Resolve-ScriptPath -ScriptValue $Script -Root $LuaRoot -Files $luaFiles
Write-Host "Selected script: $scriptPath"

$possibleEntryFunctions = @()
if (-not [string]::IsNullOrWhiteSpace($EntryFunction)) {
    $possibleEntryFunctions =
        (Get-Content -LiteralPath $scriptPath |
            Select-String -Pattern '^\s*function\s+([A-Za-z_][A-Za-z0-9_.:]*)\s*\(' -AllMatches |
            ForEach-Object {
                foreach ($m in $_.Matches) {
                    $m.Groups[1].Value
                }
            } |
            Select-Object -Unique)
}

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

$buildCmd = "call `"$vcvars`" >nul && `"$repoRoot\build.bat`" && cl.exe /nologo /MT /O2 /W4 /EHsc /std:c++17 /I`"$repoRoot\vendor\lua54-min\include`" `"$repoRoot\script_profile_host.cpp`" /Fe:`"$repoRoot\script_profile_host.exe`" /link /LIBPATH:`"$repoRoot\vendor\lua54-min\lib`" /LIBPATH:`"$repoRoot`" lua54.lib profiler.lib"
& cmd.exe /c $buildCmd
if ($LASTEXITCODE -ne 0) {
    throw "script_profile_host build failed."
}

Copy-Item -LiteralPath $vendorLuaDll -Destination $localLuaDll -Force
Remove-Item -LiteralPath (Join-Path $repoRoot "lbox_flamegraph_data.txt") -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $repoRoot "final_profile.svg") -ErrorAction SilentlyContinue

$analyzer = Start-Process -FilePath $analyzerExe -WorkingDirectory $repoRoot -PassThru
try {
    Start-Sleep -Milliseconds 500

    if ([string]::IsNullOrWhiteSpace($EntryFunction)) {
        & $hostExe $scriptPath $Repeat
    } else {
        & $hostExe $scriptPath $Repeat $EntryFunction
    }
    if ($LASTEXITCODE -ne 0) {
        if (-not [string]::IsNullOrWhiteSpace($EntryFunction)) {
            Write-Host "Entry function '$EntryFunction' failed."
            if ($possibleEntryFunctions.Count -gt 0) {
                Write-Host "Possible declared function names in script (may be local/module-scoped):"
                $possibleEntryFunctions | Select-Object -First 20 | ForEach-Object { Write-Host " - $_" }
            } else {
                Write-Host "No top-level 'function Name(...)' definitions were found in this script."
            }
        }
        throw "script_profile_host.exe failed."
    }

    $analyzer.WaitForExit(300000) | Out-Null
    if (-not $analyzer.HasExited) {
        throw "Analyzer did not exit after script profiling."
    }
}
finally {
    if ($analyzer) {
        $running = Get-Process -Id $analyzer.Id -ErrorAction SilentlyContinue
        if ($running) {
            Stop-Process -Id $analyzer.Id -Force -ErrorAction SilentlyContinue
        }
    }
}

$foldedPath = Join-Path $repoRoot "lbox_flamegraph_data.txt"
if (-not (Test-Path $foldedPath)) {
    throw "Profiling finished, but lbox_flamegraph_data.txt was not generated."
}

$safeBase = [Regex]::Replace([IO.Path]::GetFileNameWithoutExtension($scriptPath), "[^A-Za-z0-9._-]", "_")
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$namedFolded = Join-Path $repoRoot ("profile_{0}_{1}.folded.txt" -f $safeBase, $stamp)
Copy-Item -LiteralPath $foldedPath -Destination $namedFolded -Force

Write-Host "Generated folded stacks: $namedFolded"

$svgPath = Join-Path $repoRoot "final_profile.svg"
if (Test-Path $svgPath) {
    $namedSvg = Join-Path $repoRoot ("profile_{0}_{1}.svg" -f $safeBase, $stamp)
    Copy-Item -LiteralPath $svgPath -Destination $namedSvg -Force
    Write-Host "Generated flame graph:  $namedSvg"
}
