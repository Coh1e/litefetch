# Runs the acceptance suite with a wall-clock cap.
#
# ctest's per-test TIMEOUT has failed to stop a hung test (core-service sat for
# 36 minutes on 2026-10-31), so the whole run is bounded here and the process
# tree is killed on expiry. TEMP/TMP point at the project, matching the presets.
param(
    [int]$TimeoutSeconds = 900,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CtestArgs
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDir = Join-Path $root 'out/build/windows-core'
$temp = Join-Path $root '.deps/tmp'
New-Item -ItemType Directory -Force $temp | Out-Null
$env:TEMP = $temp
$env:TMP = $temp
$env:LITEFETCH_TEST_TEMP = $temp

$arguments = "--test-dir `"$buildDir`" --output-on-failure --no-tests=error"
if ($CtestArgs) { $arguments += ' ' + ($CtestArgs -join ' ') }

$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = 'ctest'
$startInfo.Arguments = $arguments
$startInfo.UseShellExecute = $false
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true

$process = [System.Diagnostics.Process]::Start($startInfo)
$stdout = $process.StandardOutput.ReadToEndAsync()
$stderr = $process.StandardError.ReadToEndAsync()

if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    & taskkill /PID $process.Id /T /F 2>&1 | Out-Null
    $text = "[acceptance] suite exceeded $TimeoutSeconds s and was killed"
    $code = 124
} else {
    $process.WaitForExit()
    $text = $stdout.Result + $stderr.Result
    $code = $process.ExitCode
}

Write-Host $text
$log = Join-Path $root 'tmp/acceptance.log'
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
[IO.File]::WriteAllText($log, $text, (New-Object System.Text.UTF8Encoding($false)))
exit $code
