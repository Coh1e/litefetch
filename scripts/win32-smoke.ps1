# Process-level smoke test for the Win32 product.
#
# Everything else about the window is covered in-process by ui-window-tests,
# which drives the real MainWindow without a second process. This script keeps
# the one thing that only a real launch can prove: the built exe starts on its
# own, shows the window, and exits with code 0 when closed.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts/win32-smoke.ps1
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $RepositoryRoot).Path
$appPath = Join-Path $root 'out/build/windows-core/litefetch-win32.exe'
$depsBin = Join-Path $root '.deps/installed-core/x64-windows/debug/bin'
$evidence = Join-Path $root '.pi/evidence/smoke'
$env:PATH = "$depsBin;$env:PATH"
if (-not (Test-Path $appPath)) { throw "missing build output: $appPath" }
New-Item -ItemType Directory -Force $evidence | Out-Null
$runDir = Join-Path $evidence ("run-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $runDir | Out-Null
$downloads = Join-Path $runDir 'downloads'
New-Item -ItemType Directory -Force $downloads | Out-Null

Add-Type -Namespace Smoke -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
'@

$app = Start-Process -FilePath $appPath -ArgumentList @(
    '--db', (Join-Path $runDir 'tasks.db'), '--download-dir', $downloads) -PassThru
try {
    $deadline = (Get-Date).AddSeconds(20)
    $window = [IntPtr]::Zero
    while ((Get-Date) -lt $deadline -and $window -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 100
        $window = [Smoke.Native]::FindWindowW('LiteFetchMainWindow', 'lite fetch')
    }
    if ($window -eq [IntPtr]::Zero) { throw 'main window did not appear' }
    [void][Smoke.Native]::PostMessageW($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $app.WaitForExit(30000)) { throw 'process did not exit within 30 s' }
    $app.WaitForExit()
    if ($app.ExitCode -ne 0) { throw "process exited with code $($app.ExitCode)" }
    "PASS: window opened and the process exited with code 0"
} finally {
    if ($app -and -not $app.HasExited) { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
}
