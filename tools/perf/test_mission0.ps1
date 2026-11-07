param([string]$Scratch, [string]$PythonExe)

$ErrorActionPreference = "Stop"
$stage = "setup"
try {
    . (Join-Path $PSScriptRoot "mission0.ps1") -Candidate GCM_FAST_METHODS

    function Assert-Fixture([bool]$Condition) {
        if (-not $Condition) {
            [Console]::Error.WriteLine("fixture assertion line: " + (Get-PSCallStack)[1].ScriptLineNumber)
            throw "fixture assertion failed"
        }
    }

    function Assert-Reason([string]$Expected, [scriptblock]$Action) {
        $actual = ""
        try { & $Action | Out-Null }
        catch { $actual = Get-BenchmarkReason $_ }
        if ($actual -ne $Expected) {
            [Console]::Error.WriteLine("fixture reason mismatch: expected=$Expected actual=$actual")
        }
        Assert-Fixture ($actual -eq $Expected)
    }

    $stage = "schedule"
    $schedule = Get-Schedule 5
    Assert-Fixture ($schedule.Count -eq 12 -and (($schedule -join ",") -eq
        "off,on,on,off,off,on,on,off,off,on,on,off"))
    $baselineSchedule = Get-Schedule 7 -BaselineMode
    Assert-Fixture ($baselineSchedule.Count -eq 7 -and
                    @($baselineSchedule | Where-Object { $_ -ne "off" }).Count -eq 0)
    $baselineSwitches = Get-CandidateSwitches "baseline" "" "off"
    foreach ($name in $script:Candidates) { Assert-Fixture ($baselineSwitches[$name] -eq "0") }
    Assert-Reason "schedule_invalid" { Get-CandidateSwitches "baseline" "" "on" }

    $stage = "window_counts"
    $fiveOff = @(1..5 | ForEach-Object { [PSCustomObject]@{ side = "off"; valid = $true } })
    $fiveOn = @(1..5 | ForEach-Object { [PSCustomObject]@{ side = "on"; valid = $true } })
    Assert-ValidWindowCounts $fiveOff 5 "baseline"
    Assert-ValidWindowCounts ($fiveOff + $fiveOn) 5 "candidate"
    Assert-Reason "insufficient_valid_windows" { Assert-ValidWindowCounts @($fiveOff[0..3]) 5 "baseline" }
    Assert-Reason "insufficient_valid_windows" {
        Assert-ValidWindowCounts (@($fiveOff[0..3]) + @([PSCustomObject]@{ side = "off"; valid = $false })) 5 "baseline"
    }
    Assert-Reason "baseline_window_count_invalid" { Assert-ValidWindowCounts ($fiveOff + $fiveOff[0]) 5 "baseline" }
    Assert-Reason "baseline_window_count_invalid" { Assert-ValidWindowCounts ($fiveOff + $fiveOn[0]) 5 "baseline" }
    Assert-Reason "insufficient_valid_windows" {
        Assert-ValidWindowCounts ($fiveOff + @($fiveOn[0..3])) 5 "candidate"
    }

    $stage = "gpu_samples"
    $samples = @(
        [PSCustomObject]@{ Name = "pid_123_luid_0_engtype_3D"; UtilizationPercentage = 80.0 },
        [PSCustomObject]@{ Name = "pid_456_luid_0_engtype_3D"; UtilizationPercentage = 0.5 }
    )
    $gpu = Convert-GpuSamples $samples 123
    Assert-Fixture ($gpu.status -eq "sampled" -and $gpu.foreign_activity -eq "not_observed" -and
                    $gpu.max_foreign_percent -eq 0.5)
    $gpu = Convert-GpuSamples $samples -1
    Assert-Fixture ($gpu.status -eq "sampled" -and $gpu.foreign_activity -eq "observed")
    $gpu = Convert-GpuSamples @(
        [PSCustomObject]@{ Name = "pid_456_luid_0_eng_0"; UtilizationPercentage = 0.6 },
        [PSCustomObject]@{ Name = "pid_456_luid_0_eng_1"; UtilizationPercentage = 0.6 }
    ) 123
    Assert-Fixture ($gpu.foreign_activity -eq "observed" -and $gpu.max_foreign_percent -eq 1.2)
    Assert-Fixture ((Convert-GpuSamples @() 123).status -eq "unavailable")
    $localized = Convert-GpuSamples @([PSCustomObject]@{ Name = "localized-instance"; UtilizationPercentage = 4 }) 123
    Assert-Fixture ($localized.status -eq "unavailable" -and $localized.foreign_activity -eq "unknown")
    $invalidGpu = Convert-GpuSamples @([PSCustomObject]@{ Name = "pid_456_luid_0_eng_0"; UtilizationPercentage = $null }) 123
    Assert-Fixture ($invalidGpu.status -eq "unavailable" -and $invalidGpu.foreign_activity -eq "unknown")
    $script:FixtureGpuStatus = "sampled"
    function Get-CimInstance {
        [CmdletBinding()]
        param([string]$ClassName, [int]$OperationTimeoutSec)
        Assert-Fixture ($ClassName -eq "Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine" -and
                        $OperationTimeoutSec -eq 2)
        if ($script:FixtureGpuStatus -eq "missing") { throw "synthetic localized counter unavailable" }
        if ($script:FixtureGpuStatus -eq "localized") {
            return [PSCustomObject]@{ Name = "localized-instance"; UtilizationPercentage = 4 }
        }
        return $samples
    }
    Assert-Fixture ((Get-GpuEvidence 123).status -eq "sampled")
    foreach ($status in @("missing", "localized")) {
        $script:FixtureGpuStatus = $status
        $gpu = Get-GpuEvidence 123
        Assert-Fixture ($gpu.status -eq "unavailable" -and $gpu.foreign_activity -eq "unknown")
    }
    Remove-Item function:\Get-CimInstance

    $stage = "launcher_fixture"
    $root = Join-Path $Scratch "SENSITIVE_TEST_MARKER"
    $build = Join-Path $root "build"
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    $fakeExe = Join-Path $build "fixture.exe"
    [IO.File]::Copy($PythonExe, $fakeExe)
    $fixtureEboot = Join-Path $root "synthetic EBOOT with spaces.elf"
    [IO.File]::WriteAllText($fixtureEboot,
        "import json,sys`nprint(json.dumps({'path':sys.argv[0],'count':len(sys.argv)}),flush=True)`nsys.stdin.readline()`n")
    $fakeLauncher = Join-Path $Scratch "fake_launcher.py"
    $metadataFile = Join-Path $Scratch "metadata.json"
    [IO.File]::WriteAllText($fakeLauncher,
        "import os`nprint(open(os.path.join(os.path.dirname(__file__), 'metadata.json'), encoding='utf-8').read())`n")
    $metadata = [ordered]@{
        action = "run"; argv = @($fakeExe); cwd = (Join-Path $root "data")
        env = [ordered]@{
            PS3_VFS_ROOT = (Join-Path $root "read-only-game")
            KZ3_CACHE_ROOT = (Join-Path $root "unused-cache")
            PS3_EDAT_CACHE = (Join-Path $root "unused-edat")
            RSX_SHADER_CACHE_DIR = (Join-Path $root "unused-shaders")
            KZ3_INSTALL_DIR = $root
        }
        ini = [ordered]@{ eboot = $fixtureEboot; font_dir = ""; lang = "" }
        template_env = [ordered]@{}
    }
    function Save-Metadata {
        [IO.File]::WriteAllText($metadataFile, ($metadata | ConvertTo-Json -Depth 8 -Compress),
                                (New-Object Text.UTF8Encoding($false)))
    }
    Save-Metadata
    $launch = Get-LaunchPlan $PythonExe $fakeLauncher $root
    Assert-Fixture ($launch.action -eq "run" -and $launch.argv[0] -eq $fakeExe)
    $metadata.action = "rebuild"
    Save-Metadata
    Assert-Reason "launcher_not_runnable" { Get-LaunchPlan $PythonExe $fakeLauncher $root }
    $developerDir = Join-Path $Scratch "synthetic-developer-build"
    New-Item -ItemType Directory -Path $developerDir | Out-Null
    $developerExe = Join-Path $developerDir "kz3.exe"
    [IO.File]::Copy($PythonExe, $developerExe)
    $metadata.argv = @($developerExe, $fixtureEboot)
    $metadata["eboot_mode"] = "argument"
    $metadata["eboot_sha256"] = Get-FileSha256 $fixtureEboot
    $metadata["cmake_cache_sha256"] = "a" * 64
    Save-Metadata
    $developerLaunch = Get-LaunchPlan $PythonExe $fakeLauncher $root $developerDir
    Assert-Fixture ($developerLaunch.action -eq "rebuild" -and $developerLaunch.argv.Count -eq 2 -and
                    $developerLaunch.argv[0] -eq $developerExe -and $developerLaunch.argv[1] -eq $fixtureEboot)
    $stage = "developer_eboot_argument_quoting"
    $ebootInfo = New-ChildInfo $PythonExe @([string]$developerLaunch.argv[1]) $Scratch $developerLaunch @{}
    $ebootLog = Join-Path $Scratch "synthetic-eboot-argument.log"
    $ebootProcess = Invoke-ControlledProcess $PythonExe $ebootInfo $ebootLog 0.5 3 "stdin"
    $ebootResult = [IO.File]::ReadAllText($ebootLog) | ConvertFrom-Json
    Assert-Fixture ($ebootResult.count -eq 1 -and $ebootResult.path -ceq $fixtureEboot -and
                    $null -eq (Get-Process -Id $ebootProcess.pid -ErrorAction SilentlyContinue))
    $metadata.argv = @($developerExe)
    Save-Metadata
    Assert-Reason "developer_eboot_provenance_invalid" { Get-LaunchPlan $PythonExe $fakeLauncher $root $developerDir }
    $metadata.eboot_mode = "embedded"
    Save-Metadata
    $embeddedLaunch = Get-LaunchPlan $PythonExe $fakeLauncher $root $developerDir
    Assert-Fixture ($embeddedLaunch.eboot_mode -eq "embedded" -and $embeddedLaunch.argv.Count -eq 1)
    $metadata.argv = @($developerExe, $fixtureEboot)
    Save-Metadata
    Assert-Reason "developer_eboot_provenance_invalid" { Get-LaunchPlan $PythonExe $fakeLauncher $root $developerDir }
    $metadata.eboot_mode = "argument"
    $metadata.ini.eboot = Join-Path $root "other fixture.elf"
    Save-Metadata
    Assert-Reason "developer_recorded_paths_invalid" { Get-LaunchPlan $PythonExe $fakeLauncher $root $developerDir }
    $metadata.ini.eboot = $fixtureEboot
    $metadata.eboot_sha256 = "0" * 64
    Save-Metadata
    Assert-Reason "developer_eboot_changed" { Get-LaunchPlan $PythonExe $fakeLauncher $root $developerDir }
    $metadata.eboot_sha256 = Get-FileSha256 $fixtureEboot
    $stage = "launcher_fixture"
    $metadata.action = "resume"
    Save-Metadata
    Assert-Reason "launcher_not_runnable" { Get-LaunchPlan $PythonExe $fakeLauncher $root $developerDir }
    $metadata.action = "rebuild"
    $metadata.argv = @($fakeExe)
    Save-Metadata
    Assert-Reason "launcher_output_invalid" { Get-LaunchPlan $PythonExe $fakeLauncher $root $developerDir }
    $metadata.action = "run"
    $metadata.argv = @($fakeExe)
    $metadata.env.PS3_VFS_ROOT = Join-Path $root "vfs"
    Save-Metadata
    Assert-Reason "link_free_launcher_required" { Get-LaunchPlan $PythonExe $fakeLauncher $root }
    $metadata.env.PS3_VFS_ROOT = Join-Path $root "read-only-game"
    Save-Metadata
    $launch = Get-LaunchPlan $PythonExe $fakeLauncher $root

    $stage = "staged_ini"
    $iniPath = Join-Path $Scratch "fixture-kz3.ini"
    $ini = [PSCustomObject]@{ eboot = (Join-Path $root "fixture.elf"); font_dir = ""; lang = "1" }
    Write-StagedIni $iniPath $root (Join-Path $root "data") $ini
    $iniText = [IO.File]::ReadAllText($iniPath, [Text.Encoding]::Default)
    Assert-Fixture ($iniText.Contains('data_dir = "' + (Join-Path $root "data") + '"') -and
                    $iniText.Contains('eboot = "' + $ini.eboot + '"') -and
                    -not $iniText.Contains('lang ='))
    Assert-Reason "ini_value_invalid" { ConvertTo-IniValue "bad`nvalue" }

    $stage = "child_environment_and_arguments"
    Assert-Fixture ((Join-WindowsArguments $null) -ceq "")
    Assert-Fixture ((Join-WindowsArguments ([string[]]@())) -ceq "")
    Assert-Fixture ((Join-WindowsArguments ([string[]]@(""))) -ceq '""')
    $leaks = @("CELLMARK_IGPU", "NV3089_ROWCOPY", "PPU_POKE", "PS3RECOMP_RSX_ENGINE", "LD_CB_REUSE",
               "KZ3_UNSAFE_TEST", "GCM_OTHER", "RSX_OTHER", "PAD_OTHER", "VK_LAYER_PATH", "SDL_OTHER",
               "SPU_OTHER", "FS_OTHER", "FLOW_OTHER", "YDKJ_OTHER", "PYTHONPATH", "UNLISTED_OTHER",
               "__COMPAT_LAYER", "DXVK_TEST", "D3D12_TEST")
    foreach ($name in $leaks) { [Environment]::SetEnvironmentVariable($name, "inherited", "Process") }
    [Environment]::SetEnvironmentVariable("KZ3_WINDOW_SIZE", "800x600", "Process")
    $code = 'import hashlib,json,os,sys; names="CELLMARK_IGPU NV3089_ROWCOPY PPU_POKE PS3RECOMP_RSX_ENGINE LD_CB_REUSE KZ3_UNSAFE_TEST GCM_OTHER RSX_OTHER PAD_OTHER VK_LAYER_PATH SDL_OTHER SPU_OTHER FS_OTHER FLOW_OTHER YDKJ_OTHER PYTHONPATH UNLISTED_OTHER __COMPAT_LAYER DXVK_TEST D3D12_TEST".split(); print(json.dumps({"args":sys.argv[1:],"flag":os.getenv("GCM_FAST_METHODS"),"window_size":os.getenv("KZ3_WINDOW_SIZE"),"leaks":{n:os.getenv(n) for n in names},"systemroot":bool(os.getenv("SystemRoot")),"launcher":bool(os.getenv("PS3_VFS_ROOT")),"cwd_name":os.path.basename(os.getcwd()),"script_sha":hashlib.sha256(os.environ["PAD_SCRIPT"].encode()).hexdigest()}),flush=True); print("synthetic-stderr",file=sys.stderr,flush=True); sys.stdin.readline()'
    $unicodeArgument = "fixture-$([char]0x4e2d)-$([char]::ConvertFromUtf32(0x1f680))"
    $unicodeDirectory = Join-Path $Scratch ("fixture-" + [char]0x4e2d)
    New-Item -ItemType Directory -Path $unicodeDirectory | Out-Null
    $arguments = [string[]]@("-B", "-c", $code, "two words", 'quote"inside', 'backslash\', "", $unicodeArgument)
    $syntheticScript = "first line`nsecond line"
    $info = New-ChildInfo $PythonExe $arguments $unicodeDirectory $launch @{
        GCM_FAST_METHODS = "1"; KZ3_WINDOW_SIZE = "1920x1080"; PAD_SCRIPT = $syntheticScript
    }
    foreach ($name in $script:WindowsEnvironment) {
        $inherited = [Environment]::GetEnvironmentVariable($name, "Process")
        if ($null -ne $inherited) {
            Assert-Fixture ($info.EnvironmentVariables.ContainsKey($name) -and
                            $info.EnvironmentVariables[$name] -ceq $inherited)
        }
    }
    $combined = Join-Path $Scratch "child-combined.txt"
    $controlled = Invoke-ControlledProcess $PythonExe $info $combined 0.5 3 "stdin"
    $lines = [IO.File]::ReadAllLines($combined)
    $childResult = $lines[0] | ConvertFrom-Json
    Assert-Fixture ($childResult.flag -eq "1" -and $childResult.window_size -eq "1920x1080" -and
                    [Environment]::GetEnvironmentVariable("KZ3_WINDOW_SIZE", "Process") -eq "800x600" -and
                    $childResult.systemroot -and $childResult.launcher)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $expectedSha = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($syntheticScript))).Replace("-", "").ToLowerInvariant() }
    finally { $sha.Dispose() }
    Assert-Fixture ($childResult.script_sha -eq $expectedSha)
    foreach ($name in $leaks) {
        Assert-Fixture ($null -eq $childResult.leaks.PSObject.Properties[$name].Value)
        Assert-Fixture ([Environment]::GetEnvironmentVariable($name, "Process") -eq "inherited")
    }
    Assert-Fixture ($childResult.args.Count -eq 5 -and $childResult.args[0] -eq "two words" -and
                    $childResult.args[1] -eq 'quote"inside' -and $childResult.args[2] -eq 'backslash\' -and
                    $childResult.args[3] -eq "" -and $childResult.args[4] -ceq $unicodeArgument -and
                    $childResult.cwd_name -ceq ("fixture-" + [char]0x4e2d))
    Assert-Fixture ($lines[1] -eq "synthetic-stderr")
    Assert-Fixture ($controlled.pid -gt 0 -and $null -eq (Get-Process -Id $controlled.pid -ErrorAction SilentlyContinue))
    $launch.env | Add-Member -NotePropertyName "PPU_POKE" -NotePropertyValue "1"
    Assert-Reason "launcher_output_invalid" { New-ChildInfo $PythonExe @() $Scratch $launch @{} }
    $launch.env.PSObject.Properties.Remove("PPU_POKE")
    Assert-Reason "override_unsupported" { New-ChildInfo $PythonExe @() $Scratch $launch @{ NV3089_ROWCOPY = "1" } }

    $stage = "shared_output_order"
    $code = 'import sys; [(print("o"+str(i),flush=True),print("e"+str(i),file=sys.stderr,flush=True)) for i in range(10)]; sys.stdin.readline()'
    $info = New-ChildInfo $PythonExe @("-B", "-c", $code) $Scratch $launch @{}
    $combined = Join-Path $Scratch "ordered.log"
    [void](Invoke-ControlledProcess $PythonExe $info $combined 0.5 3 "stdin")
    $lines = [IO.File]::ReadAllLines($combined)
    Assert-Fixture ($lines.Count -eq 20)
    for ($i = 0; $i -lt 10; $i++) {
        Assert-Fixture ($lines[2 * $i] -eq "o$i" -and $lines[2 * $i + 1] -eq "e$i")
    }
    Assert-Reason "log_destination_exists" {
        Invoke-ControlledProcess $PythonExe $info $combined 0.5 1 "stdin"
    }
    Assert-Fixture ([IO.File]::ReadAllLines($combined).Count -eq 20)

    $stage = "pid_reuse_guard"
    $innocentInfo = New-ChildInfo $PythonExe @("-B", "-c", "import time; time.sleep(20)") $Scratch $launch @{}
    $innocent = [Diagnostics.Process]::Start($innocentInfo)
    $realHelper = $script:Helper
    try {
        $fakeSupervisor = Join-Path $Scratch "fake-supervisor.py"
        $fakeText = "import json,sys`nsys.stdin.readline()`n" +
            "print(json.dumps({'event':'started','pid':$($innocent.Id),'created_filetime':'0'}),flush=True)`n" +
            "sys.stdin.read()`n"
        [IO.File]::WriteAllText($fakeSupervisor, $fakeText, [Text.Encoding]::ASCII)
        $script:Helper = $fakeSupervisor
        Assert-Reason "process_tracking_mismatch" {
            Invoke-ControlledProcess $PythonExe $info (Join-Path $Scratch "unused.log") 0.5 1 "stdin"
        }
        Assert-Fixture (-not $innocent.HasExited)
    } finally {
        $script:Helper = $realHelper
        if (-not $innocent.HasExited) { $innocent.Kill(); [void]$innocent.WaitForExit(5000) }
        $innocent.Dispose()
    }

    $stage = "early_exit"
    $info = New-ChildInfo $PythonExe @("-B", "-c", "import sys; sys.exit(17)") $Scratch $launch @{}
    Assert-Reason "process_crashed" {
        Invoke-ControlledProcess $PythonExe $info (Join-Path $Scratch "crash.log") 2 1 "stdin"
    }
    $info = New-ChildInfo $PythonExe @("-B", "-c", "import sys; sys.exit(0)") $Scratch $launch @{}
    Assert-Reason "process_ended_early" {
        Invoke-ControlledProcess $PythonExe $info (Join-Path $Scratch "early.log") 2 1 "stdin"
    }
    $info = New-ChildInfo $PythonExe @("-B", "-c", "import sys; sys.stdin.readline(); sys.exit(17)") $Scratch $launch @{}
    Assert-Reason "process_crashed" {
        Invoke-ControlledProcess $PythonExe $info (Join-Path $Scratch "close-crash.log") 0.5 1 "stdin"
    }

    $stage = "shutdown_timeout"
    $info = New-ChildInfo $PythonExe @("-B", "-c", "import os,time; open('timeout.pid','w').write(str(os.getpid())); time.sleep(20)") $Scratch $launch @{}
    Assert-Reason "shutdown_timeout" {
        Invoke-ControlledProcess $PythonExe $info (Join-Path $Scratch "timeout.log") 0.5 0.5 "stdin"
    }
    $timedPid = [int][IO.File]::ReadAllText((Join-Path $Scratch "timeout.pid"))
    Assert-Fixture ($timedPid -gt 0 -and $null -eq (Get-Process -Id $timedPid -ErrorAction SilentlyContinue))

    $stage = "no_window_cleanup"
    $probeRoute = Invoke-JsonTool $PythonExe @("-B", $script:Helper, "route", "--kind", "first-boot")
    $bootProbe = @{ PAD_SCRIPT = $probeRoute.script }
    foreach ($name in $script:Candidates) { $bootProbe[$name] = "0" }
    $info = New-ChildInfo $PythonExe @("-B", "-c", "import os,time; open('window.pid','w').write(str(os.getpid())); time.sleep(20)") $Scratch $launch $bootProbe
    Assert-Reason "close_window_unavailable" {
        Invoke-ControlledProcess $PythonExe $info (Join-Path $Scratch "window.log") 0.5 1 "window"
    }
    $windowPid = [int][IO.File]::ReadAllText((Join-Path $Scratch "window.pid"))
    Assert-Fixture ($windowPid -gt 0 -and $null -eq (Get-Process -Id $windowPid -ErrorAction SilentlyContinue))

    $stage = "summary_privacy"
    $summary = [ordered]@{ status = "failed"; runs = @([ordered]@{
        run = 1; mode = "candidate"; candidate = "GCM_FAST_METHODS"
        side = "off"; valid = $false; reason = "pad_warning"; fps_mean_flip_approx = $null
        gpu_time_status = "unavailable"
    }) }
    Write-Summary $Scratch $summary
    $summaryText = [IO.File]::ReadAllText((Join-Path $Scratch "summary.json")) +
                   [IO.File]::ReadAllText((Join-Path $Scratch "summary.csv"))
    Assert-Fixture (-not $summaryText.Contains("SENSITIVE_TEST_MARKER") -and -not $summaryText.Contains($Scratch))

    $stage = "full_synthetic_matrix"
    $sourceData = Join-Path $root "data\gamedata"
    $profileDir = Join-Path $sourceData "dev_hdd0\home\00000001\savedata\fixture"
    New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
    $profilePath = Join-Path $profileDir "PROFILE.DAT"
    $profileBytes = New-Object byte[] 0x11EE8
    $profileBytes[2] = 1
    $profileBytes[3] = 0x27
    [IO.File]::WriteAllBytes($profilePath, $profileBytes)
    $profileHash = Get-FileSha256 $profilePath
    $realJsonTool = (Get-Item function:Invoke-JsonTool).ScriptBlock
    $realSummary = (Get-Item function:Write-Summary).ScriptBlock
    $script:CreatedSessions = New-Object System.Collections.Generic.List[string]
    $script:FakeCalls = New-Object System.Collections.Generic.List[string]
    $script:ActualSwitches = New-Object System.Collections.Generic.List[object]
    $script:FakeCrashAt = 0
    $script:FakeBadEvidenceAt = 0
    $script:FakeAssetDrift = $false
    $script:FakeTranslationDrift = $false
    $script:FakeTranslationMetadata = ""
    $script:FakeDeveloper = $false
    $script:FakeEbootMode = "argument"
    $script:FakeProvenanceMismatch = ""
    $script:FakeEbootDrift = $false
    $script:FakeGpuSamples = 0
    $script:FakeBootstrapOutcome = "success"
    $script:FakeBootstrapCalls = 0
    function Convert-PadAxis([double]$Value, [bool]$Trigger) {
        $encoded = if ($Trigger) { $Value * 255.0 } else { 128.0 + $Value * 128.0 }
        return [int][Math]::Floor([Math]::Max(0.0, [Math]::Min(255.0, $encoded)) + 0.5)
    }
    function Convert-PadAction([string]$Body) {
        $parts = $Body.Split('/', 2)
        $values = $parts[0].Split(',')
        if ($values.Count -eq 1) {
            $mask = [Convert]::ToInt32($values[0].Substring(2), 16)
            $hold = if ($parts.Count -eq 2) {
                $seconds = [double]::Parse($parts[1], [Globalization.CultureInfo]::InvariantCulture)
                $seconds.ToString("F2", [Globalization.CultureInfo]::InvariantCulture) + "s"
            } else { "40 polls" }
            return ("press 0x{0:X4} for {1}" -f $mask, $hold)
        }
        $axis = New-Object System.Collections.Generic.List[int]
        for ($index = 0; $index -lt 6; $index++) {
            $value = if ($index -lt $values.Count) {
                [double]::Parse($values[$index], [Globalization.CultureInfo]::InvariantCulture)
            } else { 0.0 }
            $axis.Add((Convert-PadAxis $value ($index -ge 4)))
        }
        $duration = if ($parts.Count -eq 2) {
            [double]::Parse($parts[1], [Globalization.CultureInfo]::InvariantCulture)
        } else { 1.0 }
        return ("sticks l=({0},{1}) r=({2},{3}) l2={4} r2={5} for {6}s" -f
                $axis[0], $axis[1], $axis[2], $axis[3], $axis[4], $axis[5],
                $duration.ToString("F2", [Globalization.CultureInfo]::InvariantCulture))
    }
    $hostBase = 1000000
    $routeFixture = & $realJsonTool $PythonExe @("-B", $script:Helper, "route")
    $missionStartPattern = '^\[cellPad\] script t=' +
        [regex]::Escape(([double]$routeFixture.mission_start).ToString("F1", [Globalization.CultureInfo]::InvariantCulture)) +
        's'
    $bootstrapRouteFixture = & $realJsonTool $PythonExe @("-B", $script:Helper, "route", "--kind", "first-boot")
    $bootstrapLines = New-Object System.Collections.Generic.List[string]
    $bootstrapLines.Add("[cellPad] PAD_SCRIPT: $($bootstrapRouteFixture.event_count) event(s)")
    foreach ($line in ($bootstrapRouteFixture.script -split "`n")) {
        if ($line -match '^(\d+(?:\.\d+)?):(.+)$') {
            $time = ([double]$Matches[1]).ToString("F1", [Globalization.CultureInfo]::InvariantCulture)
            $action = Convert-PadAction $Matches[2]
            $hostMs = $hostBase + [int]([double]$Matches[1] * 1000)
            $bootstrapLines.Add("[cellPad] script t=${time}s $action (at ${time}s) host=$hostMs ms")
        }
    }
    $eventsByTick = @{}
    foreach ($line in ($routeFixture.script -split "`n")) {
        if ($line -match '^(\d+(?:\.\d+)?):(.+)$') {
            $time = [double]$Matches[1]
            if ($time -lt $routeFixture.window_end) {
                $tick = [int]($time * 4 + 2)
                $printedTime = $time.ToString("F1", [Globalization.CultureInfo]::InvariantCulture)
                $action = Convert-PadAction $Matches[2]
                $hostMs = $hostBase + 250 * $tick
                if (-not $eventsByTick.ContainsKey($tick)) {
                    $eventsByTick[$tick] = New-Object System.Collections.Generic.List[string]
                }
                $eventsByTick[$tick].Add(
                    "[cellPad] script t=${printedTime}s $action (at ${printedTime}s) host=$hostMs ms")
            }
        }
    }
    $combinedLines = New-Object System.Collections.Generic.List[string]
    $combinedLines.Add("[cellPad] PAD_SCRIPT: $($routeFixture.event_count) event(s)")
    $lastTick = [int](($routeFixture.window_end + 6) * 4)
    for ($i = 0; $i -le $lastTick; $i++) {
        $flipMs = 250 * $i
        $combinedLines.Add(("[flip] {0} at {1} ms host={2} ms" -f
                            (1 + [int][Math]::Round(7.5 * $i)), $flipMs, ($hostBase + $flipMs)))
        if ($i -eq 120) {
            $combinedLines.Add("[fs] open 'SENSITIVE_TEST_MARKER/levels.single_player.mission_0.fixture' -> fd 1")
        }
        if ($eventsByTick.ContainsKey($i)) {
            foreach ($eventLine in $eventsByTick[$i]) { $combinedLines.Add($eventLine) }
        }
    }
    function Invoke-JsonTool([string]$Executable, [string[]]$Arguments) {
        if ($Arguments -contains "verify-build") {
            return [PSCustomObject]@{ ok = $true; patchset = "0123456789abcdef-1234567"; git_head = ("0" * 40) }
        }
        if ($Arguments -contains "verify-developer-build") {
            $requestedBuild = $Arguments[[Array]::IndexOf($Arguments, "--build") + 1]
            Assert-Fixture ($requestedBuild -eq $developerDir)
            return [PSCustomObject]@{
                ok = $true; patchset = "0123456789abcdef-1234567"; git_head = ("0" * 40)
                exe_sha256 = (Get-FileSha256 $developerExe); cmake_cache_sha256 = ("a" * 64)
                cmake_generator = "Ninja"; configure_toolkit_stamp = "not_recorded"
                freshness = "ninja_dry_run_clean"
                eboot_mode = $script:FakeEbootMode
                eboot_sha256 = if ($script:FakeEbootMode -eq "embedded") { Get-FileSha256 $fixtureEboot } else { $null }
            }
        }
        if ($Arguments -contains "stage-resources") {
            $forwarded = [string[]]@($Arguments)
            if ($Arguments -contains "--required") {
                $repoIndex = [Array]::IndexOf($forwarded, "--repo")
                Assert-Fixture ($repoIndex -ge 0 -and $forwarded[$repoIndex + 1] -eq $script:Repo)
                $forwarded[$repoIndex + 1] = $fixtureRepo
                if ($script:FakeTranslationDrift -and $script:FakeCalls.Count -gt 0) {
                    [IO.File]::WriteAllText($fixtureTranslation, "changed-repository-language")
                }
            }
            $result = & $realJsonTool $Executable $forwarded
            if ($Arguments -contains "--required") {
                if ($script:FakeTranslationMetadata -eq "source") {
                    $result.ui_translations_source = "SENSITIVE_TEST_MARKER"
                } elseif ($script:FakeTranslationMetadata -eq "hash") {
                    $result.ui_translations_sha256 = "0" * 64
                }
            }
            if ($script:FakeAssetDrift -and $script:FakeCalls.Count -gt 0) {
                $app = $Arguments[[Array]::IndexOf($Arguments, "--app") + 1]
                [IO.File]::WriteAllText((Join-Path $app "drift.dll"), "synthetic drift")
            }
            return $result
        }
        return (& $realJsonTool $Executable $Arguments)
    }
    function Get-LaunchPlan([string]$PythonPath, [string]$LauncherScript, [string]$Install, [string]$Developer = "") {
        if ($Developer) {
            Assert-Fixture ($Developer -eq $developerDir)
            $bootMode = if ($script:FakeProvenanceMismatch -eq "mode") {
                if ($script:FakeEbootMode -eq "argument") { "embedded" } else { "argument" }
            } else { $script:FakeEbootMode }
            [string[]]$argv = if ($bootMode -eq "argument") { @($developerExe, $fixtureEboot) } else { @($developerExe) }
            return [PSCustomObject]@{
                action = "rebuild"; argv = $argv; cwd = $launch.cwd; env = $launch.env
                ini = $launch.ini; template_env = $launch.template_env; eboot_mode = $bootMode
                eboot_sha256 = if ($script:FakeProvenanceMismatch -eq "eboot") { "0" * 64 }
                               else { Get-FileSha256 $fixtureEboot }
                cmake_cache_sha256 = if ($script:FakeProvenanceMismatch -eq "cache") { "b" * 64 }
                                      else { "a" * 64 }
            }
        }
        return $launch
    }
    function Get-GpuEvidence([int]$AllowedPid) {
        $script:FakeGpuSamples++
        if ($script:FakeGpuSamples -eq 1) {
            return @{ status = "sampled"; foreign_activity = "observed"; max_foreign_percent = 2.0 }
        }
        return @{ status = "unavailable"; foreign_activity = "unknown"; max_foreign_percent = $null }
    }
    function Write-Summary([string]$Directory, $Summary) {
        if (-not $script:CreatedSessions.Contains($Directory)) { $script:CreatedSessions.Add($Directory) }
        & $realSummary $Directory $Summary
    }
    function Invoke-ControlledProcess(
        [string]$PythonPath, [System.Diagnostics.ProcessStartInfo]$Info, [string]$Log,
        [double]$DurationSeconds, [double]$GraceSeconds, [string]$CloseMode = "window"
    ) {
        $envs = $Info.EnvironmentVariables
        if ([string]$envs["PAD_SCRIPT"] -ceq $bootstrapRouteFixture.script) {
            $script:FakeBootstrapCalls++
            $bootDir = Split-Path -Parent $Log
            $app = Split-Path -Parent $Info.FileName
            Assert-Fixture ($script:FakeDeveloper -and $bootDir.EndsWith("-bootstrap") -and
                            $DurationSeconds -eq ($bootstrapRouteFixture.duration_seconds + 15) -and
                            $GraceSeconds -eq 10 -and $CloseMode -eq "window" -and
                            $Info.WorkingDirectory -eq (Join-Path $bootDir "data") -and
                            $envs["PS3_VFS_ROOT"] -eq $launch.env.PS3_VFS_ROOT -and
                            $envs["KZ3_INSTALL_DIR"] -eq $root -and
                            $envs["KZ3_WINDOW_SIZE"] -eq "1920x1080" -and
                            $envs["PS3_LANG"] -eq "1" -and $envs["KZ3_LANG"] -eq "1" -and
                            $envs["LD_PSO_TEMPLATE"] -eq "0" -and $envs["PAD_CLOCK"] -eq "wall" -and
                            $envs["KZ3_CACHE_ROOT"].StartsWith($bootDir) -and
                            $envs["PS3_HDD0_ROOT"].StartsWith($bootDir) -and
                            (Get-FileSha256 $Info.FileName) -eq (Get-FileSha256 $developerExe))
            $expectedArgs = if ($script:FakeEbootMode -eq "argument") { ConvertTo-WindowsArgument $fixtureEboot }
                            else { "" }
            Assert-Fixture ($Info.Arguments -ceq $expectedArgs)
            foreach ($name in $script:Candidates) { Assert-Fixture ($envs[$name] -eq "0") }
            foreach ($name in $leaks) {
                if ($name -ne "LD_CB_REUSE") { Assert-Fixture (-not $envs.ContainsKey($name)) }
            }
            foreach ($item in @(@("SDL3.dll", "install-sdl"), @("install-only.dll", "install-dll"),
                                 @("lang\port_ui.tsv", "repository-language"), @("fonts\Rajdhani-Medium.ttf", "medium"),
                                 @("fonts\Rajdhani-Bold.ttf", "bold"))) {
                Assert-Fixture ([IO.File]::ReadAllText((Join-Path $app $item[0])) -eq $item[1])
            }
            if ($script:FakeBootstrapOutcome -in @("process_crashed", "process_timeout", "shutdown_timeout")) {
                Stop-Benchmark $script:FakeBootstrapOutcome
            }
            if ($script:FakeBootstrapOutcome -ne "missing") {
                $save = if ($script:FakeBootstrapOutcome -eq "layout") {
                    Join-Path $envs["PS3_HDD0_ROOT"] "unexpected"
                } else { Join-Path $envs["PS3_HDD0_ROOT"] "home\00000001\savedata\bootstrap" }
                New-Item -ItemType Directory -Force -Path $save | Out-Null
                $bytes = [byte[]]$profileBytes.Clone()
                $bytes[0x119AB] = 12
                if ($script:FakeBootstrapOutcome -eq "invalid") { $bytes[3] = 0 }
                if ($script:FakeBootstrapOutcome -eq "incomplete") { $bytes = [byte[]]@(1, 2, 3) }
                [IO.File]::WriteAllBytes((Join-Path $save "PROFILE.DAT"), $bytes)
                if ($script:FakeBootstrapOutcome -eq "multiple") {
                    $other = Join-Path $envs["PS3_HDD0_ROOT"] "home\00000001\savedata\second"
                    New-Item -ItemType Directory -Force -Path $other | Out-Null
                    [IO.File]::WriteAllBytes((Join-Path $other "PROFILE.DAT"), $bytes)
                }
            }
            $outputLines = if ($script:FakeBootstrapOutcome -eq "pad_incomplete") {
                @($bootstrapLines[0..($bootstrapLines.Count - 2)])
            } elseif ($script:FakeBootstrapOutcome -eq "pad_warning") {
                @($bootstrapLines) + "[cellPad] PAD_SCRIPT: WARNING SENSITIVE_TEST_MARKER"
            } elseif ($script:FakeBootstrapOutcome -eq "crash_log") {
                @($bootstrapLines) + "[CRASH] SENSITIVE_TEST_MARKER"
            } else { $bootstrapLines }
            [IO.File]::WriteAllLines($Log, [string[]]$outputLines, [Text.Encoding]::ASCII)
            if ($script:FakeBootstrapOutcome -eq "asset_drift") {
                [IO.File]::WriteAllText((Join-Path $root "build\install-only.dll"), "changed-install-dll")
            }
            return @{ exit_code = 0; pid = 123 }
        }
        $side = if ($envs["GCM_FAST_METHODS"] -eq "1") { "on" } else { "off" }
        $script:FakeCalls.Add($side)
        $actual = @{}
        foreach ($name in $script:LiteralSwitches) { $actual[$name] = [string]$envs[$name] }
        $script:ActualSwitches.Add($actual)
        if ($script:FakeCalls.Count -eq $script:FakeCrashAt) { Stop-Benchmark "process_crashed" }
        Assert-Fixture ($DurationSeconds -eq ($routeFixture.duration_seconds + 15) -and
                        $envs["KZ3_WINDOW_SIZE"] -eq "1920x1080" -and
                        $envs["PS3_LANG"] -eq "1" -and $envs["KZ3_LANG"] -eq "1" -and
                        $envs["LD_PSO_TEMPLATE"] -eq "0" -and $envs["PAD_CLOCK"] -eq "wall")
        Assert-Fixture ($envs["KZ3_CACHE_ROOT"].Contains("run-") -and
                        (Test-Path -LiteralPath $envs["KZ3_CACHE_ROOT"] -PathType Container))
        Assert-Fixture (($side -eq "on" -or $envs["GCM_FAST_METHODS"] -eq "0") -and
                        $envs["LD_CB_REUSE"] -eq "0" -and $envs["KZ3_SPU_TASK_POOL"] -eq "0" -and
                        $envs["KZ3_PARK_IDLE_WAKE"] -eq "0" -and -not $envs.ContainsKey("KZ3_UNSAFE_TEST"))
        foreach ($name in $leaks) {
            if ($name -ne "LD_CB_REUSE") { Assert-Fixture (-not $envs.ContainsKey($name)) }
        }
        if ($script:FakeDeveloper) {
            $app = Split-Path -Parent $Info.FileName
            Assert-Fixture ((Get-FileSha256 $Info.FileName) -eq (Get-FileSha256 $developerExe))
            $expectedArgs = if ($script:FakeEbootMode -eq "argument") { ConvertTo-WindowsArgument $fixtureEboot }
                            else { "" }
            Assert-Fixture ($Info.Arguments -ceq $expectedArgs)
            foreach ($item in @(@("SDL3.dll", "install-sdl"), @("install-only.dll", "install-dll"),
                                 @("lang\port_ui.tsv", "repository-language"), @("fonts\Rajdhani-Medium.ttf", "medium"),
                                 @("fonts\Rajdhani-Bold.ttf", "bold"))) {
                Assert-Fixture ([IO.File]::ReadAllText((Join-Path $app $item[0])) -eq $item[1])
            }
            Assert-Fixture (-not (Test-Path -LiteralPath (Join-Path $app "developer-only.dll")))
        } else {
            Assert-Fixture ($Info.Arguments -ceq "")
        }
        $outputLines = if ($script:FakeCalls.Count -eq $script:FakeBadEvidenceAt) {
            @($combinedLines | Where-Object { $_ -notmatch $missionStartPattern })
        } else { $combinedLines }
        [IO.File]::WriteAllLines($Log, [string[]]$outputLines, [Text.Encoding]::ASCII)
        if ($script:FakeEbootDrift -and $script:FakeCalls.Count -eq 1) {
            [IO.File]::WriteAllText($fixtureEboot, "changed-synthetic-eboot")
        }
        return @{ exit_code = 0 }
    }
    $Candidate = "GCM_FAST_METHODS"
    $SamplesPerSide = 5
    $InstallRoot = $root
    $ProfileSource = $sourceData
    $Python = $PythonExe
    $Plan = $false
    $ConfirmExclusiveGpu = $true
    foreach ($name in $script:Candidates) { [Environment]::SetEnvironmentVariable($name, "1", "Process") }
    try {
        $stage = "overlap_preflight"
        $ProfileSource = $script:Repo
        $workBefore = @(Get-ChildItem -LiteralPath (Join-Path $script:Repo "work\helsari-p0") -Directory -Filter "run-*").Count
        Assert-Reason "profile_source_overlap" { Invoke-MissionBenchmark }
        $workAfter = @(Get-ChildItem -LiteralPath (Join-Path $script:Repo "work\helsari-p0") -Directory -Filter "run-*").Count
        Assert-Fixture ($workBefore -eq $workAfter -and $script:CreatedSessions.Count -eq 0)
        $ProfileSource = $sourceData
        $stage = "full_synthetic_matrix"
        Invoke-MissionBenchmark | Out-Null
        Assert-Fixture ($script:CreatedSessions.Count -eq 1)
        $complete = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[0] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($complete.status -eq "complete" -and $complete.reason -eq "ok" -and
                        $complete.mode -eq "candidate" -and $complete.candidate -eq "GCM_FAST_METHODS" -and
                        $complete.conditions.resolution -eq "1920x1080" -and
                        $complete.runs.Count -eq 12 -and $complete.build.app_assets_sha256.Length -eq 64 -and
                        $complete.valid_window_counts.off -eq 6 -and $complete.valid_window_counts.on -eq 6)
        Assert-Fixture (($script:FakeCalls -join ",") -eq ($schedule -join ","))
        Assert-Fixture (@($complete.runs | Where-Object { -not $_.valid -or $_.fps_mean_flip_approx -ne 30 }).Count -eq 0)
        Assert-Fixture ($complete.gpu_exclusivity -eq "unverified" -and
                        $complete.gpu_preflight.status -eq "sampled" -and
                        $complete.gpu_preflight.foreign_activity -eq "observed")
        for ($i = 0; $i -lt $complete.runs.Count; $i++) {
            $run = $complete.runs[$i]
            $effective = $run.effective_environment
            Assert-Fixture ($run.mode -eq "candidate" -and $run.candidate -eq "GCM_FAST_METHODS" -and
                            $effective.environment_policy -eq "windows_allowlist_v1" -and
                            $effective.switches.KZ3_WINDOW_SIZE -eq "1920x1080" -and
                            $effective.pad_script_sha256 -eq $routeFixture.sha256 -and
                            $effective.path_variables.PS3_VFS_ROOT -eq "<redacted>" -and
                            $run.gpu_before.status -eq "unavailable" -and $run.gpu_after.status -eq "unavailable" -and
                            $run.pad_clock_alignment.method -eq "shared_monotonic_timestamp")
            Assert-Fixture (@($effective.switches.PSObject.Properties).Count -eq $script:LiteralSwitches.Count)
            foreach ($name in $script:LiteralSwitches) {
                Assert-Fixture ([string]$effective.switches.PSObject.Properties[$name].Value -ceq
                                $script:ActualSwitches[$i][$name])
            }
            $runName = "run-{0:D3}-{1}" -f $run.run, $run.side
            $displayPath = Join-Path (Join-Path $script:CreatedSessions[0] $runName) "data\display.ini"
            $displayLines = [IO.File]::ReadAllLines($displayPath)
            Assert-Fixture ($displayLines.Count -eq 12 -and $displayLines[0] -eq "[display]")
            foreach ($setting in $complete.conditions.display_settings.PSObject.Properties) {
                Assert-Fixture ($displayLines -contains ("{0} = {1}" -f $setting.Name, $setting.Value))
            }
            Assert-Fixture ($effective.switches.GCM_FAST_METHODS -eq $(if ($run.side -eq "on") { "1" } else { "0" }) -and
                            $effective.switches.LD_CB_REUSE -eq "0" -and $effective.switches.GCM_FLIPCOUNT -eq "1" -and
                            $effective.switches.PAD_CLOCK -eq "wall" -and $effective.switches.PS3_FSLOG -eq "1")
        }
        $completeText = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[0] "summary.json")) +
                        [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[0] "summary.csv"))
        Assert-Fixture (-not $completeText.Contains("SENSITIVE_TEST_MARKER") -and
                        -not $completeText.Contains($Scratch) -and -not $completeText.Contains($script:Repo))
        $candidateCsv = @(Import-Csv -LiteralPath (Join-Path $script:CreatedSessions[0] "summary.csv"))
        Assert-Fixture ($candidateCsv.Count -eq 12 -and
                        @($candidateCsv | Where-Object { $_.mode -ne "candidate" -or
                                                          $_.candidate -ne "GCM_FAST_METHODS" }).Count -eq 0)
        Assert-Fixture ((Get-FileSha256 $profilePath) -eq $profileHash)

        $stage = "full_synthetic_baseline"
        $Candidate = ""
        $Baseline = $true
        $SamplesPerSide = 7
        $script:FakeCalls.Clear()
        $script:ActualSwitches.Clear()
        Invoke-MissionBenchmark | Out-Null
        Assert-Fixture ($script:CreatedSessions.Count -eq 2)
        $baselineSummary = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[1] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($baselineSummary.status -eq "complete" -and $baselineSummary.reason -eq "ok" -and
                        $baselineSummary.mode -eq "baseline" -and $null -eq $baselineSummary.candidate -and
                        $baselineSummary.conditions.mode -eq "baseline" -and $null -eq $baselineSummary.conditions.candidate -and
                        $baselineSummary.conditions.resolution -eq "1920x1080" -and
                        $baselineSummary.conditions.requested_valid_baseline_windows -eq 7 -and
                        $baselineSummary.runs.Count -eq 7 -and $baselineSummary.valid_window_counts.off -eq 7 -and
                        $baselineSummary.valid_window_counts.on -eq 0 -and $baselineSummary.valid_window_counts.total -eq 7)
        Assert-Fixture ($script:FakeCalls.Count -eq 7 -and
                        @($script:FakeCalls | Where-Object { $_ -ne "off" }).Count -eq 0)
        foreach ($run in $baselineSummary.runs) {
            Assert-Fixture ($run.mode -eq "baseline" -and $null -eq $run.candidate -and
                            $run.side -eq "off" -and $run.valid -and $run.fps_mean_flip_approx -eq 30 -and
                            $run.effective_environment.switches.KZ3_WINDOW_SIZE -eq "1920x1080" -and
                            $run.pad_clock_alignment.method -eq "shared_monotonic_timestamp")
            foreach ($name in $script:Candidates) {
                Assert-Fixture ([string]$run.effective_environment.switches.PSObject.Properties[$name].Value -eq "0")
                Assert-Fixture ([string]$baselineSummary.conditions.candidate_switches.off.PSObject.Properties[$name].Value -eq "0")
            }
        }
        $baselineCsv = @(Import-Csv -LiteralPath (Join-Path $script:CreatedSessions[1] "summary.csv"))
        Assert-Fixture ($baselineCsv.Count -eq 7 -and
                        @($baselineCsv | Where-Object { $_.mode -ne "baseline" -or $_.candidate -ne "" -or
                                                       $_.side -ne "off" }).Count -eq 0)
        $baselineText = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[1] "summary.json")) +
                        [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[1] "summary.csv"))
        Assert-Fixture (-not $baselineText.Contains("SENSITIVE_TEST_MARKER") -and
                        -not $baselineText.Contains($Scratch) -and -not $baselineText.Contains($script:Repo))
        Assert-Fixture ((Get-FileSha256 $profilePath) -eq $profileHash)

        $stage = "synthetic_baseline_bad_evidence"
        $SamplesPerSide = 5
        $script:FakeCalls.Clear()
        $script:FakeBadEvidenceAt = 3
        Assert-Reason "pad_incomplete" { Invoke-MissionBenchmark }
        $script:FakeBadEvidenceAt = 0
        Assert-Fixture ($script:CreatedSessions.Count -eq 3)
        $failed = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[2] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($failed.status -eq "failed" -and $failed.reason -eq "pad_incomplete" -and
                        $failed.mode -eq "baseline" -and $null -eq $failed.candidate -and
                        $failed.runs.Count -eq 3 -and $failed.valid_window_counts.off -eq 2 -and
                        $failed.valid_window_counts.on -eq 0 -and -not $failed.runs[2].valid)

        $stage = "synthetic_baseline_insufficient"
        $script:FakeCalls.Clear()
        $realSchedule = (Get-Item function:Get-Schedule).ScriptBlock
        try {
            Set-Item function:Get-Schedule {
                param([int]$Minimum, [switch]$BaselineMode)
                return ,([string[]]@("off", "off", "off", "off"))
            }
            Assert-Reason "insufficient_valid_windows" { Invoke-MissionBenchmark }
        } finally {
            Set-Item function:Get-Schedule $realSchedule
        }
        Assert-Fixture ($script:CreatedSessions.Count -eq 4)
        $failed = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[3] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($failed.status -eq "failed" -and $failed.reason -eq "insufficient_valid_windows" -and
                        $failed.mode -eq "baseline" -and $failed.runs.Count -eq 4 -and
                        $failed.valid_window_counts.off -eq 4 -and $failed.valid_window_counts.on -eq 0)

        $stage = "synthetic_matrix_crash"
        $Candidate = "GCM_FAST_METHODS"
        $Baseline = $false
        $SamplesPerSide = 5
        $script:FakeCalls.Clear()
        $script:FakeCrashAt = 3
        Assert-Reason "process_crashed" { Invoke-MissionBenchmark }
        Assert-Fixture ($script:CreatedSessions.Count -eq 5)
        $failed = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[4] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($failed.status -eq "failed" -and $failed.reason -eq "process_crashed" -and $failed.runs.Count -eq 3 -and
                        -not $failed.runs[2].valid -and $failed.runs[2].reason -eq "process_crashed")

        $stage = "synthetic_asset_drift"
        $script:FakeCalls.Clear()
        $script:FakeCrashAt = 0
        $script:FakeAssetDrift = $true
        Assert-Reason "app_assets_changed" { Invoke-MissionBenchmark }
        Assert-Fixture ($script:CreatedSessions.Count -eq 6)
        $failed = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[5] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($failed.status -eq "failed" -and $failed.reason -eq "app_assets_changed" -and $failed.runs.Count -eq 2 -and
                        -not $failed.runs[1].valid -and $failed.runs[1].reason -eq "app_assets_changed")

        $stage = "full_synthetic_developer_baseline"
        $script:FakeAssetDrift = $false
        $script:FakeDeveloper = $true
        $script:FakeCalls.Clear()
        $Candidate = ""
        $Baseline = $true
        $DeveloperBuildDir = $developerDir
        foreach ($name in @("runtime", "fonts")) {
            New-Item -ItemType Directory -Force -Path (Join-Path $root $name) | Out-Null
        }
        [IO.File]::WriteAllText((Join-Path $root "runtime\SDL3.dll"), "install-sdl")
        [IO.File]::WriteAllText((Join-Path $root "fonts\Rajdhani-Medium.ttf"), "medium")
        [IO.File]::WriteAllText((Join-Path $root "fonts\Rajdhani-Bold.ttf"), "bold")
        [IO.File]::WriteAllText((Join-Path $build "install-only.dll"), "install-dll")
        [IO.File]::WriteAllText((Join-Path $developerDir "developer-only.dll"), "do-not-stage")
        $portableStamp = Join-Path $build "build-stamp.json"
        [IO.File]::WriteAllText($portableStamp, '{"synthetic":"unchanged"}')
        $portableStampHash = Get-FileSha256 $portableStamp
        $fixtureRepo = Join-Path $Scratch "synthetic-tracked-repo"
        $fixtureTextDir = Join-Path $fixtureRepo "port\text"
        New-Item -ItemType Directory -Force -Path $fixtureTextDir | Out-Null
        $fixtureTranslation = Join-Path $fixtureTextDir "port_ui.tsv"
        [IO.File]::WriteAllText($fixtureTranslation, "repository-language")
        & git init --quiet $fixtureRepo 2>$null
        Assert-Fixture ($LASTEXITCODE -eq 0)
        & git -C $fixtureRepo add --force -- 'port\text\port_ui.tsv' 2>$null
        Assert-Fixture ($LASTEXITCODE -eq 0)
        Assert-Fixture (-not (Test-Path -LiteralPath (Join-Path $root "lang\port_ui.tsv")))
        Invoke-MissionBenchmark | Out-Null
        Assert-Fixture ($script:CreatedSessions.Count -eq 7)
        $developerSummary = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[6] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($developerSummary.status -eq "complete" -and $developerSummary.mode -eq "baseline" -and
                        $developerSummary.conditions.build_source -eq "developer_cmake" -and
                        $developerSummary.build.source -eq "developer_cmake" -and
                        $developerSummary.build.launcher_action -eq "rebuild" -and
                        $developerSummary.build.exe_sha256 -eq (Get-FileSha256 $developerExe) -and
                        $developerSummary.build.eboot_mode -eq "argument" -and
                        $developerSummary.build.eboot_sha256 -eq (Get-FileSha256 $fixtureEboot) -and
                        $developerSummary.build.cmake.cache_sha256 -eq ("a" * 64) -and
                        $developerSummary.build.cmake.generator -eq "Ninja" -and
                        $developerSummary.build.cmake.configure_toolkit_stamp -eq "not_recorded" -and
                        $developerSummary.build.app_resources.sdl3 -and
                        $developerSummary.build.app_resources.ui_translations -and
                        $developerSummary.build.app_resources.ui_translations_source -eq "repository_tracked" -and
                        $developerSummary.build.app_resources.ui_translations_sha256 -eq (Get-FileSha256 $fixtureTranslation) -and
                        $developerSummary.build.app_resources.notice_fonts -eq 2 -and
                        $developerSummary.runs.Count -eq 5 -and $developerSummary.valid_window_counts.off -eq 5)
        Assert-Fixture ((Get-FileSha256 $portableStamp) -eq $portableStampHash)
        Assert-Fixture (-not (Test-Path -LiteralPath (Join-Path $root "lang\port_ui.tsv")))
        $developerText = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[6] "summary.json")) +
                         [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[6] "summary.csv"))
        Assert-Fixture (-not $developerText.Contains("SENSITIVE_TEST_MARKER") -and
                        -not $developerText.Contains($Scratch) -and -not $developerText.Contains($script:Repo))

        $stage = "full_synthetic_developer_candidate"
        $script:FakeCalls.Clear()
        $Candidate = "GCM_FAST_METHODS"
        $Baseline = $false
        Invoke-MissionBenchmark | Out-Null
        Assert-Fixture ($script:CreatedSessions.Count -eq 8)
        $developerCandidate = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[7] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($developerCandidate.status -eq "complete" -and
                        $developerCandidate.mode -eq "candidate" -and
                        $developerCandidate.candidate -eq "GCM_FAST_METHODS" -and
                        $developerCandidate.build.source -eq "developer_cmake" -and
                        $developerCandidate.build.app_resources.ui_translations_source -eq "repository_tracked" -and
                        $developerCandidate.build.app_resources.ui_translations_sha256 -eq (Get-FileSha256 $fixtureTranslation) -and
                        $developerCandidate.runs.Count -eq 12 -and
                        $developerCandidate.valid_window_counts.off -eq 6 -and
                        $developerCandidate.valid_window_counts.on -eq 6 -and
                        ($script:FakeCalls -join ",") -eq ($schedule -join ",") -and
                        (Get-FileSha256 $portableStamp) -eq $portableStampHash)
        $candidateText = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[7] "summary.json")) +
                         [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[7] "summary.csv"))
        Assert-Fixture (-not $candidateText.Contains("SENSITIVE_TEST_MARKER") -and
                        -not $candidateText.Contains($Scratch) -and -not $candidateText.Contains($script:Repo))

        $stage = "synthetic_developer_translation_drift"
        $script:FakeCalls.Clear()
        $script:FakeTranslationDrift = $true
        $Candidate = ""
        $Baseline = $true
        $translationHash = Get-FileSha256 $fixtureTranslation
        Assert-Reason "app_assets_changed" { Invoke-MissionBenchmark }
        Assert-Fixture ($script:CreatedSessions.Count -eq 9)
        $failed = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[8] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($failed.status -eq "failed" -and $failed.reason -eq "app_assets_changed" -and
                        $failed.runs.Count -eq 2 -and
                        $failed.build.app_resources.ui_translations_sha256 -eq $translationHash -and
                        (Get-FileSha256 $fixtureTranslation) -ne $translationHash -and
                        (Get-FileSha256 $portableStamp) -eq $portableStampHash)

        $stage = "synthetic_developer_translation_metadata"
        $script:FakeTranslationDrift = $false
        [IO.File]::WriteAllText($fixtureTranslation, "repository-language")
        foreach ($kind in @("source", "hash")) {
            $script:FakeCalls.Clear()
            $script:FakeTranslationMetadata = $kind
            $reason = if ($kind -eq "source") { "developer_resource_provenance_invalid" }
                      else { "developer_resource_mismatch" }
            Assert-Reason $reason { Invoke-MissionBenchmark }
            $index = $script:CreatedSessions.Count - 1
            $failedText = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[$index] "summary.json"))
            $failed = $failedText | ConvertFrom-Json
            Assert-Fixture ($failed.status -eq "failed" -and $failed.reason -eq $reason -and
                            $failed.runs.Count -eq 1 -and $script:FakeCalls.Count -eq 0 -and
                            -not $failedText.Contains("SENSITIVE_TEST_MARKER") -and
                            -not $failedText.Contains($Scratch) -and -not $failedText.Contains($script:Repo))
        }

        $stage = "initialize_existing_profile"
        $script:FakeTranslationMetadata = ""
        $script:FakeCalls.Clear()
        $InitializeProfile = $true
        $ProfileSource = $sourceData
        $bootBefore = $script:FakeBootstrapCalls
        Invoke-MissionBenchmark | Out-Null
        $index = $script:CreatedSessions.Count - 1
        $existing = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[$index] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($existing.status -eq "complete" -and $existing.bootstrap.status -eq "not_needed" -and
                        $existing.bootstrap.route_sha256 -eq $bootstrapRouteFixture.sha256 -and
                        $existing.runs.Count -eq 5 -and $script:FakeBootstrapCalls -eq $bootBefore -and
                        (Get-FileSha256 $profilePath) -eq $profileHash)

        $stage = "no_implicit_profile_bootstrap"
        $InitializeProfile = $false
        $missingSource = Join-Path $root "missing-sensitive-profile"
        $ProfileSource = $missingSource
        $before = $script:CreatedSessions.Count
        $script:FakeCalls.Clear()
        Assert-Reason "profile_missing" { Invoke-MissionBenchmark }
        Assert-Fixture ($script:CreatedSessions.Count -eq $before -and
                        $script:FakeBootstrapCalls -eq $bootBefore -and -not (Test-Path -LiteralPath $missingSource))
        $emptySource = Join-Path $root "empty-sensitive-profile"
        New-Item -ItemType Directory -Path $emptySource | Out-Null
        $ProfileSource = $emptySource
        Assert-Reason "profile_count" { Invoke-MissionBenchmark }
        $index = $script:CreatedSessions.Count - 1
        $failed = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[$index] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($failed.status -eq "failed" -and $failed.bootstrap.status -eq "disabled" -and
                        $script:FakeBootstrapCalls -eq $bootBefore -and $script:FakeCalls.Count -eq 0)

        $stage = "bootstrap_default_missing_profile"
        $InitializeProfile = $true
        $ProfileSource = ""
        $savedCwd = $launch.cwd
        $launch.cwd = Join-Path $root "default-empty-data"
        New-Item -ItemType Directory -Path $launch.cwd | Out-Null
        $script:FakeBootstrapOutcome = "success"
        $script:FakeCalls.Clear()
        Invoke-MissionBenchmark | Out-Null
        $index = $script:CreatedSessions.Count - 1
        $initializedSession = $script:CreatedSessions[$index]
        $initializedText = [IO.File]::ReadAllText((Join-Path $initializedSession "summary.json")) +
                           [IO.File]::ReadAllText((Join-Path $initializedSession "summary.csv"))
        $initialized = [IO.File]::ReadAllText((Join-Path $initializedSession "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($initialized.status -eq "complete" -and $initialized.bootstrap.status -eq "complete" -and
                        $initialized.bootstrap.route -eq "first-boot" -and
                        $initialized.bootstrap.route_sha256 -eq $bootstrapRouteFixture.sha256 -and
                        $initialized.bootstrap.provenance -eq "developer_cmake" -and
                        $initialized.build.source -eq "developer_cmake" -and
                        $initialized.build.eboot_mode -eq "argument" -and
                        $initialized.build.eboot_sha256 -eq (Get-FileSha256 $fixtureEboot) -and
                        $initialized.build.app_assets_sha256.Length -eq 64 -and
                        $initialized.runs.Count -eq 5 -and $initialized.valid_window_counts.off -eq 5 -and
                        $initialized.valid_window_counts.on -eq 0 -and
                        $script:FakeBootstrapCalls -eq ($bootBefore + 1) -and
                        -not (Test-Path -LiteralPath (Join-Path $launch.cwd "gamedata")) -and
                        (Get-FileSha256 $portableStamp) -eq $portableStampHash)
        Assert-Fixture (-not $initializedText.Contains("SENSITIVE_TEST_MARKER") -and
                        -not $initializedText.Contains($Scratch) -and -not $initializedText.Contains($script:Repo))
        $seedRelative = "gamedata\dev_hdd0\home\00000001\savedata\bootstrap\PROFILE.DAT"
        $seedProfile = Join-Path (Join-Path $initializedSession "seed") $seedRelative
        $rawProfile = Join-Path (Join-Path ($initializedSession + "-bootstrap") "data") $seedRelative
        $seedBytes = [IO.File]::ReadAllBytes($seedProfile)
        $rawBytes = [IO.File]::ReadAllBytes($rawProfile)
        Assert-Fixture ($rawBytes[0x119AB] -eq 12 -and $seedBytes[0x119A8] -eq 255 -and
                        $seedBytes[0x119A9] -eq 255 -and $seedBytes[0x119AA] -eq 255 -and
                        $seedBytes[0x119AB] -eq 255)
        $seedHash = Get-FileSha256 $seedProfile
        foreach ($run in $initialized.runs) {
            $runName = "run-{0:D3}-{1}" -f $run.run, $run.side
            $runProfile = Join-Path (Join-Path (Join-Path $initializedSession $runName) "data") $seedRelative
            Assert-Fixture ((Get-FileSha256 $runProfile) -eq $seedHash)
        }
        $launch.cwd = $savedCwd

        $stage = "bootstrap_candidate_matrix"
        $ProfileSource = $emptySource
        $Candidate = "GCM_FAST_METHODS"
        $Baseline = $false
        $script:FakeCalls.Clear()
        Invoke-MissionBenchmark | Out-Null
        $index = $script:CreatedSessions.Count - 1
        $bootCandidate = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[$index] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($bootCandidate.status -eq "complete" -and $bootCandidate.bootstrap.status -eq "complete" -and
                        $bootCandidate.mode -eq "candidate" -and $bootCandidate.runs.Count -eq 12 -and
                        $bootCandidate.valid_window_counts.off -eq 6 -and $bootCandidate.valid_window_counts.on -eq 6 -and
                        ($script:FakeCalls -join ",") -eq ($schedule -join ",") -and
                        $script:FakeBootstrapCalls -eq ($bootBefore + 2))

        $stage = "bootstrap_asset_drift"
        $Candidate = ""
        $Baseline = $true
        $script:FakeBootstrapOutcome = "asset_drift"
        $script:FakeCalls.Clear()
        Assert-Reason "app_assets_changed" { Invoke-MissionBenchmark }
        $stage = "bootstrap_asset_drift_summary"
        $index = $script:CreatedSessions.Count - 1
        $failed = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[$index] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($failed.status -eq "failed" -and $failed.reason -eq "app_assets_changed" -and
                        $failed.bootstrap.status -eq "complete" -and $failed.runs.Count -eq 1 -and
                        $script:FakeCalls.Count -eq 0 -and $script:FakeBootstrapCalls -eq ($bootBefore + 3))
        [IO.File]::WriteAllText((Join-Path $build "install-only.dll"), "install-dll")

        $stage = "initialize_rejects_source_profiles"
        $Candidate = ""
        $Baseline = $true
        $invalidSource = Join-Path $root "invalid-sensitive-profile"
        $invalidSave = Join-Path $invalidSource "dev_hdd0\home\00000001\savedata\bad"
        New-Item -ItemType Directory -Force -Path $invalidSave | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $invalidSave "PROFILE.DAT"), [byte[]]@(1, 2, 3))
        $ProfileSource = $invalidSource
        $script:FakeCalls.Clear()
        Assert-Reason "profile_invalid" { Invoke-MissionBenchmark }
        $index = $script:CreatedSessions.Count - 1
        $failed = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[$index] "summary.json")) | ConvertFrom-Json
        Assert-Fixture ($failed.bootstrap.status -eq "not_needed" -and $script:FakeCalls.Count -eq 0 -and
                        $script:FakeBootstrapCalls -eq ($bootBefore + 3))
        $secondSave = Join-Path $invalidSource "dev_hdd0\home\00000001\savedata\second"
        New-Item -ItemType Directory -Force -Path $secondSave | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $secondSave "PROFILE.DAT"), $profileBytes)
        $before = $script:CreatedSessions.Count
        Assert-Reason "profile_count" { Invoke-MissionBenchmark }
        Assert-Fixture ($script:CreatedSessions.Count -eq $before -and $script:FakeCalls.Count -eq 0 -and
                        $script:FakeBootstrapCalls -eq ($bootBefore + 3))

        $stage = "bootstrap_rejection_and_privacy"
        $ProfileSource = $emptySource
        foreach ($testCase in @(@("missing", "profile_count"), @("incomplete", "profile_invalid"),
                                 @("invalid", "profile_invalid"), @("layout", "profile_layout"),
                                 @("multiple", "profile_count"),
                                 @("pad_incomplete", "pad_incomplete"), @("pad_warning", "pad_warning"),
                                 @("crash_log", "crash_log"), @("process_crashed", "process_crashed"),
                                 @("process_timeout", "process_timeout"), @("shutdown_timeout", "shutdown_timeout"))) {
            $script:FakeBootstrapOutcome = $testCase[0]
            $script:FakeCalls.Clear()
            $expectedBoots = $script:FakeBootstrapCalls + 1
            Assert-Reason $testCase[1] { Invoke-MissionBenchmark }
            $index = $script:CreatedSessions.Count - 1
            $failedText = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[$index] "summary.json")) +
                          [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[$index] "summary.csv"))
            $failed = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[$index] "summary.json")) | ConvertFrom-Json
            Assert-Fixture ($failed.status -eq "failed" -and $failed.reason -eq $testCase[1] -and
                            $failed.bootstrap.status -eq "failed" -and
                            $failed.bootstrap.route_sha256 -eq $bootstrapRouteFixture.sha256 -and
                            $failed.runs.Count -eq 0 -and $failed.valid_window_counts.total -eq 0 -and
                            $script:FakeCalls.Count -eq 0 -and $script:FakeBootstrapCalls -eq $expectedBoots -and
                            -not $failedText.Contains("SENSITIVE_TEST_MARKER") -and
                            -not $failedText.Contains($Scratch) -and -not $failedText.Contains($script:Repo))
        }

        $stage = "embedded_bootstrap_and_matrix"
        $script:FakeEbootMode = "embedded"
        $script:FakeBootstrapOutcome = "success"
        $script:FakeCalls.Clear()
        $bootBefore = $script:FakeBootstrapCalls
        $InitializeProfile = $true
        $ProfileSource = $emptySource
        Invoke-MissionBenchmark | Out-Null
        $index = $script:CreatedSessions.Count - 1
        $embeddedText = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[$index] "summary.json"))
        $embeddedSummary = $embeddedText | ConvertFrom-Json
        Assert-Fixture ($embeddedSummary.status -eq "complete" -and
                        $embeddedSummary.bootstrap.status -eq "complete" -and
                        $embeddedSummary.build.eboot_mode -eq "embedded" -and
                        $embeddedSummary.build.eboot_sha256 -eq (Get-FileSha256 $fixtureEboot) -and
                        $embeddedSummary.runs.Count -eq 5 -and $script:FakeBootstrapCalls -eq ($bootBefore + 1) -and
                        -not $embeddedText.Contains("SENSITIVE_TEST_MARKER") -and
                        -not $embeddedText.Contains($Scratch) -and -not $embeddedText.Contains($script:Repo))

        $stage = "developer_eboot_provenance_mismatches"
        $InitializeProfile = $false
        $ProfileSource = $sourceData
        foreach ($kind in @("mode", "cache", "eboot")) {
            $script:FakeProvenanceMismatch = $kind
            $before = $script:CreatedSessions.Count
            $script:FakeCalls.Clear()
            Assert-Reason "developer_provenance_changed" { Invoke-MissionBenchmark }
            Assert-Fixture ($script:CreatedSessions.Count -eq $before -and $script:FakeCalls.Count -eq 0)
        }
        $script:FakeProvenanceMismatch = ""

        $stage = "developer_eboot_drift"
        $script:FakeEbootMode = "argument"
        $script:FakeEbootDrift = $true
        $script:FakeCalls.Clear()
        $originalEboot = [IO.File]::ReadAllBytes($fixtureEboot)
        $originalEbootHash = Get-FileSha256 $fixtureEboot
        try {
            Assert-Reason "developer_eboot_changed" { Invoke-MissionBenchmark }
        } finally {
            [IO.File]::WriteAllBytes($fixtureEboot, $originalEboot)
            $script:FakeEbootDrift = $false
        }
        $index = $script:CreatedSessions.Count - 1
        $driftText = [IO.File]::ReadAllText((Join-Path $script:CreatedSessions[$index] "summary.json"))
        $drift = $driftText | ConvertFrom-Json
        Assert-Fixture ($drift.status -eq "failed" -and $drift.reason -eq "developer_eboot_changed" -and
                        $drift.build.eboot_mode -eq "argument" -and $drift.build.eboot_sha256 -eq $originalEbootHash -and
                        $drift.runs.Count -eq 2 -and $drift.runs[0].valid -and -not $drift.runs[1].valid -and
                        $script:FakeCalls.Count -eq 1 -and -not $driftText.Contains("SENSITIVE_TEST_MARKER") -and
                        -not $driftText.Contains($Scratch) -and -not $driftText.Contains($script:Repo))
    } finally {
        foreach ($session in $script:CreatedSessions) {
            $bootSession = $session + "-bootstrap"
            if (Test-Path -LiteralPath $bootSession) { Remove-Item -LiteralPath $bootSession -Recurse -Force }
            Remove-Item -LiteralPath $session -Recurse -Force
        }
    }

    Write-Output "test_mission0_ps: PASS"
} catch {
    [Console]::Error.WriteLine("test_mission0_ps: FAIL ($stage)")
    [Console]::Error.WriteLine(("diagnostic: reason={0} type={1} line={2}" -f
        (Get-BenchmarkReason $_), $_.Exception.GetType().Name, $_.InvocationInfo.ScriptLineNumber))
    if ($_.Exception -is [Management.Automation.CommandNotFoundException] -and
        $_.Exception.CommandName -match '^[A-Za-z][A-Za-z0-9-]*$') {
        [Console]::Error.WriteLine("diagnostic command: " + $_.Exception.CommandName)
    }
    exit 1
}
