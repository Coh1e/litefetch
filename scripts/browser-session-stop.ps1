# Stops everything browser-session.ps1 started.
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$probe = Join-Path $root 'tmp\browser-probe'

$serverJob = Get-Content (Join-Path $probe 'server-job.txt') -ErrorAction SilentlyContinue
if ($serverJob) {
    Stop-Job -Id $serverJob -ErrorAction SilentlyContinue
    Remove-Job -Id $serverJob -Force -ErrorAction SilentlyContinue
}
Get-Process litefetch-browser-host -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process litefetch-win32 -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process chrome -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -like '*Chrome*' } | Stop-Process -Force -ErrorAction SilentlyContinue
Write-Host 'browser session stopped'
