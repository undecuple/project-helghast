# Parse before PowerShell's parameter binder can echo private paths in syntax errors.
$Candidate = ""
$Baseline = $false
$SamplesPerSide = "5"
$InstallRoot = ""
$DeveloperBuildDir = ""
$ProfileSource = ""
$InitializeProfile = $false
$Python = ""
$Plan = $false
$ConfirmExclusiveGpu = $false
$script:DeveloperBuildRequested = $false
$optionNames = @{
    candidate = "Candidate"; baseline = "Baseline"; samplesperside = "SamplesPerSide"
    installroot = "InstallRoot"; developerbuilddir = "DeveloperBuildDir"; profilesource = "ProfileSource"
    initializeprofile = "InitializeProfile"
    python = "Python"; plan = "Plan"; confirmexclusivegpu = "ConfirmExclusiveGpu"
}
$switchNames = @("baseline", "initializeprofile", "plan", "confirmexclusivegpu")
$seenOptions = @{}
$invalidArguments = $false
for ($argumentIndex = 0; $argumentIndex -lt $args.Count; $argumentIndex++) {
    $argument = $args[$argumentIndex]
    if ($argument -isnot [string]) { $invalidArguments = $true; break }
    $match = [regex]::Match($argument, '^-(?<name>[A-Za-z][A-Za-z0-9]*)(?<colon>:)?(?<value>.*)$')
    if (-not $match.Success -or (-not $match.Groups["colon"].Success -and $match.Groups["value"].Value)) {
        $invalidArguments = $true; break
    }
    $prefix = $match.Groups["name"].Value.ToLowerInvariant()
    $matchingNames = @($optionNames.Keys | Where-Object { $_.StartsWith($prefix) })
    if ($matchingNames.Count -ne 1 -or $seenOptions.ContainsKey($matchingNames[0])) {
        $invalidArguments = $true; break
    }
    $name = $matchingNames[0]
    $seenOptions[$name] = $true
    $hasColon = $match.Groups["colon"].Success
    $value = $match.Groups["value"].Value
    if ($name -in $switchNames) {
        $nextIsBoolean = $false
        if ($argumentIndex + 1 -lt $args.Count) {
            $next = $args[$argumentIndex + 1]
            $nextIsBoolean = $next -is [bool] -or
                ($next -is [string] -and $next -match '^(\$?true|\$?false)$')
        }
        if ($hasColon -or $nextIsBoolean) {
            if (-not $value -and $argumentIndex + 1 -lt $args.Count) { $value = $args[++$argumentIndex] }
            if ($value -is [bool]) { $value = [bool]$value }
            elseif ([string]$value -match '^(\$?true|\$?false)$') {
                $value = ([string]$value).TrimStart('$') -ieq "true"
            } else { $invalidArguments = $true; break }
        } else { $value = $true }
    } else {
        if (-not $hasColon -or -not $value) {
            if ($argumentIndex + 1 -ge $args.Count) { $invalidArguments = $true; break }
            $value = $args[++$argumentIndex]
            if ($value -is [string] -and $value -match '^-(candidate|baseline|samplesperside|installroot|developerbuilddir|profilesource|initializeprofile|python|plan|confirmexclusivegpu)(:.*)?$') {
                $invalidArguments = $true; break
            }
        }
        $value = [string]$value
    }
    Set-Variable -Name $optionNames[$name] -Value $value
    if ($name -eq "developerbuilddir") { $script:DeveloperBuildRequested = $true }
}
if ($invalidArguments) {
    [Console]::Error.WriteLine("benchmark failed: arguments_invalid")
    if ($MyInvocation.InvocationName -ne ".") { exit 1 }
    return
}

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$script:Repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$script:Helper = Join-Path $PSScriptRoot "mission0_helper.py"
$script:Launcher = Join-Path $script:Repo "tools\release\launch.py"
$script:Candidates = @("GCM_FAST_METHODS", "LD_CB_REUSE", "KZ3_SPU_TASK_POOL", "KZ3_PARK_IDLE_WAKE")
$script:WindowsEnvironment = @(
    "SystemRoot", "WINDIR", "SystemDrive", "ComSpec", "PATH", "PATHEXT", "TEMP", "TMP",
    "USERPROFILE", "HOMEDRIVE", "HOMEPATH", "HOME", "APPDATA", "LOCALAPPDATA", "ALLUSERSPROFILE",
    "ProgramData", "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "CommonProgramFiles",
    "CommonProgramFiles(x86)", "CommonProgramW6432", "PUBLIC", "OS", "NUMBER_OF_PROCESSORS",
    "PROCESSOR_ARCHITECTURE", "PROCESSOR_ARCHITEW6432", "PROCESSOR_IDENTIFIER",
    "PROCESSOR_LEVEL", "PROCESSOR_REVISION"
)
$script:PathEnvironment = @(
    "PS3_VFS_ROOT", "KZ3_CACHE_ROOT", "PS3_EDAT_CACHE", "RSX_SHADER_CACHE_DIR", "KZ3_INSTALL_DIR",
    "KZ3_UPDATE_DIR", "PS3_HDD0_ROOT", "PS3_HDD1_ROOT", "KZ3_KEYBINDS"
)
$script:LiteralSwitches = @(
    "KZ3_WINDOW_SIZE", "KZ3_DISPLAY", "KZ3_DISPLAY_SCALING", "KZ3_DISPLAY_FILTER", "KZ3_FPS_LIMIT",
    "KZ3_BLOOM", "KZ3_UNFOCUSED", "KZ3_MOUSE_LOOK", "KZ3_DISCORD_PRESENCE", "PAD_NO_KEYBOARD",
    "PAD_CLOCK", "GCM_FLIPCOUNT", "RSX_PACE", "PS3_FSLOG", "KZ3_LOG_STDOUT", "KZ3_NO_ERROR_BOX",
    "KZ3_ERROR_NOTICE_TIMEOUT", "PS3_LANG", "KZ3_LANG", "LD_PSO_TEMPLATE"
) + $script:Candidates

function Stop-Benchmark([string]$Code) {
    if ($Code -notmatch '^[a-z][a-z0-9_]{0,60}$') { $Code = "internal_error" }
    $exception = New-Object System.InvalidOperationException($Code)
    $exception.Data["BenchmarkReason"] = $Code
    throw $exception
}

function Get-BenchmarkReason($Record) {
    $exception = $Record.Exception
    while ($null -ne $exception) {
        if ($exception.Data.Contains("BenchmarkReason")) {
            $code = [string]$exception.Data["BenchmarkReason"]
            if ($code -match '^[a-z][a-z0-9_]{0,60}$') { return $code }
        }
        $exception = $exception.InnerException
    }
    return "internal_error"
}

