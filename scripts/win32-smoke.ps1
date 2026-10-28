# Process-level smoke test for the Win32 product.
#
# Everything else about the window is covered in-process by ui-window-tests,
# which drives the real MainWindow without a second process. This script keeps
# the one thing only a real launch can prove: the built exe starts on its own,
# shows its window, and exits with code 0 when closed.
#
# Per ticket acceptance does NOT run this. It belongs to the release check (#28);
# see docs/adr/0001-acceptance-without-a-second-process.md.
#
# The launch uses CreateProcess semantics (UseShellExecute = false) on purpose.
# The shell path (Start-Process) asks the shell to trust the binary, and Windows
# refuses to run unsigned binaries without a confirmation dialog on some
# machines: that dialog blocks the caller forever, so an unattended run hangs
# with no output instead of failing. The built exe is unsigned, and signing it
# is #27's job. A user who double-clicks the exe still goes through the shell and
# still sees "unknown publisher" once; this script cannot change that.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/win32-smoke.ps1
#   ... -File scripts/win32-smoke.ps1 -EvidenceRoot .pi/evidence/issue-31
#   ... -File scripts/win32-smoke.ps1 -WindowTimeoutSeconds 0   # timeout branch
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    # How long the main window may take to appear before the run fails.
    # 0 skips the wait entirely, which is how the timeout branch is exercised;
    # it therefore proves the failure path, not that a dialog can be seen.
    [int]$WindowTimeoutSeconds = 20,
    # How long the process may take to exit after WM_CLOSE before the run fails.
    [int]$ExitTimeoutSeconds = 30,
    # Where the run directory and smoke.log go. Relative paths resolve against
    # the repository root. Default: .pi/evidence/smoke (release runs have no
    # ticket number); ticket runs pass .pi/evidence/<ticket>.
    [string]$EvidenceRoot = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $RepositoryRoot).Path
$appPath = Join-Path $root 'out/build/windows-core/litefetch-win32.exe'
$depsBin = Join-Path $root '.deps/installed-core/x64-windows/debug/bin'
if (-not $EvidenceRoot) {
    $EvidenceRoot = Join-Path $root '.pi/evidence/smoke'
} elseif (-not [System.IO.Path]::IsPathRooted($EvidenceRoot)) {
    $EvidenceRoot = Join-Path $root $EvidenceRoot
}
$EvidenceRoot = [System.IO.Path]::GetFullPath($EvidenceRoot)
$env:PATH = "$depsBin;$env:PATH"
if (-not (Test-Path $appPath)) { throw "missing build output: $appPath" }
New-Item -ItemType Directory -Force $EvidenceRoot | Out-Null
$runDir = Join-Path $EvidenceRoot ("run-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $runDir | Out-Null
$downloads = Join-Path $runDir 'downloads'
New-Item -ItemType Directory -Force $downloads | Out-Null
$database = Join-Path $runDir 'tasks.db'
$logPath = Join-Path $runDir 'smoke.log'
Set-Content -LiteralPath $logPath -Value '' -Encoding UTF8

function Log([string]$text) {
    $line = (Get-Date).ToString('HH:mm:ss.fff') + ' ' + $text
    Add-Content -LiteralPath $logPath -Value $line -Encoding UTF8
    Write-Host $line
}

Add-Type -Namespace Smoke -Name Native -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder text, int max);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
'@

function Read-WindowText([IntPtr]$handle) {
    $text = New-Object System.Text.StringBuilder 512
    [void][Smoke.Native]::GetWindowTextW($handle, $text, 512)
    return $text.ToString()
}

function Get-OwnerProcessId([IntPtr]$handle) {
    $ownerPid = 0
    [void][Smoke.Native]::GetWindowThreadProcessId($handle, [ref]$ownerPid)
    return $ownerPid
}

# Collects handles only. A callback that calls back into PowerShell and nests a
# second EnumWindows/EnumChildWindows call fails the session with
# SessionStateUnauthorizedAccessException; plain .NET calls are fine.
function Get-DialogHandles {
    param([IntPtr]$Parent, [string]$ClassPattern)
    $handles = New-Object System.Collections.ArrayList
    $callback = [Smoke.Native+EnumProc]{
        param([IntPtr]$handle, [IntPtr]$state)
        $class = New-Object System.Text.StringBuilder 128
        [void][Smoke.Native]::GetClassNameW($handle, $class, 128)
        if ($class.ToString() -like $ClassPattern) { [void]$handles.Add($handle) }
        return $true
    }
    if ($Parent -eq [IntPtr]::Zero) {
        [void][Smoke.Native]::EnumWindows($callback, [IntPtr]::Zero)
    } else {
        [void][Smoke.Native]::EnumChildWindows($Parent, $callback, [IntPtr]::Zero)
    }
    return $handles
}

