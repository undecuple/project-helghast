param(
    [string]$Dll = ""
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Out = Join-Path $Root "build\smoke-host\xess"
$Patched = if ($env:KZ3_PATCHED_DIR) { $env:KZ3_PATCHED_DIR } else { Join-Path $Root "work\ps3recomp-patched" }
$Video = Join-Path $Patched "libs\video"
$Include = Join-Path $Patched "include"
$PinScript = "import sys; sys.path.insert(0, sys.argv[1]); import xess_runtime as x; print(x.FILES['libxess.dll']['sha256'])"
$ExpectedHash = (& python -c $PinScript (Join-Path $Root "tools\release")).Trim().ToUpperInvariant()

if (-not $Dll) {
    $Dll = Join-Path $Root "work\release\xess-sdk-3.0.2\bin\libxess.dll"
}
if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) {
    throw "XeSS runtime not found: $Dll (pass -Dll with bin\libxess.dll from Intel's official XeSS SDK 3.0.2 archive)"
}
if (-not (Test-Path -LiteralPath (Join-Path $Video "rsx_upscale.h") -PathType Leaf)) {
    throw "Patched ps3recomp tree not found: $Patched (run tools/ps3recomp-patched.sh first)"
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $Dll).Hash -ne $ExpectedHash) {
    throw "XeSS runtime hash does not match the pinned SDK 3.0.2 DLL"
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
Copy-Item -LiteralPath $Dll -Destination (Join-Path $Out "libxess.dll") -Force

& $Compiler.Source `
    -std=c++20 -O2 -Wall -Wextra -Werror `
    -Wno-cast-function-type `
    "-I$(Join-Path $Root 'port\kz3')" `
    "-I$Video" `
    "-I$Include" `
    (Join-Path $Root "port\kz3\xess.cpp") `
    (Join-Path $Root "tools\smoke\test_xess.cpp") `
    -ld3d12 -ldxgi -ldxguid -ld3dcompiler `
    -o (Join-Path $Out "test_xess.exe")
if ($LASTEXITCODE -ne 0) {
    throw "XeSS smoke test compilation failed"
}

& $Compiler.Source `
    -std=c++20 -O2 -Wall -Wextra -Werror `
    -Wno-cast-function-type `
    "-I$(Join-Path $Root 'port\kz3')" `
    (Join-Path $Root "tools\smoke\test_xess_temporal.cpp") `
    -ld3d12 -ldxgi -ldxguid `
    -o (Join-Path $Out "test_xess_temporal.exe")
if ($LASTEXITCODE -ne 0) {
    throw "XeSS temporal test compilation failed"
}

$Exe = Join-Path $Out "test_xess.exe"
$Runtime = Join-Path $Out "libxess.dll"
$HeldRuntime = "$Runtime.held-$PID"
& $Exe
if ($LASTEXITCODE -ne 0) {
    throw "XeSS smoke test failed"
}

& $Exe --jitter-override
if ($LASTEXITCODE -ne 0) {
    throw "XeSS independent execute jitter test failed"
}

& (Join-Path $Out "test_xess_temporal.exe")
if ($LASTEXITCODE -ne 0) {
    throw "XeSS temporal test failed"
}

Move-Item -LiteralPath $Runtime -Destination $HeldRuntime
try {
    & $Exe --expect-unsupported
    if ($LASTEXITCODE -ne 0) {
        throw "XeSS missing-runtime fallback test failed"
    }

    Copy-Item -LiteralPath "$env:WINDIR\System32\version.dll" -Destination $Runtime
    try {
        & $Exe --expect-unsupported
        if ($LASTEXITCODE -ne 0) {
            throw "XeSS wrong-runtime fallback test failed"
        }
    }
    finally {
        Remove-Item -LiteralPath $Runtime -Force -ErrorAction SilentlyContinue
    }
}
finally {
    Move-Item -LiteralPath $HeldRuntime -Destination $Runtime
}
