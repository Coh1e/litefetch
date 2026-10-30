# Checks that every binary in the build tree can actually start.
#
# A missing dependency DLL surfaces as exit code 0xC0000135 (STATUS_DLL_NOT_FOUND),
# and CTest reports that as a timeout, which reads like a hung test. This runs one
# binary per dependency set and fails on that exit code specifically.
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDir = Join-Path $root 'out/build/windows-core'

$probes = @(
    @{exe = 'core-service-tests.exe'; why = 'core plus libcurl'},
    @{exe = 'core-store-tests.exe'; why = 'core plus sqlite'},
    @{exe = 'core-bt-tests.exe'; why = 'core plus libtorrent'},
    @{exe = 'litefetch-win32.exe'; why = 'the window'}
)

$failed = @()
foreach ($probe in $probes) {
    $path = Join-Path $buildDir $probe.exe
    if (-not (Test-Path $path)) { $failed += "$($probe.exe): not built"; continue }
    # --help or a normal start is enough: what matters is that the loader got past
    # every import before the program ran.
    $process = Start-Process -FilePath $path -ArgumentList '--help' -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $env:TEMP 'check-binaries.out') `
        -RedirectStandardError (Join-Path $env:TEMP 'check-binaries.err')
    $null = $process.WaitForExit(15000)
    if (-not $process.HasExited) { $process.Kill(); continue }  # a GUI app that stays open is fine
    if ($process.ExitCode -eq -1073741515) {
        $failed += "$($probe.exe): missing dependency DLL ($($probe.why))"
    }
}

if ($failed.Count -gt 0) {
    $failed | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
Write-Host 'all probed binaries load'