# The main window belongs to this run's process: another instance may be on the
# desktop, and matching its window would report a launch that never happened.
function Find-AppWindow([int]$processId) {
    $script:appWindow = [IntPtr]::Zero
    $callback = [Smoke.Native+EnumProc]{
        param([IntPtr]$handle, [IntPtr]$state)
        $class = New-Object System.Text.StringBuilder 128
        [void][Smoke.Native]::GetClassNameW($handle, $class, 128)
        if ($class.ToString() -eq 'LiteFetchMainWindow') {
            $title = New-Object System.Text.StringBuilder 256
            [void][Smoke.Native]::GetWindowTextW($handle, $title, 256)
            $ownerPid = 0
            [void][Smoke.Native]::GetWindowThreadProcessId($handle, [ref]$ownerPid)
            if ($title.ToString() -eq 'lite fetch' -and $ownerPid -eq $processId) {
                $script:appWindow = $handle
                return $false
            }
        }
        return $true
    }
    [void][Smoke.Native]::EnumWindows($callback, [IntPtr]::Zero)
    return $script:appWindow
}

function Get-DialogSnapshot {
    $snapshot = @()
    foreach ($handle in (Get-DialogHandles -Parent ([IntPtr]::Zero) -ClassPattern '#32770*')) {
        $ownerPid = Get-OwnerProcessId $handle
        $owner = 'gone'
        try { $owner = (Get-Process -Id $ownerPid -ErrorAction Stop).ProcessName } catch { }
        $texts = @(Get-DialogHandles -Parent $handle -ClassPattern '*' |
            ForEach-Object { Read-WindowText $_ } |
            Where-Object { $_ } |
            Select-Object -First 4)
        $snapshot += ("#32770 | visible={0} | owner={1}({2}) | title='{3}' | {4}" -f `
            [Smoke.Native]::IsWindowVisible($handle), $owner, $ownerPid, (Read-WindowText $handle), ($texts -join ' / '))
    }
    return $snapshot
}

# Called while the app is still alive: "is the process still running" is part of
# the evidence, and the app's own dialog would disappear with it.
function Write-FailureEvidence([string]$reason) {
    Log "FAIL: $reason"
    Log ("exe=$appPath signature=" + (Get-AuthenticodeSignature $appPath).Status)
    if ($app) {
        Log ("app: pid=$($app.Id) hasExited=$($app.HasExited)" + $(if ($app.HasExited) { " exitCode=$($app.ExitCode)" } else { ' (still running)' }))
    } else {
        Log 'app: never started'
    }
    $dialogs = @(Get-DialogSnapshot)
    if ($dialogs.Count -eq 0) {
        Log 'dialogs: none'
    } else {
        foreach ($dialog in $dialogs) { Log "dialog: $dialog" }
    }
    $errorLog = Join-Path $runDir 'ui-error.log'
    if (Test-Path -LiteralPath $errorLog) {
        Log 'ui-error.log:'
        Get-Content -LiteralPath $errorLog -Tail 20 | ForEach-Object { Log "  $_" }
    }
    Log "evidence: $logPath"
}

$app = $null
$failure = $null
$windowAtMs = $null
$exitAtMs = $null
$watch = [Diagnostics.Stopwatch]::StartNew()
try {
    # CreateProcess, not the shell: the shell's file-trust check would block an
    # unsigned binary behind a confirmation dialog (see the header comment).
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $appPath
    $startInfo.Arguments = '--db "{0}" --download-dir "{1}"' -f $database, $downloads
    $startInfo.UseShellExecute = $false
    $startInfo.WorkingDirectory = $runDir
    Log ("launch: CreateProcess (UseShellExecute=false) args=" + $startInfo.Arguments)
    $app = [System.Diagnostics.Process]::Start($startInfo)
    Log "app: pid=$($app.Id)"

    $window = [IntPtr]::Zero
    if ($WindowTimeoutSeconds -gt 0) {
        $deadline = (Get-Date).AddSeconds($WindowTimeoutSeconds)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 100
            $window = Find-AppWindow $app.Id
            if ($window -ne [IntPtr]::Zero) { break }
        }
    }
    if ($window -eq [IntPtr]::Zero) {
        $failure = "main window did not appear within $WindowTimeoutSeconds s"
    } else {
        $windowAtMs = $watch.ElapsedMilliseconds
        Log "window: appeared after $windowAtMs ms"
        [void][Smoke.Native]::PostMessageW($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        Log 'window: posted WM_CLOSE'
        if (-not $app.WaitForExit($ExitTimeoutSeconds * 1000)) {
            $failure = "process did not exit within $ExitTimeoutSeconds s of WM_CLOSE"
        } else {
            $exitAtMs = $watch.ElapsedMilliseconds
            Log "process: exited after $exitAtMs ms with code $($app.ExitCode)"
            if ($app.ExitCode -ne 0) { $failure = "process exited with code $($app.ExitCode)" }
        }
    }
} catch {
    $failure = $_.Exception.Message
}

if ($failure) { Write-FailureEvidence $failure }
if ($app -and -not $app.HasExited) {
    Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
    Log 'cleanup: stopped the app process'
}

if ($failure) {
    Log 'exit: 1'
    exit 1
}
Log "PASS: window appeared after $windowAtMs ms and the process exited with code 0 after $exitAtMs ms"
Log "evidence: $logPath"
Log 'exit: 0'
exit 0
