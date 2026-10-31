# Builds, tests and packages LiteFetch with one command.
#
# The same script runs locally and in GitHub Actions, so a green pipeline and a
# green local run mean the same thing. Artifacts and logs always land in the
# repository, under out/ci, no matter who starts the run:
#
#   out/ci/artifacts/litefetch-<version>-win64.zip   portable build
#   out/ci/artifacts/SHA256SUMS.txt                  checksum of every artifact
#   out/ci/summary.md                                what happened, in Chinese
#   out/ci/manifest.json                             the same facts for machines
#   out/ci/logs/*.log                                per-step output
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\ci.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\ci.ps1 -Smoke      # release gate
#   powershell -ExecutionPolicy Bypass -File scripts\ci.ps1 -SkipTests  # packaging only
[CmdletBinding()]
param(
    [string]$Preset = 'windows-release',
    # A real start of the built exe. Per ADR-0001 this is a release-gate check,
    # not part of per-ticket acceptance, so it is off unless asked for.
    [switch]$Smoke,
    [switch]$SkipTests,
    [switch]$SkipPackage,
    # Wall-clock cap for the whole ctest run; a hung test is killed with its tree.
    [int]$TestTimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'

if ($env:LITEFETCH_CI_SMOKE -eq 'true') { $Smoke = $true }

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

# CMakePresets points TEMP/TMP at .deps/tmp. A fresh checkout has neither, and
# MSVC reports D8050 ("cannot execute c1.dll") when its temp directory is
# missing, which is exactly what a clean GitHub runner hit.
New-Item -ItemType Directory -Force (Join-Path $root '.deps/tmp') | Out-Null

$ciRoot = Join-Path $root 'out/ci'
$artifactDir = Join-Path $ciRoot 'artifacts'
$logDir = Join-Path $ciRoot 'logs'
New-Item -ItemType Directory -Force $artifactDir, $logDir | Out-Null
Get-ChildItem $artifactDir -File -Force | Remove-Item -Force

$buildDir = Join-Path $root "out/build/$Preset"
$projectText = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
$versionMatch = [regex]::Match($projectText, 'project\(\s*NativeDownloader\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) { throw 'cannot read the project version from CMakeLists.txt' }
$version = $versionMatch.Groups[1].Value

$commit = (& git rev-parse --short HEAD 2>$null)
if (-not $commit) { $commit = 'unknown' }
$ref = $env:GITHUB_REF_NAME
if (-not $ref) { $ref = 'local' }
$startedAt = Get-Date

. (Join-Path $PSScriptRoot 'enter-msvc.ps1') -Quiet

function Write-Utf8 {
    param([string]$Path, [string]$Text, [switch]$Append)
    $encoding = New-Object System.Text.UTF8Encoding($false)
    if ($Append) {
        [IO.File]::AppendAllText($Path, $Text, $encoding)
    } else {
        [IO.File]::WriteAllText($Path, $Text, $encoding)
    }
}

# Tee-Object writes UTF-16 in Windows PowerShell; logs are more useful as UTF-8.
function Convert-LogToUtf8 {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $text = [IO.File]::ReadAllText($Path)
    [IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

function Invoke-Step {
    param([string]$Name, [scriptblock]$Body, [switch]$AllowFailure)
    $log = Join-Path $logDir "$Name.log"
    $clock = [Diagnostics.Stopwatch]::StartNew()
    Write-Host "==> $Name"
    $threw = $null
    $savedPreference = $ErrorActionPreference
    try {
        # A native tool writes warnings to stderr. With ErrorActionPreference Stop
        # Windows PowerShell turns those lines into terminating errors, so the
        # step would fail even when the tool exits 0. The exit code is the verdict.
        $ErrorActionPreference = 'Continue'
        & $Body 2>&1 | Tee-Object -FilePath $log | Out-Host
    } catch {
        $threw = $_
        $_ | Out-String | Tee-Object -FilePath $log -Append | Out-Host
    } finally {
        $ErrorActionPreference = $savedPreference
    }
    $code = if ($threw) { 1 } else { $LASTEXITCODE }
    $clock.Stop()
    Convert-LogToUtf8 $log
    if ($code -ne 0 -and -not $AllowFailure) {
        throw "$Name failed with exit code $code (see $log)"
    }
    return [pscustomobject]@{ Name = $Name; ExitCode = $code; Seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1); Log = $log }
}

$steps = [System.Collections.Generic.List[object]]::new()
$failure = $null
$tests = [pscustomobject]@{ Total = 0; Failed = 0; Seconds = 0.0; Status = '未运行' }
$smokeResult = [pscustomobject]@{ Status = '未运行'; WindowMillis = 0; ExitCode = $null }
$packageDir = $null
$zipPath = $null

try {
    $steps.Add((Invoke-Step 'check-scripts' { & (Join-Path $PSScriptRoot 'check-scripts.ps1') }))
    $steps.Add((Invoke-Step 'check-project-local' { & (Join-Path $PSScriptRoot 'check-project-local.ps1') }))
    $steps.Add((Invoke-Step 'check-no-vcpkg' { & (Join-Path $PSScriptRoot 'check-no-vcpkg.ps1') }))

    $steps.Add((Invoke-Step 'configure' { & cmake --preset $Preset }))
    $steps.Add((Invoke-Step 'build' { & cmake --build $buildDir --config Release }))

    if (-not $SkipTests) {
        # ctest's per-test TIMEOUT is the first line of defence, but a hung test
        # can outlive it (observed 2026-10-31: core-service sat for 36 minutes).
        # A wall clock cap keeps CI and a local run from hanging forever.
        #
        # TEMP/TMP match test-local.cmd: several tests have no per-test ENV entry
        # and would otherwise write to the user's %TEMP%.
        $testTemp = Join-Path $root 'tmp/core-test-temp'
        New-Item -ItemType Directory -Force $testTemp | Out-Null
        $env:TEMP = $testTemp
        $env:TMP = $testTemp
        $env:LITEFETCH_TEST_TEMP = $testTemp

        $testLog = Join-Path $logDir 'ctest.log'
        $testErr = Join-Path $logDir 'ctest-err.log'
        $clock = [Diagnostics.Stopwatch]::StartNew()
        Write-Host '==> ctest'
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = 'ctest'
        $startInfo.Arguments = "--test-dir `"$buildDir`" --output-on-failure --no-tests=error"
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $proc = [System.Diagnostics.Process]::Start($startInfo)
        $stdout = $proc.StandardOutput.ReadToEndAsync()
        $stderr = $proc.StandardError.ReadToEndAsync()
        if (-not $proc.WaitForExit($TestTimeoutSeconds * 1000)) {
            & taskkill /PID $proc.Id /T /F 2>&1 | Out-Null
            $stdoutText = "[ci] ctest exceeded $TestTimeoutSeconds s and was killed"
            $stderrText = ''
            $code = 124
        } else {
            $proc.WaitForExit()
            $stdoutText = $stdout.Result
            $stderrText = $stderr.Result
            $code = [int]$proc.ExitCode
        }
        $clock.Stop()
        Write-Utf8 $testLog ($stdoutText + $stderrText)
        if ($stderrText) { Write-Utf8 $testErr $stderrText }
        $step = [pscustomobject]@{ Name = 'ctest'; ExitCode = $code; Seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1); Log = $testLog }
        $steps.Add($step)
        $ctestLog = Get-Content -LiteralPath $testLog -Raw
        $total = [regex]::Match($ctestLog, 'out of (\d+)')
        $failed = [regex]::Match($ctestLog, '(\d+) tests failed')
        $seconds = [regex]::Match($ctestLog, 'Total Test time \(real\)\s*=\s*([0-9.]+)')
        $tests = [pscustomobject]@{
            Total   = if ($total.Success) { [int]$total.Groups[1].Value } else { 0 }
            Failed  = if ($failed.Success) { [int]$failed.Groups[1].Value } else { 0 }
            Seconds = if ($seconds.Success) { [double]$seconds.Groups[1].Value } else { 0.0 }
            Status  = if ($code -eq 0) { '通过' } else { '失败' }
        }
        if ($code -ne 0) { throw "ctest failed with exit code $code" }
    }

    if (-not $SkipPackage) {
        $steps.Add((Invoke-Step 'package' { & (Join-Path $PSScriptRoot 'package-release.ps1') -SkipBuild }))
        $packageDir = Join-Path $root "out/package/litefetch-$version"
        if (-not (Test-Path -LiteralPath (Join-Path $packageDir 'litefetch-win32.exe'))) {
            throw "package is missing litefetch-win32.exe: $packageDir"
        }

        $zipPath = Join-Path $artifactDir "litefetch-$version-win64.zip"
        Compress-Archive -Path (Join-Path $packageDir '*') -DestinationPath $zipPath -Force
    }

    if ($Smoke) {
        $exe = Join-Path $buildDir 'litefetch-win32.exe'
        $smokeLog = Join-Path $logDir 'smoke.log'
        $clock = [Diagnostics.Stopwatch]::StartNew()
        & (Join-Path $PSScriptRoot 'win32-smoke.ps1') -AppPath $exe `
            -EvidenceRoot (Join-Path $root '.pi/evidence/ci-smoke') 2>&1 |
            Tee-Object -FilePath $smokeLog | Out-Host
        $code = $LASTEXITCODE
        $clock.Stop()
        # win32-smoke writes its lines with Write-Host, so Tee may never create
        # the file. Fall back to the evidence log the script always writes.
        if (-not (Test-Path -LiteralPath $smokeLog)) {
            $latest = Get-ChildItem (Join-Path $root '.pi/evidence/ci-smoke') -Recurse `
                -Filter smoke.log -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if ($latest) { Copy-Item -LiteralPath $latest.FullName -Destination $smokeLog }
        }
        if (-not (Test-Path -LiteralPath $smokeLog)) { Write-Utf8 $smokeLog '' }
        $text = Get-Content -LiteralPath $smokeLog -Raw
        $window = [regex]::Match($text, 'window: appeared after (\d+) ms')
        $smokeResult = [pscustomobject]@{
            Status       = if ($code -eq 0) { '通过' } else { '失败' }
            WindowMillis = if ($window.Success) { [int]$window.Groups[1].Value } else { 0 }
            ExitCode     = $code
        }
        $steps.Add([pscustomobject]@{ Name = 'smoke'; ExitCode = $code; Seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1); Log = $smokeLog })
        if ($code -ne 0) { throw "smoke failed with exit code $code (see $smokeLog)" }
    }
} catch {
    $failure = $_.Exception.Message
    Write-Host "FAILED: $failure" -ForegroundColor Red
} finally {
    # The summary and manifest are written on failure too: a red run must still
    # tell a human what passed, what broke and where the logs are.
    $status = if ($failure) { '失败' } else { '通过' }

    $artifacts = @()
    foreach ($file in Get-ChildItem $artifactDir -File -Force) {
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $artifacts += [pscustomobject]@{ name = $file.Name; bytes = $file.Length; sha256 = $hash }
    }
    if ($artifacts.Count -gt 0) {
        $sums = $artifacts | ForEach-Object { "$($_.sha256)  $($_.name)" }
        Write-Utf8 (Join-Path $artifactDir 'SHA256SUMS.txt') (($sums -join "`n") + "`n")
        $artifacts += [pscustomobject]@{
            name   = 'SHA256SUMS.txt'
            bytes  = (Get-Item (Join-Path $artifactDir 'SHA256SUMS.txt')).Length
            sha256 = (Get-FileHash (Join-Path $artifactDir 'SHA256SUMS.txt') -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }

    $finishedAt = Get-Date
    $manifest = [ordered]@{
        status      = $status
        failure     = $failure
        version     = $version
        commit      = $commit
        ref         = $ref
        preset      = $Preset
        startedAt   = $startedAt.ToString('o')
        finishedAt  = $finishedAt.ToString('o')
        tests       = [ordered]@{ total = $tests.Total; failed = $tests.Failed; seconds = $tests.Seconds; status = $tests.Status }
        smoke       = [ordered]@{ status = $smokeResult.Status; windowMillis = $smokeResult.WindowMillis; exitCode = $smokeResult.ExitCode }
        steps       = @($steps | ForEach-Object { [ordered]@{ name = $_.Name; exitCode = $_.ExitCode; seconds = $_.Seconds; log = $_.Log } })
        artifacts   = @($artifacts | ForEach-Object { [ordered]@{ name = $_.name; bytes = $_.bytes; sha256 = $_.sha256 } })
        artifactDir = $artifactDir
        logDir      = $logDir
    }
    $manifestJson = $manifest | ConvertTo-Json -Depth 6
    Write-Utf8 (Join-Path $ciRoot 'manifest.json') $manifestJson

    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("# LiteFetch $version 构建产物")
    $lines.Add('')
    $lines.Add("- 状态：**$status**$(if ($failure) { "（$failure）" })")
    $lines.Add("- 提交：``$commit``（``$ref``）")
    $lines.Add("- 预设：``$Preset``")
    $lines.Add("- 生成时间：$($finishedAt.ToString('yyyy-MM-dd HH:mm:ss'))，耗时 $([math]::Round(($finishedAt - $startedAt).TotalSeconds, 0)) 秒")
    $lines.Add("- 测试：$($tests.Total - $tests.Failed)/$($tests.Total) 通过（$($tests.Seconds) 秒）")
    if ($smokeResult.Status -ne '未运行') {
        $lines.Add("- 冒烟：$($smokeResult.Status)（窗口 $($smokeResult.WindowMillis) ms，退出码 $($smokeResult.ExitCode)）")
    }
    $lines.Add('')
    $lines.Add('## 产物')
    $lines.Add('')
    if ($artifacts.Count -eq 0) {
        $lines.Add('本次运行没有生成产物。')
    } else {
        $lines.Add('| 文件 | 大小 | SHA-256 |')
        $lines.Add('|---|---|---|')
        foreach ($artifact in $artifacts) {
            $size = if ($artifact.bytes -ge 1MB) {
                '{0:N1} MiB' -f ($artifact.bytes / 1MB)
            } elseif ($artifact.bytes -ge 1KB) {
                '{0:N0} KiB' -f ($artifact.bytes / 1KB)
            } else {
                "$($artifact.bytes) B"
            }
            $lines.Add("| ``$($artifact.name)`` | $size | ``$($artifact.sha256.Substring(0, 16))…`` |")
        }
    }
    $lines.Add('')
    $lines.Add('## 怎么用')
    $lines.Add('')
    if ($zipPath) {
        $lines.Add('1. 解压 `' + (Split-Path $zipPath -Leaf) + '`。')
        $lines.Add('2. 运行 `litefetch-win32.exe`。任务数据和设置留在程序所在目录。')
        $lines.Add('3. 用 `SHA256SUMS.txt` 核对文件，命令：`Get-FileHash <file> -Algorithm SHA256`。')
    }
    $lines.Add('')
    $lines.Add('## 日志')
    $lines.Add('')
    foreach ($step in $steps) {
        $relative = $step.Log.Substring($root.Length).TrimStart('\', '/').Replace('\', '/')
        $lines.Add("- ``$($step.Name)``（退出码 $($step.ExitCode)，$($step.Seconds) 秒）：``$relative``")
    }
    $lines.Add('')
    $lines.Add('GitHub Actions：在本次运行的 Artifacts 中下载 `LiteFetch-Windows-x64`；推送 tag 时同一份产物附加到 Release。')
    $summary = $lines -join [Environment]::NewLine
    Write-Utf8 (Join-Path $ciRoot 'summary.md') $summary

    if ($env:GITHUB_STEP_SUMMARY) {
        Write-Utf8 $env:GITHUB_STEP_SUMMARY ($summary + [Environment]::NewLine) -Append
    }
}

Write-Host ''
Write-Host "summary:  $(Join-Path $ciRoot 'summary.md')"
Write-Host "manifest: $(Join-Path $ciRoot 'manifest.json')"
if ($zipPath) { Write-Host "package:  $zipPath" }

if ($failure) { exit 1 }
exit 0