function ConvertTo-WindowsArgument([string]$Value) {
    # CreateProcess/CommandLineToArgvW quoting, including trailing backslashes.
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $slashes++
        } elseif ($character -eq '"') {
            [void]$builder.Append(('\' * (2 * $slashes + 1)))
            [void]$builder.Append('"')
            $slashes = 0
        } else {
            if ($slashes) { [void]$builder.Append(('\' * $slashes)) }
            [void]$builder.Append($character)
            $slashes = 0
        }
    }
    if ($slashes) { [void]$builder.Append(('\' * (2 * $slashes))) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Join-WindowsArguments([string[]]$Arguments) {
    if ($null -eq $Arguments -or $Arguments.Count -eq 0) { return "" }
    return (($Arguments | ForEach-Object { ConvertTo-WindowsArgument $_ }) -join ' ')
}

function Get-Python([string]$Requested) {
    if ($Requested) {
        $command = Get-Command $Requested -CommandType Application -ErrorAction SilentlyContinue
    } else {
        $command = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if (-not $command -or -not (Test-Path -LiteralPath $command.Source -PathType Leaf)) {
        Stop-Benchmark "python_missing"
    }
    return $command.Source
}

function Get-FileSha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace("-", "").ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Set-PlatformEnvironment([System.Diagnostics.ProcessStartInfo]$Info) {
    $values = @{}
    foreach ($name in $script:WindowsEnvironment) {
        $value = [Environment]::GetEnvironmentVariable($name, "Process")
        if ($null -ne $value) { $values[$name] = $value }
    }
    $Info.EnvironmentVariables.Clear()
    foreach ($name in $values.Keys) { $Info.EnvironmentVariables[$name] = $values[$name] }
}

function Invoke-JsonTool([string]$Executable, [string[]]$Arguments, [int]$TimeoutMilliseconds = 30000) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Executable
    $info.Arguments = Join-WindowsArguments (@("-I") + $Arguments)
    $info.WorkingDirectory = $script:Repo
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    Set-PlatformEnvironment $info
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { Stop-Benchmark "tool_start_failed" }
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            $process.Kill()
            [void]$process.WaitForExit(5000)
            Stop-Benchmark "tool_timeout"
        }
        $text = $output.GetAwaiter().GetResult()
        [void]$errors.GetAwaiter().GetResult()
        try { $result = $text | ConvertFrom-Json -ErrorAction Stop }
        catch { Stop-Benchmark "tool_output_invalid" }
        if ($process.ExitCode -ne 0) {
            $reason = $result.PSObject.Properties["reason"]
            if ($null -ne $reason) { Stop-Benchmark ([string]$reason.Value) }
            Stop-Benchmark "tool_failed"
        }
        return $result
    } finally {
        $process.Dispose()
    }
}

function Require-Property($Object, [string]$Name) {
    if ($null -eq $Object -or $null -eq $Object.PSObject.Properties[$Name]) {
        Stop-Benchmark "launcher_output_invalid"
    }
    return $Object.PSObject.Properties[$Name].Value
}

function Test-AtOrBelow([string]$Path, [string]$Parent) {
    $a = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $b = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    return $a.Equals($b, [StringComparison]::OrdinalIgnoreCase) -or
        $a.StartsWith($b + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoReparseAncestors([string]$Path, [string]$Missing = "launcher_root_missing",
                                   [string]$Link = "launcher_root_link") {
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($full)
    $current = $root
    foreach ($part in $full.Substring($root.Length).Split('\', [StringSplitOptions]::RemoveEmptyEntries)) {
        $current = Join-Path $current $part
        try { $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop }
        catch { Stop-Benchmark $Missing }
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            Stop-Benchmark $Link
        }
    }
}

function Assert-DeveloperEboot($Launch) {
    $mode = [string](Require-Property $Launch "eboot_mode")
    $hash = [string](Require-Property $Launch "eboot_sha256")
    $argv = @(Require-Property $Launch "argv")
    if ($mode -notin @("argument", "embedded") -or $hash -cnotmatch '^[0-9a-f]{64}$') {
        Stop-Benchmark "developer_eboot_provenance_invalid"
    }
    if ($mode -eq "embedded") {
        if ($argv.Count -ne 1) { Stop-Benchmark "developer_eboot_provenance_invalid" }
        return
    }
    if ($argv.Count -ne 2) { Stop-Benchmark "developer_eboot_provenance_invalid" }
    $argument = [string]$argv[1]
    $iniEboot = [string](Require-Property (Require-Property $Launch "ini") "eboot")
    if (-not $argument -or -not $iniEboot -or $argument -match '[\x00\r\n]' -or
        $iniEboot -match '[\x00\r\n]' -or -not [IO.Path]::IsPathRooted($argument) -or
        -not [IO.Path]::IsPathRooted($iniEboot) -or
        -not (Test-AtOrBelow $argument $iniEboot) -or -not (Test-AtOrBelow $iniEboot $argument) -or
        -not (Test-Path -LiteralPath $argument -PathType Leaf)) {
        Stop-Benchmark "developer_recorded_paths_invalid"
    }
    if (@($argument -split '[\\/]') -contains "vfs") { Stop-Benchmark "link_free_launcher_required" }
    Assert-NoReparseAncestors $argument "developer_recorded_paths_invalid" "developer_path_link"
    if ((Get-FileSha256 $argument) -cne $hash) { Stop-Benchmark "developer_eboot_changed" }
}

function Get-LaunchPlan([string]$PythonExe, [string]$LauncherScript, [string]$Root, [string]$Developer = "") {
    if (-not (Test-Path -LiteralPath $LauncherScript -PathType Leaf)) { Stop-Benchmark "launcher_missing" }
    $launcherArgs = @("-B", $LauncherScript, "launcher", "--install", $Root)
    if ($Developer) { $launcherArgs += @("--developer-build", $Developer, "--repo", $script:Repo) }
    $launch = Invoke-JsonTool $PythonExe $launcherArgs
    $action = [string](Require-Property $launch "action")
    if ((-not $Developer -and $action -ne "run") -or
        ($Developer -and $action -notin @("run", "rebuild"))) { Stop-Benchmark "launcher_not_runnable" }
    $argv = @(Require-Property $launch "argv")
    $cwd = [string](Require-Property $launch "cwd")
    $envMap = Require-Property $launch "env"
    [void](Require-Property $launch "ini")
    [void](Require-Property $launch "template_env")
    if ($argv.Count -lt 1 -or -not [IO.Path]::IsPathRooted([string]$argv[0]) -or
        [IO.Path]::GetExtension([string]$argv[0]) -ine ".exe" -or
        -not (Test-Path -LiteralPath ([string]$argv[0]) -PathType Leaf) -or
        -not [IO.Path]::IsPathRooted($cwd)) {
        Stop-Benchmark "launcher_output_invalid"
    }
    if ($Developer) {
        $expected = Join-Path $Developer "kz3.exe"
        if (-not (Test-AtOrBelow ([string]$argv[0]) $expected) -or
            -not (Test-AtOrBelow $expected ([string]$argv[0]))) { Stop-Benchmark "launcher_output_invalid" }
        Assert-NoReparseAncestors ([string]$argv[0])
        Assert-DeveloperEboot $launch
    } elseif (-not (Test-AtOrBelow ([string]$argv[0]) $Root)) {
        Stop-Benchmark "launcher_output_invalid"
    }
    $executable = [IO.File]::OpenRead([string]$argv[0])
    try {
        if ($executable.ReadByte() -ne 0x4d -or $executable.ReadByte() -ne 0x5a) {
            Stop-Benchmark "executable_invalid"
        }
    } finally { $executable.Dispose() }
    foreach ($arg in $argv) {
        if ($null -eq $arg -or [string]$arg -match '[\x00\r\n]') { Stop-Benchmark "launcher_output_invalid" }
    }
    foreach ($name in @("PS3_VFS_ROOT", "KZ3_CACHE_ROOT", "PS3_EDAT_CACHE", "RSX_SHADER_CACHE_DIR", "KZ3_INSTALL_DIR")) {
        $value = [string](Require-Property $envMap $name)
        if (-not $value -or -not [IO.Path]::IsPathRooted($value) -or $value -match '[\x00\r\n]') {
            Stop-Benchmark "launcher_output_invalid"
        }
    }
    $gameRoot = [string](Require-Property $envMap "PS3_VFS_ROOT")
    $cacheRoot = [string](Require-Property $envMap "KZ3_CACHE_ROOT")
    $install = [string](Require-Property $envMap "KZ3_INSTALL_DIR")
    if (-not (Test-AtOrBelow $install $Root) -or -not (Test-AtOrBelow $Root $install)) {
        Stop-Benchmark "launcher_output_invalid"
    }
    if ((Test-AtOrBelow $gameRoot $cacheRoot) -or (Test-AtOrBelow $gameRoot (Join-Path $Root "vfs")) -or
        (Test-AtOrBelow $gameRoot (Join-Path $script:Repo "vfs"))) {
        Stop-Benchmark "link_free_launcher_required"
    }
    return $launch
}

function Get-Schedule([int]$Minimum, [switch]$BaselineMode) {
    $list = New-Object System.Collections.Generic.List[string]
    if ($BaselineMode) {
        for ($i = 0; $i -lt $Minimum; $i++) { $list.Add("off") }
    } else {
        $blocks = [int][Math]::Ceiling($Minimum / 2.0)
        for ($i = 0; $i -lt $blocks; $i++) {
            foreach ($side in @("off", "on", "on", "off")) { $list.Add($side) }
        }
    }
    return ,$list.ToArray()
}

function Get-CandidateSwitches([string]$Mode, [string]$Selected, [string]$Side) {
    if ($Mode -notin @("baseline", "candidate") -or $Side -notin @("off", "on") -or
        ($Mode -eq "baseline" -and $Side -ne "off") -or
        ($Mode -eq "candidate" -and $Selected -notin $script:Candidates)) {
        Stop-Benchmark "schedule_invalid"
    }
    $switches = [ordered]@{}
    foreach ($name in $script:Candidates) {
        $switches[$name] = if ($Mode -eq "candidate" -and $Side -eq "on" -and $name -eq $Selected) { "1" }
                           else { "0" }
    }
    return $switches
}

function Get-ValidWindowCounts([object[]]$Runs) {
    $off = @($Runs | Where-Object { $_.side -eq "off" -and $_.valid }).Count
    $on = @($Runs | Where-Object { $_.side -eq "on" -and $_.valid }).Count
    return [ordered]@{ off = $off; on = $on; total = ($off + $on) }
}

function Assert-ValidWindowCounts([object[]]$Runs, [int]$Minimum, [string]$Mode) {
    $counts = Get-ValidWindowCounts $Runs
    if ($Mode -eq "baseline") {
        if ($counts.off -lt $Minimum) { Stop-Benchmark "insufficient_valid_windows" }
        if ($counts.off -ne $Minimum -or $counts.on -ne 0 -or $Runs.Count -ne $Minimum) {
            Stop-Benchmark "baseline_window_count_invalid"
        }
    } elseif ($Mode -eq "candidate") {
        if ($counts.off -lt $Minimum -or $counts.on -lt $Minimum) {
            Stop-Benchmark "insufficient_valid_windows"
        }
    } else {
        Stop-Benchmark "schedule_invalid"
    }
}

function Convert-GpuSamples($Samples, [int]$AllowedPid) {
    $seen = 0
    $foreignLoads = @{}
    foreach ($sample in @($Samples)) {
        if ($null -eq $sample) {
            return @{ status = "unavailable"; foreign_activity = "unknown"; max_foreign_percent = $null }
        }
        $nameProperty = $sample.PSObject.Properties["Name"]
        $valueProperty = $sample.PSObject.Properties["UtilizationPercentage"]
        if ($null -eq $nameProperty -or $null -eq $valueProperty -or $null -eq $valueProperty.Value) {
            return @{ status = "unavailable"; foreign_activity = "unknown"; max_foreign_percent = $null }
        }
        $name = [string]$nameProperty.Value
        if ($name -ieq "_total") { continue }
        if ($name -notmatch '^pid_(\d+)_') {
            return @{ status = "unavailable"; foreign_activity = "unknown"; max_foreign_percent = $null }
        }
        try { $processId = [int]$Matches[1]; $load = [double]$valueProperty.Value }
        catch { return @{ status = "unavailable"; foreign_activity = "unknown"; max_foreign_percent = $null } }
        if ([double]::IsNaN($load) -or [double]::IsInfinity($load) -or $load -lt 0 -or $load -gt 100) {
            return @{ status = "unavailable"; foreign_activity = "unknown"; max_foreign_percent = $null }
        }
        $seen++
        if ($processId -ne $AllowedPid) {
            if (-not $foreignLoads.ContainsKey($processId)) { $foreignLoads[$processId] = 0.0 }
            $foreignLoads[$processId] += $load
        }
    }
    if ($seen -eq 0) {
        return @{ status = "unavailable"; foreign_activity = "unknown"; max_foreign_percent = $null }
    }
    $maximum = 0.0
    foreach ($load in $foreignLoads.Values) { $maximum = [Math]::Max($maximum, $load) }
    return @{ status = "sampled"; foreign_activity = $(if ($maximum -gt 1.0) { "observed" } else { "not_observed" })
              max_foreign_percent = [Math]::Round($maximum, 2) }
}

function Get-GpuEvidence([int]$AllowedPid) {
    try {
        $samples = @(Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine `
            -OperationTimeoutSec 2 -ErrorAction Stop)
        return Convert-GpuSamples $samples $AllowedPid
    } catch {
        return @{ status = "unavailable"; foreign_activity = "unknown"; max_foreign_percent = $null }
    }
}

function New-ChildInfo([string]$Exe, [string[]]$Arguments, [string]$Cwd, $Launch, $Overrides) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Exe
    $info.Arguments = Join-WindowsArguments $Arguments
    $info.WorkingDirectory = $Cwd
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    Set-PlatformEnvironment $info
    foreach ($property in $Launch.env.PSObject.Properties) {
        $value = [string]$property.Value
        if ($property.Name -notin $script:PathEnvironment -or -not $value -or
            -not [IO.Path]::IsPathRooted($value) -or $value -match '[\x00\r\n]') {
            Stop-Benchmark "launcher_output_invalid"
        }
        $info.EnvironmentVariables[$property.Name] = [string]$property.Value
    }
    foreach ($property in $Launch.template_env.PSObject.Properties) {
        if ($property.Name -ne "LD_PSO_TEMPLATE" -or [string]$property.Value -match '[\x00\r\n]') {
            Stop-Benchmark "launcher_output_invalid"
        }
        $info.EnvironmentVariables[$property.Name] = [string]$property.Value
    }
    foreach ($name in $Overrides.Keys) {
        if ($name -notin ($script:PathEnvironment + $script:LiteralSwitches + @("PAD_SCRIPT"))) {
            Stop-Benchmark "override_unsupported"
        }
        if ($null -ne $Overrides[$name] -and
            (($name -eq "PAD_SCRIPT" -and [string]$Overrides[$name] -match '\x00') -or
             ($name -ne "PAD_SCRIPT" -and [string]$Overrides[$name] -match '[\x00\r\n]'))) {
            Stop-Benchmark "override_invalid"
        }
        if ($null -eq $Overrides[$name]) { $info.EnvironmentVariables.Remove($name) }
        else { $info.EnvironmentVariables[$name] = [string]$Overrides[$name] }
    }
    return $info
}

function Get-EffectiveSwitches([System.Diagnostics.ProcessStartInfo]$Info, [string]$RouteHash) {
    $switches = [ordered]@{}
    foreach ($name in $script:LiteralSwitches) {
        if (-not $Info.EnvironmentVariables.ContainsKey($name)) { Stop-Benchmark "switch_missing" }
        $switches[$name] = [string]$Info.EnvironmentVariables[$name]
    }
    $scriptText = [string]$Info.EnvironmentVariables["PAD_SCRIPT"]
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($scriptText))).
            Replace("-", "").ToLowerInvariant()
    } finally { $sha.Dispose() }
    if ($hash -ne $RouteHash) { Stop-Benchmark "route_changed" }
    $paths = [ordered]@{}
    foreach ($name in $script:PathEnvironment) {
        if ($Info.EnvironmentVariables.ContainsKey($name)) { $paths[$name] = "<redacted>" }
    }
    return @{ environment_policy = "windows_allowlist_v1"; switches = $switches
              pad_script_sha256 = $hash; path_variables = $paths }
}

function Invoke-ControlledProcess(
    [string]$PythonExe, [System.Diagnostics.ProcessStartInfo]$Info, [string]$Log,
    [double]$DurationSeconds, [double]$GraceSeconds, [string]$CloseMode = "window"
) {
    $supervisorInfo = New-Object System.Diagnostics.ProcessStartInfo
    $supervisorInfo.FileName = $PythonExe
    $supervisorInfo.Arguments = Join-WindowsArguments @("-I", "-B", $script:Helper, "supervise")
    $supervisorInfo.WorkingDirectory = $script:Repo
    $supervisorInfo.UseShellExecute = $false
    $supervisorInfo.CreateNoWindow = $true
    $supervisorInfo.RedirectStandardInput = $true
    $supervisorInfo.RedirectStandardOutput = $true
    $supervisorInfo.RedirectStandardError = $true
    Set-PlatformEnvironment $supervisorInfo
    $environment = @{}
    foreach ($name in $Info.EnvironmentVariables.Keys) { $environment[$name] = $Info.EnvironmentVariables[$name] }
    $spec = @{
        executable = $Info.FileName; command_line = (ConvertTo-WindowsArgument $Info.FileName) + " " + $Info.Arguments
        cwd = $Info.WorkingDirectory; environment = $environment; log = $Log
        duration_seconds = $DurationSeconds; grace_seconds = $GraceSeconds; close_mode = $CloseMode
    }
    $json = ConvertTo-Json -InputObject $spec -Depth 8 -Compress
    $json = [regex]::Replace($json, '[^\x00-\x7f]', [Text.RegularExpressions.MatchEvaluator]{
        param($match) return ('\u{0:x4}' -f [int][char]$match.Value[0])
    })
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $supervisorInfo
    $tracked = $null
    $started = $false
    $failure = $null
    $childPid = 0
    try {
        try {
            if (-not $process.Start()) { Stop-Benchmark "process_start_failed" }
        } catch { Stop-Benchmark "process_start_failed" }
        $started = $true
        $errors = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.WriteLine($json)
        $process.StandardInput.Flush()
        $startLine = $process.StandardOutput.ReadLineAsync()
        if (-not $startLine.Wait(30000)) { Stop-Benchmark "process_start_timeout" }
        try { $start = $startLine.GetAwaiter().GetResult() | ConvertFrom-Json -ErrorAction Stop }
        catch { Stop-Benchmark "process_supervisor_failed" }
        if ($null -eq $start -or $null -eq $start.PSObject.Properties["event"]) {
            Stop-Benchmark "process_supervisor_failed"
        }
        if ($start.event -eq "finished") {
            $reason = $start.PSObject.Properties["reason"]
            if ($null -ne $reason) { Stop-Benchmark ([string]$reason.Value) }
            Stop-Benchmark "process_supervisor_failed"
        }
        if ($start.event -ne "started" -or $null -eq $start.PSObject.Properties["pid"] -or [int]$start.pid -le 0 -or
            $null -eq $start.PSObject.Properties["created_filetime"] -or
            [string]$start.created_filetime -notmatch '^\d{1,20}$') {
            Stop-Benchmark "process_supervisor_failed"
        }
        $childPid = [int]$start.pid
        $createdFileTime = $null
        try {
            $tracked = [Diagnostics.Process]::GetProcessById($childPid)
            [void]$tracked.Handle
            $createdFileTime = $tracked.StartTime.ToUniversalTime().ToFileTimeUtc().ToString()
        } catch {
            if ($null -ne $tracked) { $tracked.Dispose(); $tracked = $null }
            if (-not $process.WaitForExit(2000)) { Stop-Benchmark "process_tracking_failed" }
        }
        if ($null -ne $tracked -and $createdFileTime -ne [string]$start.created_filetime) {
            $tracked.Dispose()
            $tracked = $null
            Stop-Benchmark "process_tracking_mismatch"
        }
        $tail = $process.StandardOutput.ReadToEndAsync()
        $timeout = [int][Math]::Ceiling(($DurationSeconds + $GraceSeconds + 30) * 1000)
        if (-not $process.WaitForExit($timeout)) { Stop-Benchmark "process_timeout" }
        try { $result = $tail.GetAwaiter().GetResult() | ConvertFrom-Json -ErrorAction Stop }
        catch { Stop-Benchmark "process_supervisor_failed" }
        [void]$errors.GetAwaiter().GetResult()
        if ($null -eq $result -or $null -eq $result.PSObject.Properties["event"] -or
            $result.event -ne "finished" -or $null -eq $result.PSObject.Properties["ok"]) {
            Stop-Benchmark "process_supervisor_failed"
        }
        if (-not $result.ok) {
            $reason = $result.PSObject.Properties["reason"]
            if ($null -ne $reason) { Stop-Benchmark ([string]$reason.Value) }
            Stop-Benchmark "process_supervisor_failed"
        }
        if ($process.ExitCode -ne 0) { Stop-Benchmark "process_supervisor_failed" }
    } catch {
        $failure = $_
    } finally {
        if ($started) {
            try {
                $process.StandardInput.Close()
                if (-not $process.HasExited -and -not $process.WaitForExit(5000)) {
                    if ($null -ne $tracked -and -not $tracked.HasExited) {
                        $tracked.Kill()
                        if (-not $tracked.WaitForExit(5000)) { Stop-Benchmark "process_cleanup_failed" }
                    }
                    $process.Kill()
                    if (-not $process.WaitForExit(5000)) { Stop-Benchmark "process_cleanup_failed" }
                }
                if ($null -ne $tracked -and -not $tracked.HasExited) {
                    $tracked.Kill()
                    if (-not $tracked.WaitForExit(5000)) { Stop-Benchmark "process_cleanup_failed" }
                }
            } catch {
                $failure = $_
            }
        }
        if ($null -ne $tracked) { $tracked.Dispose() }
        $process.Dispose()
    }
    if ($null -ne $failure) { throw $failure }
    return @{ exit_code = 0; pid = $childPid }
}

function ConvertTo-IniValue([string]$Value) {
    if ($Value -match '[\x00\r\n"]') { Stop-Benchmark "ini_value_invalid" }
    return '"' + $Value + '"'
}

function Write-StagedIni([string]$Path, [string]$Root, [string]$Data, $Ini) {
    $lines = @("[game]", "root = $(ConvertTo-IniValue $Root)", "data_dir = $(ConvertTo-IniValue $Data)")
    foreach ($name in @("eboot", "font_dir")) {
        $property = $Ini.PSObject.Properties[$name]
        if ($null -ne $property -and [string]$property.Value) {
            $lines += "$name = $(ConvertTo-IniValue ([string]$property.Value))"
        }
    }
    $text = ($lines -join "`r`n") + "`r`n"
    $bytes = [Text.Encoding]::Default.GetBytes($text)
    if ([Text.Encoding]::Default.GetString($bytes) -cne $text) { Stop-Benchmark "ini_encoding_invalid" }
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function Copy-AppAssets([string]$Exe, [string]$App, [string]$ResourceSource = "", [switch]$DllOnly) {
    $source = if ($ResourceSource) { $ResourceSource } else { Split-Path -Parent $Exe }
    New-Item -ItemType Directory -Path $App | Out-Null
    Copy-Item -LiteralPath $Exe -Destination (Join-Path $App ([IO.Path]::GetFileName($Exe)))
    foreach ($dll in @(Get-ChildItem -LiteralPath $source -Filter "*.dll" -File)) {
        if ($dll.Attributes -band [IO.FileAttributes]::ReparsePoint) { Stop-Benchmark "app_asset_link" }
        Copy-Item -LiteralPath $dll.FullName -Destination $App
    }
    if (-not $DllOnly) {
        foreach ($name in @("fonts", "lang")) {
            $directory = Join-Path $source $name
            if (Test-Path -LiteralPath $directory -PathType Container) {
                $items = @(Get-Item -LiteralPath $directory) + @(Get-ChildItem -LiteralPath $directory -Recurse -Force)
                foreach ($item in $items) {
                    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { Stop-Benchmark "app_asset_link" }
                }
                Copy-Item -LiteralPath $directory -Destination $App -Recurse
            }
        }
    }
}

function Get-AppAssetsSha256([string]$App) {
    $directory = [IO.Path]::GetFullPath($App).TrimEnd('\')
    $manifest = New-Object Text.StringBuilder
    foreach ($file in @(Get-ChildItem -LiteralPath $directory -File -Recurse | Sort-Object FullName)) {
        [void]$manifest.Append($file.FullName.Substring($directory.Length).ToLowerInvariant())
        [void]$manifest.Append("`0")
        [void]$manifest.Append((Get-FileSha256 $file.FullName))
        [void]$manifest.Append("`n")
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($manifest.ToString())
        return [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace("-", "").ToLowerInvariant()
    } finally { $sha.Dispose() }
}

function New-BenchmarkProcess(
    [string]$Directory, [string]$Seed, [string]$Root, [string]$Exe, [string]$ExeHash, $Launch,
    $Route, $Conditions, $DisplaySettings, $SwitchValues, [bool]$DeveloperMode, [string]$PythonExe, $Summary
) {
    if ($DeveloperMode) { Assert-DeveloperEboot $Launch }
    $app = Join-Path $Directory "app"
    $data = Join-Path $Directory "data"
    $cache = Join-Path $Directory "cache"
    New-Item -ItemType Directory -Path $Directory, $data, $cache | Out-Null
    if ($Seed) {
        Copy-Item -LiteralPath (Join-Path $Seed "gamedata") -Destination $data -Recurse
    } else {
        New-Item -ItemType Directory -Force -Path (Join-Path $data "gamedata\dev_hdd0") | Out-Null
    }
    if ($DeveloperMode) {
        $resourceSource = Join-Path $Root "build"
        Assert-NoReparseAncestors $resourceSource
        Copy-AppAssets $Exe $app $resourceSource -DllOnly
    } else {
        Copy-AppAssets $Exe $app
    }
    $resourceArgs = @("-B", $script:Helper, "stage-resources", "--install", $Root, "--app", $app)
    if ($DeveloperMode) { $resourceArgs += @("--required", "--repo", $script:Repo) }
    $resourceResult = Invoke-JsonTool $PythonExe $resourceArgs
    if ($DeveloperMode) {
        $translationSource = [string](Require-Property $resourceResult "ui_translations_source")
        $translationHash = [string](Require-Property $resourceResult "ui_translations_sha256")
        if ($translationSource -ne "repository_tracked" -or $translationHash -cnotmatch '^[0-9a-f]{64}$') {
            Stop-Benchmark "developer_resource_provenance_invalid"
        }
        if ((Get-FileSha256 (Join-Path $app "lang\port_ui.tsv")) -cne $translationHash) {
            Stop-Benchmark "developer_resource_mismatch"
        }
    }
    $stagedExe = Join-Path $app ([IO.Path]::GetFileName($Exe))
    if ((Get-FileSha256 $stagedExe) -ne $ExeHash) { Stop-Benchmark "executable_changed" }
    $assetsHash = Get-AppAssetsSha256 $app
    if ($null -eq $Summary.build.app_assets_sha256) {
        $Summary.build.app_assets_sha256 = $assetsHash
        $fontDirectory = Join-Path $app "fonts"
        $fontCount = if (Test-Path -LiteralPath $fontDirectory -PathType Container) {
            @(Get-ChildItem -LiteralPath $fontDirectory -Filter "Rajdhani-*.ttf" -File).Count
        } else { 0 }
        $fontSetting = $Launch.ini.PSObject.Properties["font_dir"]
        $Summary.build.app_resources = @{
            sdl3 = [bool](Test-Path -LiteralPath (Join-Path $app "SDL3.dll") -PathType Leaf)
            ui_translations = [bool](Test-Path -LiteralPath (Join-Path $app "lang\port_ui.tsv") -PathType Leaf)
            notice_fonts = $fontCount
            firmware_fonts = if ($null -ne $fontSetting -and [string]$fontSetting.Value) {
                "launcher_configured"
            } else { "runtime_default" }
        }
        if ($DeveloperMode) {
            $Summary.build.app_resources["ui_translations_source"] = $translationSource
            $Summary.build.app_resources["ui_translations_sha256"] = $translationHash
        }
    } elseif ($Summary.build.app_assets_sha256 -ne $assetsHash) {
        Stop-Benchmark "app_assets_changed"
    }
    Write-StagedIni (Join-Path $app "kz3.ini") $Root $data $Launch.ini
    $displayLines = @("[display]") + @($DisplaySettings.Keys | ForEach-Object {
        "$_ = $($DisplaySettings[$_])"
    })
    $display = ($displayLines -join "`r`n") + "`r`n"
    [IO.File]::WriteAllText((Join-Path $data "display.ini"), $display, [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $data "online.ini"), "[online]`r`n", [Text.Encoding]::ASCII)
    $shaderCache = Join-Path $cache "shaders"
    $edatCache = Join-Path $cache "edat"
    $cacheRoot = Join-Path $cache "root"
    New-Item -ItemType Directory -Path $shaderCache, $edatCache, $cacheRoot | Out-Null
    $override = @{
        KZ3_CACHE_ROOT = $cacheRoot; PS3_HDD0_ROOT = (Join-Path $data "gamedata\dev_hdd0")
        PS3_HDD1_ROOT = (Join-Path $cacheRoot "cache"); PS3_EDAT_CACHE = $edatCache
        RSX_SHADER_CACHE_DIR = $shaderCache; KZ3_KEYBINDS = (Join-Path $data "keybinds.ini")
        KZ3_WINDOW_SIZE = $Conditions.resolution; KZ3_DISPLAY = "windowed"; KZ3_DISPLAY_SCALING = "stretch"
        KZ3_DISPLAY_FILTER = "bilinear"; KZ3_FPS_LIMIT = "30"; KZ3_BLOOM = "100"
        KZ3_UNFOCUSED = "keep"; KZ3_MOUSE_LOOK = "0"; KZ3_DISCORD_PRESENCE = "0"
        PAD_NO_KEYBOARD = "1"; PAD_SCRIPT = $Route.script; PAD_CLOCK = "wall"
        GCM_FLIPCOUNT = "1"; RSX_PACE = "1"
        PS3_FSLOG = "1"; KZ3_LOG_STDOUT = "1"; KZ3_NO_ERROR_BOX = "1"; KZ3_ERROR_NOTICE_TIMEOUT = "15"
        PS3_LANG = "1"; KZ3_LANG = "1"; LD_PSO_TEMPLATE = "0"
    }
    foreach ($name in $script:Candidates) { $override[$name] = $SwitchValues[$name] }
    $arguments = [string[]]@()
    if ($Launch.argv.Count -gt 1) { $arguments = [string[]]$Launch.argv[1..($Launch.argv.Count - 1)] }
    $info = New-ChildInfo $stagedExe $arguments $data $Launch $override
    $effective = Get-EffectiveSwitches $info $Route.sha256
    return [PSCustomObject]@{ info = $info; effective = $effective; data = $data
                              log = (Join-Path $Directory "combined.log") }
}

function Write-Summary([string]$Directory, $Summary) {
    $json = $Summary | ConvertTo-Json -Depth 10
    $encoding = New-Object Text.UTF8Encoding($false)
    $jsonFile = Join-Path $Directory "summary.json"
    [IO.File]::WriteAllText($jsonFile + ".new", $json + "`n", $encoding)
    Move-Item -LiteralPath ($jsonFile + ".new") -Destination $jsonFile -Force
    $rows = foreach ($run in $Summary.runs) {
        [PSCustomObject]@{ run = $run.run; side = $run.side; valid = $run.valid; reason = $run.reason;
                          fps_mean_flip_approx = $run.fps_mean_flip_approx; gpu_time_status = $run.gpu_time_status
                          mode = $run.mode; candidate = $run.candidate }
    }
    $csv = if (@($rows).Count) { @($rows | ConvertTo-Csv -NoTypeInformation) }
           else { @('"run","side","valid","reason","fps_mean_flip_approx","gpu_time_status","mode","candidate"') }
    $csvFile = Join-Path $Directory "summary.csv"
    [IO.File]::WriteAllLines($csvFile + ".new", [string[]]$csv, $encoding)
    Move-Item -LiteralPath ($csvFile + ".new") -Destination $csvFile -Force
}

function Invoke-MissionBenchmark {
    if ($Baseline -and $Candidate) { Stop-Benchmark "baseline_candidate_conflict" }
    if (-not $Baseline -and -not $Candidate) { Stop-Benchmark "candidate_required" }
    if ($Candidate -and $Candidate -notin $script:Candidates) { Stop-Benchmark "candidate_invalid" }
    $developerMode = $script:DeveloperBuildRequested -or [bool]$DeveloperBuildDir
    if ($developerMode -and [string]::IsNullOrWhiteSpace($DeveloperBuildDir)) {
        Stop-Benchmark "developer_build_dir_invalid"
    }
    if ($InitializeProfile -and -not $developerMode) { Stop-Benchmark "initialize_profile_requires_developer_build" }
    [int]$minimum = 0
    if (-not [int]::TryParse($SamplesPerSide, [ref]$minimum) -or $minimum -lt 5 -or $minimum -gt 100) {
        Stop-Benchmark "sample_count_invalid"
    }
    $mode = if ($Baseline) { "baseline" } else { "candidate" }
    $selected = if ($Baseline) { $null } else { $Candidate.ToUpperInvariant() }
    if (-not (Test-Path -LiteralPath $script:Helper -PathType Leaf)) { Stop-Benchmark "helper_missing" }
    $pythonExe = Get-Python $Python
    $route = Invoke-JsonTool $pythonExe @("-B", $script:Helper, "route")
    $bootstrapRoute = if ($InitializeProfile) {
        Invoke-JsonTool $pythonExe @("-B", $script:Helper, "route", "--kind", "first-boot")
    } else { $null }
    $bootstrap = [ordered]@{
        status = if ($InitializeProfile) { "if_source_has_no_profile" } else { "disabled" }
        route = if ($InitializeProfile) { "first-boot" } else { $null }
        route_sha256 = if ($InitializeProfile) { $bootstrapRoute.sha256 } else { $null }
        provenance = if ($InitializeProfile) { "developer_cmake" } else { $null }
    }
    $schedule = Get-Schedule $minimum -BaselineMode:$Baseline
    $candidateSwitches = [ordered]@{ off = (Get-CandidateSwitches $mode $selected "off") }
    if ($mode -eq "candidate") { $candidateSwitches["on"] = Get-CandidateSwitches $mode $selected "on" }
    $displaySettings = [ordered]@{
        mode = "windowed"; scaling = "stretch"; filter = "bilinear"; vsync = "on"; motion_blur = "100"
        fps_limit = "30"; fov = "default"; hud_169 = "on"; bloom = "100"; unfocused = "keep"
        maximize = "borderless"
    }
    $conditions = [ordered]@{
        mode = $mode; candidate = $selected; candidate_switches = $candidateSwitches
        build_source = if ($developerMode) { "developer_cmake" } else { "installed" }
        workload = "mission_0_profile_route"; route_sha256 = $route.sha256; measurement_clock = $route.clock
        window_start_seconds = $route.window_start; window_end_seconds = $route.window_end
        duration_seconds = ([int]$route.duration_seconds + 15); resolution = "1920x1080"
        display = "windowed, stretch, bilinear, vsync on, 30 fps"; display_settings = $displaySettings
        language = "English US"; cache = "fresh per-run application caches, pipeline template disabled"
        gpu = "operator confirmation, cooperative mutex, best-effort snapshots; exclusivity unverified"
        environment = "windows_allowlist_v1"; pad_alignment = "shared monotonic timestamps at pad and flip production"
        pad_max_lateness_seconds = $route.pad_lateness_seconds
        pad_max_alignment_error_seconds = $route.pad_alignment_seconds; switch_effect = "not verified"
        fs_logging = "enabled for route validation"
    }
    if ($mode -eq "baseline") { $conditions["requested_valid_baseline_windows"] = $minimum }
    else { $conditions["requested_valid_windows_per_side"] = $minimum }
    if ($Plan) {
        [ordered]@{ status = "plan_only"; mode = $mode; candidate = $selected
                    conditions = $conditions; schedule = $schedule; bootstrap = $bootstrap
                    bootstrap_duration_seconds = if ($InitializeProfile) {
                        [int]$bootstrapRoute.duration_seconds + 15
                    } else { $null } } |
            ConvertTo-Json -Depth 8
        return
    }
    if (-not $ConfirmExclusiveGpu) { Stop-Benchmark "exclusive_gpu_confirmation_required" }
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { Stop-Benchmark "windows_required" }
    $root = if ($InstallRoot) { [IO.Path]::GetFullPath($InstallRoot) } else { $script:Repo }
    if (Test-AtOrBelow $root (Join-Path $script:Repo "vfs")) { Stop-Benchmark "launcher_root_invalid" }
    if ($developerMode -and @($root -split '[\\/]') -contains "vfs") {
        Stop-Benchmark "link_free_launcher_required"
    }
    Assert-NoReparseAncestors $root
    if (-not (Test-Path -LiteralPath $script:Launcher -PathType Leaf)) { Stop-Benchmark "launcher_missing" }
    if ($developerMode) {
        $developerRoot = [IO.Path]::GetFullPath($DeveloperBuildDir)
        if (@($developerRoot -split '[\\/]') -contains "vfs") { Stop-Benchmark "link_free_launcher_required" }
        Assert-NoReparseAncestors $developerRoot "developer_build_dir_invalid" "developer_path_link"
        if (-not (Test-Path -LiteralPath $developerRoot -PathType Container)) {
            Stop-Benchmark "developer_build_dir_invalid"
        }
        $build = Invoke-JsonTool $pythonExe @("-B", $script:Helper, "verify-developer-build",
                                              "--repo", $script:Repo, "--build", $developerRoot) 120000
        $launch = Get-LaunchPlan $pythonExe $script:Helper $root $developerRoot
    } else {
        $launch = Get-LaunchPlan $pythonExe $script:Helper $root
        $build = Invoke-JsonTool $pythonExe @("-B", $script:Helper, "verify-build", "--repo", $script:Repo, "--install", $root)
    }
    $exe = [string]$launch.argv[0]
    $exeHash = Get-FileSha256 $exe
    $buildSummary = @{ exe_sha256 = $exeHash; app_assets_sha256 = $null; app_resources = $null
                       patchset = $build.patchset; git_head = $build.git_head }
    if ($developerMode) {
        $verifiedExe = [string](Require-Property $build "exe_sha256")
        $cacheHash = [string](Require-Property $build "cmake_cache_sha256")
        $generator = [string](Require-Property $build "cmake_generator")
        $configured = [string](Require-Property $build "configure_toolkit_stamp")
        $freshness = [string](Require-Property $build "freshness")
        $ebootMode = [string](Require-Property $build "eboot_mode")
        $embeddedHash = [string](Require-Property $build "eboot_sha256")
        $launchMode = [string](Require-Property $launch "eboot_mode")
        $launchHash = [string](Require-Property $launch "eboot_sha256")
        $launchCacheHash = [string](Require-Property $launch "cmake_cache_sha256")
        if ($verifiedExe -notmatch '^[0-9a-f]{64}$' -or $cacheHash -notmatch '^[0-9a-f]{64}$' -or
            $generator -notin @("Ninja", "Ninja Multi-Config") -or
            $configured -notin @("matched", "not_recorded") -or
            $freshness -ne "ninja_dry_run_clean" -or $ebootMode -notin @("argument", "embedded") -or
            $launchHash -cnotmatch '^[0-9a-f]{64}$' -or
            ($ebootMode -eq "argument" -and $embeddedHash) -or
            ($ebootMode -eq "embedded" -and $embeddedHash -cnotmatch '^[0-9a-f]{64}$')) {
            Stop-Benchmark "developer_provenance_invalid"
        }
        if ($verifiedExe -cne $exeHash) { Stop-Benchmark "executable_changed" }
        if ($launchMode -cne $ebootMode -or $launchCacheHash -cne $cacheHash -or
            ($ebootMode -eq "embedded" -and $embeddedHash -cne $launchHash)) {
            Stop-Benchmark "developer_provenance_changed"
        }
        Assert-DeveloperEboot $launch
        $buildSummary["source"] = "developer_cmake"
        $buildSummary["launcher_action"] = [string]$launch.action
        $buildSummary["eboot_mode"] = $ebootMode
        $buildSummary["eboot_sha256"] = $launchHash
        $buildSummary["cmake"] = @{ cache_sha256 = $cacheHash; generator = $generator
                                    configure_toolkit_stamp = $configured; freshness = $freshness }
    }
    $source = if ($ProfileSource) { [IO.Path]::GetFullPath($ProfileSource) }
              else { Join-Path ([string]$launch.cwd) "gamedata" }
    foreach ($forbidden in @((Join-Path $root "vfs"), (Join-Path $script:Repo "vfs"),
                             [string]$launch.env.PS3_VFS_ROOT, [string]$launch.env.KZ3_CACHE_ROOT)) {
        if (Test-AtOrBelow $source $forbidden) { Stop-Benchmark "profile_source_invalid" }
    }
    $workBase = Join-Path $script:Repo "work"
    $work = Join-Path $workBase "helsari-p0"
    $session = Join-Path $work ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss") + "-" +
                                [Guid]::NewGuid().ToString("N").Substring(0, 8))
    $bootstrapSession = $session + "-bootstrap"
    $checkArgs = @("-B", $script:Helper, "check-profile", "--source", $source, "--session", $session)
    if ($InitializeProfile) { $checkArgs += "--allow-missing" }
    [void](Invoke-JsonTool $pythonExe $checkArgs)
    if ($InitializeProfile) {
        $checkArgs = @("-B", $script:Helper, "check-profile", "--source", $source, "--session", $bootstrapSession,
                       "--allow-missing")
        [void](Invoke-JsonTool $pythonExe $checkArgs)
    }
    foreach ($forbidden in @((Join-Path $root "vfs"), (Join-Path $script:Repo "vfs"),
                             [string]$launch.env.PS3_VFS_ROOT, [string]$launch.env.KZ3_CACHE_ROOT)) {
        if (Test-AtOrBelow $forbidden $source) { Stop-Benchmark "profile_source_invalid" }
    }
    $needBootstrap = $false
    if ($InitializeProfile) {
        $inventory = Invoke-JsonTool $pythonExe @("-B", $script:Helper, "inspect-profile", "--source", $source,
                                                  "--session", $session)
        $needBootstrap = $inventory.profile_count -eq 0
        $bootstrap.status = if ($needBootstrap) { "pending" } else { "not_needed" }
    }
    foreach ($directory in @($workBase, $work)) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            New-Item -ItemType Directory -Path $directory | Out-Null
        }
        if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            Stop-Benchmark "output_directory_link"
        }
    }
    New-Item -ItemType Directory -Path $session | Out-Null
    $summary = [ordered]@{ schema = 2; status = "in_progress"; reason = "not_completed"
                           mode = $mode; candidate = $selected; conditions = $conditions
                           valid_window_counts = [ordered]@{ off = 0; on = 0; total = 0 }
                           gpu_exclusivity = "unverified"; gpu_preflight = $null
                           build = $buildSummary; bootstrap = $bootstrap
                           runs = @() }
    Write-Summary $session $summary
    $mutex = $null
    $held = $false
    try {
        $mutex = New-Object Threading.Mutex($false, "Global\ProjectHelghast-HelsariP0-GPU")
        try { $held = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $held = $true }
        if (-not $held) { Stop-Benchmark "gpu_lock_busy" }
        $summary.gpu_preflight = Get-GpuEvidence -1
        if ($summary.gpu_preflight.status -ne "sampled") {
            Write-Output "GPU counters unavailable; exclusivity remains unverified."
        } elseif ($summary.gpu_preflight.foreign_activity -eq "observed") {
            Write-Output "Other GPU activity observed; exclusivity remains unverified."
        }
        $seed = Join-Path $session "seed"
        if ($needBootstrap) {
            $summary.bootstrap.status = "in_progress"
            Write-Summary $session $summary
            Write-Output "initializing a private profile using the first-boot route"
            try {
                $bootSwitches = Get-CandidateSwitches "baseline" "" "off"
                $prepared = New-BenchmarkProcess $bootstrapSession "" $root $exe $exeHash $launch $bootstrapRoute `
                    $conditions $displaySettings $bootSwitches $true $pythonExe $summary
                [void](Invoke-ControlledProcess $pythonExe $prepared.info $prepared.log `
                    ([int]$bootstrapRoute.duration_seconds + 15) 10)
                $result = Invoke-JsonTool $pythonExe @("-B", $script:Helper, "analyze-first-boot",
                                                       "--log", $prepared.log)
                if (-not $result.valid -or $result.route_sha256 -cne $bootstrapRoute.sha256) {
                    Stop-Benchmark "bootstrap_invalid"
                }
                $bootSource = Join-Path $prepared.data "gamedata"
                [void](Invoke-JsonTool $pythonExe @("-B", $script:Helper, "seed", "--source", $bootSource,
                                                    "--destination", $seed))
                $summary.bootstrap.status = "complete"
            } catch {
                $summary.bootstrap.status = "failed"
                throw
            }
        } else {
            [void](Invoke-JsonTool $pythonExe @("-B", $script:Helper, "seed", "--source", $source, "--destination", $seed))
        }
        $number = 0
        foreach ($side in $schedule) {
            $number++
            $run = [ordered]@{ run = $number; mode = $mode; candidate = $selected
                               side = $side; valid = $false; reason = "not_completed"
                               fps_mean_flip_approx = $null; gpu_time_status = "unavailable"
                               effective_environment = $null; pad_clock_alignment = $null
                               gpu_before = $null; gpu_after = $null }
            $summary.runs += $run
            Write-Summary $session $summary
            Write-Output ("run {0}/{1}: {2}" -f $number, $schedule.Count, $side)
            try {
                $run.gpu_before = Get-GpuEvidence -1
                $dir = Join-Path $session ("run-{0:D3}-{1}" -f $number, $side)
                if (-not $candidateSwitches.Contains($side)) { Stop-Benchmark "schedule_invalid" }
                $prepared = New-BenchmarkProcess $dir $seed $root $exe $exeHash $launch $route $conditions `
                    $displaySettings $candidateSwitches[$side] $developerMode $pythonExe $summary
                $run.effective_environment = $prepared.effective
                Write-Summary $session $summary
                [void](Invoke-ControlledProcess $pythonExe $prepared.info $prepared.log $conditions.duration_seconds 10)
                $run.gpu_after = Get-GpuEvidence -1
                $result = Invoke-JsonTool $pythonExe @("-B", $script:Helper, "analyze", "--log", $prepared.log)
                if (-not $result.valid) { Stop-Benchmark "window_invalid" }
                $run.valid = $true
                $run.reason = "ok"
                $run.fps_mean_flip_approx = $result.fps_mean_flip_approx
                $run.pad_clock_alignment = $result.pad_clock_alignment
            } catch {
                $run.reason = Get-BenchmarkReason $_
                throw
            } finally {
                $summary.valid_window_counts = Get-ValidWindowCounts $summary.runs
                Write-Summary $session $summary
            }
        }
        Assert-ValidWindowCounts $summary.runs $minimum $mode
        $summary.status = "complete"
        $summary.reason = "ok"
    } catch {
        $summary.status = "failed"
        $summary.reason = Get-BenchmarkReason $_
        throw
    } finally {
        Write-Summary $session $summary
        if ($held) { $mutex.ReleaseMutex() }
        if ($null -ne $mutex) { $mutex.Dispose() }
    }
    Write-Output "benchmark complete; sanitized summaries are under work\helsari-p0"
}

if ($MyInvocation.InvocationName -ne ".") {
    try {
        Invoke-MissionBenchmark
    } catch {
        [Console]::Error.WriteLine("benchmark failed: " + (Get-BenchmarkReason $_))
        exit 1
    }
}
