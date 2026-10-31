param(
    [string]$Dll = ""
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Out = Join-Path $Root "build\smoke-host\fsr31"
$ExpectedHash = "12A5081257EC95B0B53AD51B4A87FB3C03F97FE0BBB59F9496968F8D50EF93A6"

if (-not $Dll) {
    $Dll = Join-Path $Root "work\release\fidelityfx\amd_fidelityfx_dx12.dll"
}
if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) {
    throw "FidelityFX runtime not found: $Dll (run tools/release/stage_fidelityfx.py first)"
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $Dll).Hash -ne $ExpectedHash) {
    throw "FidelityFX runtime hash does not match the pinned SDK v1.1.4 DLL"
}

$Compiler = Get-Command g++.exe -ErrorAction SilentlyContinue
if (-not $Compiler) {
    $Ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($Ninja) {
        $Candidate = Join-Path (Split-Path $Ninja.Source) "g++.exe"
        if (Test-Path -LiteralPath $Candidate) {
            $Compiler = Get-Item -LiteralPath $Candidate
        }
    }
}
if (-not $Compiler) {
    throw "g++.exe was not found on PATH or beside ninja.exe"
}

New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item -LiteralPath $Dll -Destination (Join-Path $Out "amd_fidelityfx_dx12.dll") -Force

& $Compiler.Source `
    -std=c++20 -O2 -Wall -Wextra `
    -Wno-cast-function-type -Wno-switch `
    "-I$(Join-Path $Root 'port\kz3')" `
    "-I$(Join-Path $Root 'third_party\fidelityfx-sdk\ffx-api\include')" `
    (Join-Path $Root "port\kz3\fsr31.cpp") `
    (Join-Path $Root "tools\smoke\test_fsr31.cpp") `
    -ld3d12 -ldxgi -ldxguid -ld3dcompiler -lole32 `
    -o (Join-Path $Out "test_fsr31.exe")
if ($LASTEXITCODE -ne 0) {
    throw "FSR 3.1 smoke test compilation failed"
}

$Exe = Join-Path $Out "test_fsr31.exe"
$Runtime = Join-Path $Out "amd_fidelityfx_dx12.dll"
$HeldRuntime = "$Runtime.held-$PID"
& $Exe
if ($LASTEXITCODE -ne 0) {
    throw "FSR 3.1 smoke test failed"
}

Move-Item -LiteralPath $Runtime -Destination $HeldRuntime
try {
    & $Exe --expect-unsupported
    if ($LASTEXITCODE -ne 0) {
        throw "FSR 3.1 missing-runtime fallback test failed"
    }
}
finally {
    Move-Item -LiteralPath $HeldRuntime -Destination $Runtime
}
