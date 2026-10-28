# End-to-end Win32 acceptance run for Coh1e/litefetch #4.
#
# Seeds real completed/failed/queued tasks through the core CLI and controlled
# HTTP peer, then drives the real window: temporary search, hidden-selection
# safety, native multi-selection, numeric size sorting, column chooser and
# persistence across restart, batch add with per-line results, a real HTTP
# transfer started from the list with file hash, delete-record-keeps-file, and
# open eligibility failures.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts/win32-e2e-04.ps1
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $RepositoryRoot).Path
$build = Join-Path $root 'out/build/windows-core'
$appPath = Join-Path $build 'litefetch-win32.exe'
$cliPath = Join-Path $build 'litefetch-core-cli.exe'
$depsBin = Join-Path $root '.deps/installed-core/x64-windows/debug/bin'
$fixturePath = Join-Path $root 'scripts/core-http-fixture.mjs'
$evidence = Join-Path $root '.pi/evidence/issue-04'
$work = Join-Path $evidence 'e2e'
$log = Join-Path $evidence 'win32-e2e-04.log'
$env:PATH = "$depsBin;$env:PATH"
$env:TEMP = Join-Path $root '.deps/tmp'
$env:TMP = $env:TEMP
try {
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $OutputEncoding = [System.Text.Encoding]::UTF8
} catch {
}

$WM_COMMAND = 0x0111
$WM_CLOSE = 0x0010
$WM_SETTEXT = 0x000C
$WM_GETTEXT = 0x000D
$WM_GETTEXTLENGTH = 0x000E
$WM_KEYDOWN = 0x0100
$WM_LBUTTONDOWN = 0x0201
$WM_LBUTTONUP = 0x0202
$WM_MOUSEMOVE = 0x0200
$BM_CLICK = 0x00F5
$TDM_CLICK_BUTTON = 0x0400 + 102
$BM_SETCHECK = 0x00F1
$VK_ESCAPE = 0x1B
$VK_SPACE = 0x20
$IDOK = 1
$IDCANCEL = 2
$IDYES = 6

$ID_FILE_NEW = 1001
$ID_FILE_BATCH = 1003
$ID_EDIT_COPY_LINK = 1010
$ID_EDIT_FIND = 1012
$ID_VIEW_FILTER_ALL = 1021
$ID_VIEW_FILTER_FAILED = 1024
$ID_VIEW_COLUMNS = 1027
$ID_TASK_START = 1030
$ID_TASK_PAUSE = 1031
$ID_TASK_STOP = 1032
$ID_TASK_DELETE = 1033
$ID_TASK_OPEN_FILE = 1035
$ID_TASK_OPEN_FOLDER = 1034
$ID_TASK_DELETE_FILES = 1037
$IDC_URL = 2001
$IDC_SAVE_DIR = 2003
$IDC_AUTO_START = 2005
$IDC_COLUMN_LIST = 2102
$IDC_BATCH_URLS = 2110
$IDC_BATCH_LIST = 2111
$IDC_BATCH_DIR = 2112
$IDC_BATCH_ONLY_VALID = 2115
$IDC_BATCH_AUTO_START = 2116
$MF_GRAYED = 0x00000001
$TB_ISBUTTONENABLED = 0x0409
$SB_GETPARTS = 0x0406
$SB_GETTEXTLENGTHW = 0x0400 + 12
$SB_GETTEXTW = 0x0400 + 13
$LVM_FIRST = 0x1000
$LVM_GETITEMCOUNT = $LVM_FIRST + 4
$LVM_SETITEMSTATE = $LVM_FIRST + 43
$LVM_GETITEMTEXTW = $LVM_FIRST + 115
$LVM_GETCOLUMNWIDTH = $LVM_FIRST + 29
$HDM_GETITEMCOUNT = 0x1200
$LVIS_SELECTED = 0x0002
$LVIS_FOCUSED = 0x0001

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class E2E4 {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    public struct GUITHREADINFO {
        public uint cbSize;
        public uint flags;
        public IntPtr hwndActive;
        public IntPtr hwndFocus;
        public IntPtr hwndCapture;
        public IntPtr hwndMenuOwner;
        public IntPtr hwndMoveSize;
        public IntPtr hwndCaret;
        public RECT rcCaret;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct LVITEMW {
        public uint mask;
        public int iItem;
        public int iSubItem;
        public uint state;
        public uint stateMask;
        public IntPtr pszText;
        public int cchTextMax;
        public int iImage;
        public IntPtr lParam;
        public int iIndent;
        public int iGroupId;
        public uint cColumns;
        public IntPtr puColumns;
        public IntPtr piColFmt;
        public int iGroup;
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr FindWindowW(string className, string windowName);
    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr dialog, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")]
    public static extern IntPtr SendMessageString(IntPtr hWnd, uint message, IntPtr wParam, string lParam);
    [DllImport("user32.dll", EntryPoint="SendMessageW")]
    public static extern IntPtr SendMessagePtr(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", EntryPoint="SendMessageW")]
    public static extern IntPtr SendMessageInt(IntPtr hWnd, uint message, int wParam, int lParam);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern IntPtr GetMenu(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern int GetMenuItemCount(IntPtr hMenu);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetMenuStringW(IntPtr hMenu, uint item, StringBuilder text, int maxCount, uint flags);
    [DllImport("user32.dll")]
    public static extern uint GetMenuState(IntPtr hMenu, uint id, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder text, int maxCount);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr hWnd, StringBuilder text, int maxCount);
    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumProc callback, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr insertAfter, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool IsWindowEnabled(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern IntPtr GetFocus();
    [DllImport("user32.dll")]
    public static extern bool GetGUIThreadInfo(uint threadId, ref GUITHREADINFO info);
    [DllImport("user32.dll")]
    public static extern IntPtr SetFocus(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")]
    public static extern bool AttachThreadInput(uint attach, uint attachTo, bool attachFlag);
    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, UIntPtr size, uint allocationType, uint protect);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool VirtualFreeEx(IntPtr process, IntPtr address, UIntPtr size, uint freeType);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, UIntPtr size, out UIntPtr written);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, UIntPtr size, out UIntPtr read);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll")]
    public static extern bool OpenClipboard(IntPtr owner);
    [DllImport("user32.dll")]
    public static extern bool CloseClipboard();
    [DllImport("user32.dll")]
    public static extern IntPtr GetClipboardData(uint format);
    [DllImport("user32.dll")]
    public static extern bool IsClipboardFormatAvailable(uint format);
    [DllImport("kernel32.dll")]
    public static extern IntPtr GlobalLock(IntPtr handle);
    [DllImport("kernel32.dll")]
    public static extern bool GlobalUnlock(IntPtr handle);
    public static string ReadClipboardText() {
        for (int attempt = 0; attempt < 20; attempt++) {
            if (OpenClipboard(IntPtr.Zero)) {
                try {
                    if (IsClipboardFormatAvailable(13)) {
                        IntPtr data = GetClipboardData(13);
                        if (data != IntPtr.Zero) {
                            IntPtr pointer = GlobalLock(data);
                            if (pointer != IntPtr.Zero) {
                                try { return Marshal.PtrToStringUni(pointer); }
                                finally { GlobalUnlock(data); }
                            }
                        }
                    }
                } finally { CloseClipboard(); }
                return null;
            }
            System.Threading.Thread.Sleep(50);
        }
        return null;
    }
}
"@

New-Item -ItemType Directory -Force $work | Out-Null
New-Item -ItemType Directory -Force $evidence | Out-Null
$runRoot = Join-Path $work ('run-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $runRoot | Out-Null
$database = Join-Path $runRoot 'tasks.db'
$downloads = Join-Path $runRoot '下载目录'
New-Item -ItemType Directory -Force $downloads | Out-Null
$transcript = [System.Collections.Generic.List[string]]::new()
$tracePath = Join-Path $evidence 'win32-e2e-04-trace.log'
Set-Content -LiteralPath $tracePath -Value '' -Encoding UTF8
function Log([string]$text) {
    $line = (Get-Date).ToString('HH:mm:ss.fff') + ' ' + $text
    Write-Host $text
    Add-Content -LiteralPath $tracePath -Value $line -Encoding UTF8
    $transcript.Add($text) | Out-Null
}
function Fail([string]$text) {
    Log "FAIL: $text"
    throw $text
}
function Check([bool]$condition, [string]$text) {
    if ($condition) { Log "PASS: $text" } else { Fail $text }
}
function Wait-For([scriptblock]$condition, [int]$timeoutMs, [string]$label) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        if (& $condition) { return $true }
        Start-Sleep -Milliseconds 150
    }
    if (& $condition) { return $true }
    Fail "timed out waiting for $label"
}
function MakeLParam([int]$x, [int]$y) {
    return [int]((($x -band 0xFFFF)) -bor ((($y -band 0xFFFF)) -shl 16))
}
function Find-Child([IntPtr]$parent, [string]$className) {
    $script:childResult = [IntPtr]::Zero
    $callback = [E2E4+EnumProc]{
        param([IntPtr]$hWnd, [IntPtr]$lParam)
        $name = New-Object System.Text.StringBuilder 128
        [void][E2E4]::GetClassNameW($hWnd, $name, 128)
        if ($name.ToString() -eq $className) {
            $script:childResult = $hWnd
            return $false
        }
        return $true
    }
    [void][E2E4]::EnumChildWindows($parent, $callback, [IntPtr]::Zero)
    return $script:childResult
}
function Find-TopWindow([string]$className, [string]$titleFragment) {
    $script:topResult = [IntPtr]::Zero
    $callback = [E2E4+EnumProc]{
        param([IntPtr]$hWnd, [IntPtr]$lParam)
        if (-not [E2E4]::IsWindowVisible($hWnd)) { return $true }
        $name = New-Object System.Text.StringBuilder 128
        [void][E2E4]::GetClassNameW($hWnd, $name, 128)
        if ($name.ToString() -ne $className) { return $true }
        $title = New-Object System.Text.StringBuilder 512
        [void][E2E4]::GetWindowTextW($hWnd, $title, 512)
        if ($title.ToString().Contains($titleFragment)) {
            $script:topResult = $hWnd
            return $false
        }
        return $true
    }
    [void][E2E4]::EnumWindows($callback, [IntPtr]::Zero)
    return $script:topResult
}
function Focus-Control([IntPtr]$window, [IntPtr]$control) {
    $processId = 0
    $targetThread = [E2E4]::GetWindowThreadProcessId($window, [ref]$processId)
    $currentThread = [E2E4]::GetCurrentThreadId()
    $attached = [E2E4]::AttachThreadInput($currentThread, $targetThread, $true)
    try { [void][E2E4]::SetFocus($control) } finally {
        if ($attached) { [void][E2E4]::AttachThreadInput($currentThread, $targetThread, $false) }
    }
}
function Get-ClipboardText() {
    $value = [E2E4]::ReadClipboardText()
    if ($null -eq $value) { return '' }
    return $value.TrimEnd()
}
function Get-FocusedWindow([IntPtr]$window) {
    $processId = 0
    $threadId = [E2E4]::GetWindowThreadProcessId($window, [ref]$processId)
    $info = New-Object E2E4+GUITHREADINFO
    $info.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($info)
    if (-not [E2E4]::GetGUIThreadInfo($threadId, [ref]$info)) { return [IntPtr]::Zero }
    return $info.hwndFocus
}
function Window-Text([IntPtr]$handle) {
    $buffer = New-Object System.Text.StringBuilder 4096
    [void][E2E4]::GetWindowTextW($handle, $buffer, 4096)
    return $buffer.ToString()
}
function Capture-Window([IntPtr]$handle, [string]$file) {
    Log "capture start: $file"
    [void][E2E4]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 250
    $rect = New-Object E2E4+RECT
    if (-not [E2E4]::GetWindowRect($handle, [ref]$rect)) { Fail "GetWindowRect failed for $file" }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    try { $ok = [E2E4]::PrintWindow($handle, $hdc, 2) } finally { $graphics.ReleaseHdc($hdc) }
    $graphics.Dispose()
    $bitmap.Save((Join-Path $evidence $file), [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    Check $ok "captured $file ($width x $height)"
}
function Start-Fixture([int]$size, [int]$rate = 0) {
    $outFile = Join-Path $work ("fixture-{0}.json" -f ([guid]::NewGuid().ToString('N')))
    $errFile = "$outFile.err"
    $arguments = @($fixturePath, '0', "$size")
    if ($rate -gt 0) { $arguments += "$rate" }
    $process = Start-Process -FilePath (Get-Command node).Source -ArgumentList $arguments `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -NoNewWindow
    Wait-For -timeoutMs 15000 -label 'fixture start' -condition {
        (Test-Path $outFile) -and ((Get-Content $outFile -Raw) -match '"port"')
    } | Out-Null
    $info = Get-Content $outFile -Raw | ConvertFrom-Json
    return [pscustomobject]@{ Process = $process; Port = $info.port; Sha256 = $info.sha256; Size = $info.size }
}
function Stop-Fixture($fixture) {
    if ($fixture -and $fixture.Process -and -not $fixture.Process.HasExited) {
        Stop-Process -Id $fixture.Process.Id -Force -ErrorAction SilentlyContinue
        [void]$fixture.Process.WaitForExit(5000)
    }
}
function Start-App([string]$db, [string]$folder) {
    $process = Start-Process -FilePath $appPath -ArgumentList @('--db', $db, '--download-dir', $folder) -PassThru
    Log "app pid=$($process.Id) db=$db downloads=$folder"
    Wait-For -timeoutMs 20000 -label 'main window' -condition {
        [E2E4]::FindWindowW('LiteFetchMainWindow', 'lite fetch') -ne [IntPtr]::Zero
    } | Out-Null
    return $process
}
function Get-Tasks([string]$db) {
    $raw = & $cliPath list --db $db 2>$null
    if ($LASTEXITCODE -ne 0) { return @() }
    $parsed = $raw | ConvertFrom-Json
    return @($parsed.tasks)
}
function Close-App($process, [IntPtr]$window, [string]$label) {
    [void][E2E4]::PostMessageW($window, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $process.WaitForExit(30000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        Fail "${label}: process did not exit within 30 s"
    }
    $process.WaitForExit()
    Check ($process.ExitCode -eq 0) "${label}: process exited with code $($process.ExitCode)"
    Check (-not (Get-Process -Id $process.Id -ErrorAction SilentlyContinue)) "${label}: no background process remains"
}

$PROCESS_VM = 0x0008 -bor 0x0010 -bor 0x0020 -bor 0x0400
$MEM_COMMIT_RESERVE = 0x1000 -bor 0x2000
$PAGE_READWRITE = 0x04
$MEM_RELEASE = 0x8000
function Open-ProcessForWindow([IntPtr]$window) {
    $processId = 0
    [void][E2E4]::GetWindowThreadProcessId($window, [ref]$processId)
    $handle = [E2E4]::OpenProcess($PROCESS_VM, $false, $processId)
    if ($handle -eq [IntPtr]::Zero) { throw "OpenProcess failed for pid $processId" }
    return $handle
}
function Read-RemoteString([IntPtr]$process, [IntPtr]$address, [int]$maxChars) {
    $buffer = New-Object byte[] ($maxChars * 2)
    $read = [UIntPtr]::Zero
    if (-not [E2E4]::ReadProcessMemory($process, $address, $buffer, [System.UIntPtr]::new($buffer.Length), [ref]$read)) {
        throw 'ReadProcessMemory failed'
    }
    $length = 0
    while ($length -lt $maxChars -and $buffer[$length * 2] -ne 0) { $length += 1 }
    return [System.Text.Encoding]::Unicode.GetString($buffer, 0, $length * 2)
}
function Get-RemoteListRowsFor([IntPtr]$list, [int]$columns) {
    if ($list -eq [IntPtr]::Zero) { return @() }
    $process = Open-ProcessForWindow $list
    $remote = [E2E4]::VirtualAllocEx($process, [IntPtr]::Zero, [System.UIntPtr]::new(8192), $MEM_COMMIT_RESERVE, $PAGE_READWRITE)
    if ($remote -eq [IntPtr]::Zero) { [void][E2E4]::CloseHandle($process); throw 'VirtualAllocEx failed' }
    try {
        $count = [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
        $rows = @()
        for ($row = 0; $row -lt $count; $row += 1) {
            $texts = @()
            for ($column = 0; $column -lt $columns; $column += 1) {
                $item = New-Object E2E4+LVITEMW
                $item.mask = 0x1
                $item.iItem = $row
                $item.iSubItem = $column
                $item.pszText = [IntPtr]($remote.ToInt64() + 1024)
                $item.cchTextMax = 512
                $size = [System.Runtime.InteropServices.Marshal]::SizeOf($item)
                $local = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($size)
                $bytes = New-Object byte[] $size
                try {
                    [System.Runtime.InteropServices.Marshal]::StructureToPtr($item, $local, $false)
                    [System.Runtime.InteropServices.Marshal]::Copy($local, $bytes, 0, $size)
                } finally {
                    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($local)
                }
                $written = [UIntPtr]::Zero
                [void][E2E4]::WriteProcessMemory($process, $remote, $bytes, [System.UIntPtr]::new($size), [ref]$written)
                [void][E2E4]::SendMessagePtr($list, $LVM_GETITEMTEXTW, [IntPtr]$row, $remote)
                $texts += Read-RemoteString $process ([IntPtr]($remote.ToInt64() + 1024)) 512
            }
            $rows += ,$texts
        }
        return $rows
    } finally {
        [void][E2E4]::VirtualFreeEx($process, $remote, [UIntPtr]::Zero, $MEM_RELEASE)
        [void][E2E4]::CloseHandle($process)
    }
}
function Set-RemoteItemState([IntPtr]$list, [int]$row, [uint32]$state, [uint32]$stateMask) {
    $process = Open-ProcessForWindow $list
    $remote = [E2E4]::VirtualAllocEx($process, [IntPtr]::Zero, [System.UIntPtr]::new(4096), $MEM_COMMIT_RESERVE, $PAGE_READWRITE)
    if ($remote -eq [IntPtr]::Zero) { [void][E2E4]::CloseHandle($process); throw 'VirtualAllocEx failed' }
    try {
        $item = New-Object E2E4+LVITEMW
        $item.iItem = $row
        $item.state = $state
        $item.stateMask = $stateMask
        $size = [System.Runtime.InteropServices.Marshal]::SizeOf($item)
        $local = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($size)
        $bytes = New-Object byte[] $size
        try {
            [System.Runtime.InteropServices.Marshal]::StructureToPtr($item, $local, $false)
            [System.Runtime.InteropServices.Marshal]::Copy($local, $bytes, 0, $size)
        } finally {
            [System.Runtime.InteropServices.Marshal]::FreeHGlobal($local)
        }
        $written = [UIntPtr]::Zero
        [void][E2E4]::WriteProcessMemory($process, $remote, $bytes, [System.UIntPtr]::new($size), [ref]$written)
        [void][E2E4]::SendMessagePtr($list, $LVM_SETITEMSTATE, [IntPtr]$row, $remote)
    } finally {
        [void][E2E4]::VirtualFreeEx($process, $remote, [UIntPtr]::Zero, $MEM_RELEASE)
        [void][E2E4]::CloseHandle($process)
    }
}
function Get-RemoteStatusTexts([IntPtr]$window) {
    $status = Find-Child $window 'msctls_statusbar32'
    if ($status -eq [IntPtr]::Zero) { return @() }
    $process = Open-ProcessForWindow $window
    $remote = [E2E4]::VirtualAllocEx($process, [IntPtr]::Zero, [System.UIntPtr]::new(4096), $MEM_COMMIT_RESERVE, $PAGE_READWRITE)
    if ($remote -eq [IntPtr]::Zero) { [void][E2E4]::CloseHandle($process); throw 'VirtualAllocEx failed' }
    try {
        $texts = @()
        for ($part = 0; $part -lt 4; $part += 1) {
            [void][E2E4]::SendMessagePtr($status, $SB_GETTEXTW, [IntPtr]$part, $remote)
            $texts += Read-RemoteString $process $remote 512
        }
        return $texts
    } finally {
        [void][E2E4]::VirtualFreeEx($process, $remote, [UIntPtr]::Zero, $MEM_RELEASE)
        [void][E2E4]::CloseHandle($process)
    }
}
function Get-HeaderColumnCount([IntPtr]$list) {
    $header = Find-Child $list 'SysHeader32'
    if ($header -eq [IntPtr]::Zero) { return 0 }
    return [int][E2E4]::SendMessagePtr($header, $HDM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Get-ColumnWidths([IntPtr]$list) {
    $count = Get-HeaderColumnCount $list
    $widths = @()
    for ($index = 0; $index -lt $count; $index += 1) {
        $widths += [int][E2E4]::SendMessagePtr($list, $LVM_GETCOLUMNWIDTH, [IntPtr]$index, [IntPtr]::Zero)
    }
    return $widths
}
function Click-HeaderColumn([IntPtr]$list, [int]$index) {
    $header = Find-Child $list 'SysHeader32'
    if ($header -eq [IntPtr]::Zero) { Fail 'header control missing' }
    $rect = New-Object E2E4+RECT
    [void][E2E4]::GetClientRect($header, [ref]$rect)
    $widths = @(Get-ColumnWidths $list)
    $x = 4
    for ($column = 0; $column -lt $index; $column += 1) { $x += $widths[$column] }
    $x += [int]($widths[$index] / 2)
    $y = [int](($rect.Bottom - $rect.Top) / 2)
    [void][E2E4]::SendMessageInt($header, $WM_LBUTTONDOWN, 1, (MakeLParam $x $y))
    [void][E2E4]::SendMessageInt($header, $WM_MOUSEMOVE, 1, (MakeLParam ($x + 2) $y))
    [void][E2E4]::SendMessageInt($header, $WM_LBUTTONUP, 0, (MakeLParam ($x + 2) $y))
}
function Select-Only-Row([IntPtr]$list, [int]$row) {
    Set-RemoteItemState $list -1 0 ($LVIS_SELECTED -bor $LVIS_FOCUSED)
    Set-RemoteItemState $list $row ($LVIS_SELECTED -bor $LVIS_FOCUSED) ($LVIS_SELECTED -bor $LVIS_FOCUSED)
    Start-Sleep -Milliseconds 300
}
function Find-RowIndex([IntPtr]$list, [string]$name, [int]$columns = 1) {
    $rows = @(Get-RemoteListRowsFor $list $columns)
    for ($index = 0; $index -lt $rows.Count; $index += 1) {
        if ($rows[$index][0] -eq $name) { return $index }
    }
    return -1
}
function Click-ControlMouse([IntPtr]$dialog, [IntPtr]$control) {
    $rect = New-Object E2E4+RECT
    if (-not [E2E4]::GetWindowRect($control, [ref]$rect)) { Fail 'control rect unavailable' }
    [void][E2E4]::SetForegroundWindow($dialog)
    Start-Sleep -Milliseconds 200
    [void][E2E4]::SetCursorPos([int](($rect.Left + $rect.Right) / 2), [int](($rect.Top + $rect.Bottom) / 2))
    Start-Sleep -Milliseconds 100
    [E2E4]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [E2E4]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
}
function Click-DialogButton([IntPtr]$dialog, [string]$fragment) {
    $script:clickedButton = [IntPtr]::Zero
    $callback = [E2E4+EnumProc]{
        param([IntPtr]$hWnd, [IntPtr]$lParam)
        $name = New-Object System.Text.StringBuilder 128
        [void][E2E4]::GetClassNameW($hWnd, $name, 128)
        if ($name.ToString() -eq 'Button') {
            $text = Window-Text $hWnd
            if ($text.Contains($fragment)) {
                $script:clickedButton = $hWnd
                [void][E2E4]::SendMessageInt($hWnd, $BM_CLICK, 0, 0)
                return $false
            }
        }
        return $true
    }
    [void][E2E4]::EnumChildWindows($dialog, $callback, [IntPtr]::Zero)
    if ($script:clickedButton -eq [IntPtr]::Zero) { Fail "dialog button containing '$fragment' not found" }
}
function Get-DialogTexts([IntPtr]$dialog) {
    $texts = @(Window-Text $dialog)
    $callback = [E2E4+EnumProc]{
        param([IntPtr]$hWnd, [IntPtr]$lParam)
        $name = New-Object System.Text.StringBuilder 128
        [void][E2E4]::GetClassNameW($hWnd, $name, 128)
        if ($name.ToString() -eq 'Static' -or $name.ToString() -eq 'Button') {
            $text = Window-Text $hWnd
            if ($text.Length -gt 0) { $script:dialogTexts += $text }
        }
        return $true
    }
    $script:dialogTexts = @()
    [void][E2E4]::EnumChildWindows($dialog, $callback, [IntPtr]::Zero)
    return $texts + $script:dialogTexts
}
function Set-DialogText([IntPtr]$dialog, [int]$id, [string]$text) {
    $control = [E2E4]::GetDlgItem($dialog, $id)
    if ($control -eq [IntPtr]::Zero) { Fail "dialog control $id not found" }
    [void][E2E4]::SendMessageString($control, $WM_SETTEXT, [IntPtr]::Zero, $text)
}
function Notify-EditChange([IntPtr]$parent, [int]$id, [IntPtr]$control) {
    [void][E2E4]::SendMessagePtr($parent, $WM_COMMAND, [IntPtr]($id -bor (0x0300 -shl 16)), $control)
}
function Open-NewDownloadDialog([IntPtr]$window) {
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_FILE_NEW, [IntPtr]::Zero)
    Wait-For -timeoutMs 10000 -label 'new download dialog' -condition {
        $candidate = [E2E4]::FindWindowW('#32770', '新建下载')
        return ($candidate -ne [IntPtr]::Zero) -and
            ([E2E4]::GetDlgItem($candidate, $IDC_URL) -ne [IntPtr]::Zero) -and
            ([E2E4]::GetDlgItem($candidate, $IDOK) -ne [IntPtr]::Zero)
    } | Out-Null
    Start-Sleep -Milliseconds 400
    return [E2E4]::FindWindowW('#32770', '新建下载')
}
function Open-BatchDialog([IntPtr]$window) {
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_FILE_BATCH, [IntPtr]::Zero)
    Wait-For -timeoutMs 10000 -label 'batch dialog' -condition {
        $candidate = [E2E4]::FindWindowW('#32770', '批量添加')
        if ($candidate -eq [IntPtr]::Zero) { return $false }
        return ([E2E4]::GetDlgItem($candidate, $IDC_BATCH_URLS) -ne [IntPtr]::Zero) -and
            ((Find-Child $candidate 'SysListView32') -ne [IntPtr]::Zero)
    } | Out-Null
    return [E2E4]::FindWindowW('#32770', '批量添加')
}
function Wait-DialogClosed([IntPtr]$handle, [string]$label) {
    Wait-For -timeoutMs 15000 -label $label -condition {
        -not [E2E4]::IsWindowVisible($handle)
    } | Out-Null
}

function Seed-Tasks {
    $specs = @(
        @{ Name = 'zzz-small.bin'; Size = 2048 },
        @{ Name = 'mmm-mid.bin'; Size = 921600 },
        @{ Name = 'aaa-large.bin'; Size = 1048576 }
    )
    $hashes = @{}
    foreach ($spec in $specs) {
        $fixture = Start-Fixture -size $spec.Size
        try {
            $destination = Join-Path $downloads $spec.Name
            $source = "http://127.0.0.1:$($fixture.Port)/$($spec.Name)"
            $add = & $cliPath add --db $database --source $source --destination $destination | ConvertFrom-Json
            if ($LASTEXITCODE -ne 0 -or -not $add.ok) { Fail "seed add failed for $($spec.Name)" }
            $started = & $cliPath start --db $database --id $add.task.id | ConvertFrom-Json
            if ($LASTEXITCODE -ne 0 -or $started.task.state -ne 'completed') {
                Fail "seed start failed for $($spec.Name): $($started.task.state) $($started.error)"
            }
            $hashes[$spec.Name] = $fixture.Sha256
        } finally {
            Stop-Fixture $fixture
        }
    }
    $missing = Start-Fixture -size 1024
    try {
        $destination = Join-Path $downloads 'failed.bin'
        $source = "http://127.0.0.1:$($missing.Port)/missing"
        $add = & $cliPath add --db $database --source $source --destination $destination | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0 -or -not $add.ok) { Fail 'failed-task seed add failed' }
        $started = & $cliPath start --db $database --id $add.task.id | ConvertFrom-Json
        if ($started.task.state -ne 'failed') { Fail "404 seed did not fail: $($started.task.state)" }
    } finally {
        Stop-Fixture $missing
    }
    $queued = & $cliPath add --db $database --source 'http://127.0.0.1:1/queued.bin' `
        --destination (Join-Path $downloads 'queued.bin') | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or -not $queued.ok) { Fail 'queued seed add failed' }
    $missingDir = Join-Path $runRoot 'missing-dir/nofile.bin'
    $noParent = & $cliPath add --db $database --source 'http://127.0.0.1:1/nofile.bin' `
        --destination $missingDir | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or -not $noParent.ok) { Fail 'missing-dir seed add failed' }
    $cred = & $cliPath add --db $database --source 'http://user:secret@example.com/cred.bin' `
        --destination (Join-Path $downloads 'cred.bin') | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or -not $cred.ok) { Fail 'credential seed add failed' }
    return $hashes
}

$app = $null
$batchFixture = $null
try {
    $hashes = Seed-Tasks
    $tasks = @(Get-Tasks $database)
    Check ($tasks.Count -eq 7) "seeded database has seven real tasks (got $($tasks.Count))"
    $app = Start-App $database $downloads
    $window = [E2E4]::FindWindowW('LiteFetchMainWindow', 'lite fetch')
    $menu = [E2E4]::GetMenu($window)
    $list = Find-Child $window 'SysListView32'
    $status = Find-Child $window 'msctls_statusbar32'
    $toolbar = Find-Child $window 'ToolbarWindow32'
    Check ($list -ne [IntPtr]::Zero) 'main ListView exists'
    Check ((Get-HeaderColumnCount $list) -eq 6) 'six default columns'
    Wait-For -timeoutMs 8000 -label 'seven rows' -condition {
        [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 7
    } | Out-Null
    $rows = @(Get-RemoteListRowsFor $list 6)
    Log ("seed rows: " + (($rows | ForEach-Object { $_[0] }) -join ', '))
    Check ($rows.Count -eq 7) 'seven seeded rows visible'

    # --- temporary search overlay -------------------------------------------------
    $search = Find-Child $window 'Edit'
    Check ($search -ne [IntPtr]::Zero) 'temporary search edit exists'
    Check (-not [E2E4]::IsWindowVisible($search)) 'search overlay starts hidden'
    $listRectBefore = New-Object E2E4+RECT
    [void][E2E4]::GetClientRect($list, [ref]$listRectBefore)
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_EDIT_FIND, [IntPtr]::Zero)
    Wait-For -timeoutMs 3000 -label 'search overlay visible' -condition { [E2E4]::IsWindowVisible($search) } | Out-Null
    $listRectAfter = New-Object E2E4+RECT
    [void][E2E4]::GetClientRect($list, [ref]$listRectAfter)
    Check ($listRectAfter.Right -eq $listRectBefore.Right -and $listRectAfter.Bottom -eq $listRectBefore.Bottom) `
        'search overlay does not resize the ListView'
    $searchRect = New-Object E2E4+RECT
    [void][E2E4]::GetWindowRect($search, [ref]$searchRect)
    $listScreen = New-Object E2E4+RECT
    [void][E2E4]::GetWindowRect($list, [ref]$listScreen)
    $header = Find-Child $list 'SysHeader32'
    $headerRect = New-Object E2E4+RECT
    [void][E2E4]::GetWindowRect($header, [ref]$headerRect)
    Check ($searchRect.Top -ge ($listScreen.Top + ($headerRect.Bottom - $headerRect.Top))) `
        'search overlay is below the list header'
    Check ($searchRect.Right -le $listScreen.Right -and $searchRect.Left -gt ($listScreen.Left + (($listScreen.Right - $listScreen.Left) / 2))) `
        'search overlay is anchored at the upper-right of the list content'
    Check ((Get-FocusedWindow $window) -eq $search) 'Ctrl+F/Find focuses the search field'
    Set-DialogText $window 2100 'large'
    Notify-EditChange $window 2100 $search
    Wait-For -timeoutMs 3000 -label 'search filter result' -condition {
        [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1
    } | Out-Null
    $statusTexts = @(Get-RemoteStatusTexts $window)
    Check ($statusTexts[0] -eq '可见: 1 / 7') "search status shows visible / total ($($statusTexts[0]))"
    Capture-Window $window '04-search-overlay.png'
    [void][E2E4]::SendMessageInt($search, $WM_KEYDOWN, $VK_ESCAPE, 0)
    Wait-For -timeoutMs 3000 -label 'search hidden after Esc' -condition {
        -not [E2E4]::IsWindowVisible($search) -and
        [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 7
    } | Out-Null
    Check ((Get-FocusedWindow $window) -eq $list) 'Esc restores list focus'
    Log 'search overlay checked'

    # --- hidden selection is not acted on (non-modal Stop) -----------------------
    $queuedIndex = Find-RowIndex $list 'queued.bin' 6
    $nofileIndex = Find-RowIndex $list 'nofile.bin' 6
    Check ($queuedIndex -ge 0 -and $nofileIndex -ge 0) 'selected rows found by stable name'
    Select-Only-Row $list $queuedIndex
    Set-RemoteItemState $list $nofileIndex $LVIS_SELECTED $LVIS_SELECTED
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_EDIT_FIND, [IntPtr]::Zero)
    Wait-For -timeoutMs 3000 -label 'search reopened' -condition { [E2E4]::IsWindowVisible($search) } | Out-Null
    Set-DialogText $window 2100 'queued.bin'
    Notify-EditChange $window 2100 $search
    Wait-For -timeoutMs 3000 -label 'hidden nofile row' -condition {
        [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1
    } | Out-Null
    Select-Only-Row $list 0
    Focus-Control $window $list
    Start-Sleep -Milliseconds 300
    Check ((Get-FocusedWindow $window) -eq $list) 'moving focus to the filtered list succeeds'
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_TASK_STOP, [IntPtr]::Zero)
    Wait-For -timeoutMs 8000 -label 'visible queued task stopped' -condition {
        $tasks = @(Get-Tasks $database)
        @($tasks | Where-Object { (Split-Path -Leaf $_.destination) -eq 'queued.bin' -and $_.state -eq 'cancelled' }).Count -eq 1
    } | Out-Null
    $tasksAfterStop = @(Get-Tasks $database)
    Check (@($tasksAfterStop | Where-Object { (Split-Path -Leaf $_.destination) -eq 'nofile.bin' -and $_.state -eq 'queued' }).Count -eq 1) 'hidden selected task stayed queued and was not stopped'
    [void][E2E4]::SendMessageInt($list, $WM_KEYDOWN, $VK_ESCAPE, 0)
    Wait-For -timeoutMs 3000 -label 'search hidden after list Esc' -condition {
        -not [E2E4]::IsWindowVisible($search) -and
        [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 7
    } | Out-Null

    # --- numeric size sort (not formatted strings) --------------------------------
    Click-HeaderColumn $list 1
    Start-Sleep -Milliseconds 500
    $rows = @(Get-RemoteListRowsFor $list 6)
    $firstNames = @($rows[0][0], $rows[1][0], $rows[2][0])
    Log ("size ascending first rows: " + ($firstNames -join ', '))
    Check ((($firstNames -join ',') -eq 'zzz-small.bin,mmm-mid.bin,aaa-large.bin')) `
        'size ascending orders 2 KB, 900 KB, 1 MB numerically'
    Click-HeaderColumn $list 1
    Start-Sleep -Milliseconds 500
    $rows = @(Get-RemoteListRowsFor $list 6)
    $firstNames = @($rows[0][0], $rows[1][0], $rows[2][0])
    Log ("size descending first rows: " + ($firstNames -join ', '))
    Check ((($firstNames -join ',') -eq 'aaa-large.bin,mmm-mid.bin,zzz-small.bin')) `
        'size descending reverses the numeric order'
    $lastNames = @($rows[3][0], $rows[4][0], $rows[5][0])
    Log ("unknown sizes remain after known: " + ($lastNames -join ', '))
    Check (($lastNames -notcontains 'zzz-small.bin') -and ($lastNames -notcontains 'mmm-mid.bin') -and ($lastNames -notcontains 'aaa-large.bin')) `
        'unknown values stay after known values in both directions'
    Capture-Window $window '04-sorted-size.png'

    # --- column chooser, width save, sort/visibility persistence ------------------
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_VIEW_COLUMNS, [IntPtr]::Zero)
    Wait-For -timeoutMs 10000 -label 'column chooser' -condition {
        $candidate = [E2E4]::FindWindowW('#32770', '选择列表列')
        if ($candidate -eq [IntPtr]::Zero) { return $false }
        return (Find-Child $candidate 'SysListView32') -ne [IntPtr]::Zero
    } | Out-Null
    $columnDialog = [E2E4]::FindWindowW('#32770', '选择列表列')
    $columnList = Find-Child $columnDialog 'SysListView32'
    $columnRows = @(Get-RemoteListRowsFor $columnList 2)
    Check ($columnRows.Count -eq 16) "column chooser lists all 16 known columns (got $($columnRows.Count))"
    $remainingIndex = -1
    for ($index = 0; $index -lt $columnRows.Count; $index += 1) {
        if ($columnRows[$index][0] -eq '剩余时间') { $remainingIndex = $index }
    }
    Check ($remainingIndex -ge 0) 'remaining-time column is in the chooser'
    Set-RemoteItemState $columnList $remainingIndex 0x1000 0xF000 # unchecked state image
    [void][E2E4]::PostMessageW($columnDialog, $WM_COMMAND, [IntPtr]$IDOK, [E2E4]::GetDlgItem($columnDialog, $IDOK))
    Wait-For -timeoutMs 5000 -label 'column chooser closed' -condition {
        -not [E2E4]::IsWindowVisible($columnDialog)
    } | Out-Null
    Wait-For -timeoutMs 5000 -label 'five columns after hide' -condition {
        (Get-HeaderColumnCount $list) -eq 5
    } | Out-Null
    Check ((Get-HeaderColumnCount $list) -eq 5) 'hiding 剩余时间 removes exactly one column'
    $widths = @(Get-ColumnWidths $list)
    $header = Find-Child $list 'SysHeader32'
    $headerRect = New-Object E2E4+RECT
    [void][E2E4]::GetClientRect($header, [ref]$headerRect)
    $startRect = New-Object E2E4+RECT
    [void][E2E4]::GetWindowRect($header, [ref]$startRect)
    $dragY = [int](($headerRect.Bottom - $headerRect.Top) / 2)
    $dividerX = $widths[0]
    [void][E2E4]::SendMessageInt($header, $WM_LBUTTONDOWN, 1, (MakeLParam $dividerX $dragY))
    [void][E2E4]::SendMessageInt($header, $WM_MOUSEMOVE, 1, (MakeLParam ($dividerX + 40) $dragY))
    [void][E2E4]::SendMessageInt($header, $WM_LBUTTONUP, 0, (MakeLParam ($dividerX + 40) $dragY))
    Start-Sleep -Milliseconds 500
    $widthsAfterDrag = @(Get-ColumnWidths $list)
    Log ("name width before/after drag: $($widths[0]) / $($widthsAfterDrag[0])")
    Check ($widthsAfterDrag[0] -gt $widths[0]) 'dragging the header divider widens 名称'
    Capture-Window $window '04-columns.png'

    $stateFile = Join-Path $runRoot 'ui-state.ini'
    Check (Test-Path -LiteralPath $stateFile) 'column/sort state file created beside the database'
    $stateText = Get-Content -LiteralPath $stateFile -Raw
    Check ($stateText -match 'remaining:0') 'hidden column persisted'
    Check ($stateText -match 'sort=size:desc') 'sort column/direction persisted'

    # Restart: visibility, width and sort must survive real process restart.
    Close-App $app $window 'pre-restart'
    $app = Start-App $database $downloads
    $window = [E2E4]::FindWindowW('LiteFetchMainWindow', 'lite fetch')
    $menu = [E2E4]::GetMenu($window)
    $list = Find-Child $window 'SysListView32'
    $status = Find-Child $window 'msctls_statusbar32'
    $toolbar = Find-Child $window 'ToolbarWindow32'
    $search = Find-Child $window 'Edit'
    Wait-For -timeoutMs 8000 -label 'rows after restart' -condition {
        [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 7
    } | Out-Null
    Check ((Get-HeaderColumnCount $list) -eq 5) 'hidden column stays hidden after restart'
    $restartWidths = @(Get-ColumnWidths $list)
    $listRect = New-Object E2E4+RECT
    [void][E2E4]::GetClientRect($list, [ref]$listRect)
    $clientWidth = [int]($listRect.Right - $listRect.Left)
    $dpi = [int][E2E4]::GetDpiForWindow($window)
    if ($dpi -le 0) { $dpi = 96 }
    $savedDips = [int][Math]::Round($widthsAfterDrag[0] * 96 / $dpi)
    Log ("restart name width: $($restartWidths[0]) (client $clientWidth, dpi $dpi, saved DIP $savedDips)")
    Check ($stateText -match ("name:1:" + $savedDips)) 'dragged width persisted in DIP'
    # The list always fills its client width and shrinks every column
    # proportionally when the preferred total exceeds it, so the restored pitch
    # is that layout - not the raw drag pixels. What must survive the restart is
    # the persisted DIP width and a visibly wider name column than the default.
    Check ($restartWidths[0] -gt $widths[0]) 'widened name column survives restart'
    Check ((($restartWidths | Measure-Object -Sum).Sum) -eq $clientWidth) `
        'restored columns fill the list client width'
    $restartRows = @(Get-RemoteListRowsFor $list 5)
    $restartFirst = @($restartRows[0][0], $restartRows[1][0], $restartRows[2][0])
    Check ((($restartFirst -join ',') -eq 'aaa-large.bin,mmm-mid.bin,zzz-small.bin')) `
        'sort direction persists across restart'
    Capture-Window $window '04-restart.png'

    # --- batch add with per-line results -----------------------------------------
    $batchFailFile = Join-Path $downloads 'batch-fail.bin'
    Set-Content -LiteralPath $batchFailFile -Value 'keep-this-file' -Encoding UTF8
    $batchFailHash = (Get-FileHash -LiteralPath $batchFailFile -Algorithm SHA256).Hash
    $batchFixture = Start-Fixture -size 4096
    $batchDialog = Open-BatchDialog $window
    $batchEdit = [E2E4]::GetDlgItem($batchDialog, $IDC_BATCH_URLS)
    $batchList = Find-Child $batchDialog 'SysListView32'
    Check ($batchEdit -ne [IntPtr]::Zero -and $batchList -ne [IntPtr]::Zero) 'batch dialog has address edit and result list'
    $batchText = @"
http://127.0.0.1:$($batchFixture.Port)/batch-fail.bin
http://127.0.0.1:$($batchFixture.Port)/batch-new.bin
https://example.org/batch-https.bin
magnet:?xt=urn:btih:0123456789012345678901234567890123456789
ftp://ftp.example.org/archive.zip
htp://example.org/error.zip
"@
    Set-DialogText $batchDialog $IDC_BATCH_URLS $batchText
    Notify-EditChange $batchDialog $IDC_BATCH_URLS $batchEdit
    Wait-For -timeoutMs 4000 -label 'batch validation rows' -condition {
        [int][E2E4]::SendMessagePtr($batchList, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 6
    } | Out-Null
    $batchRows = @(Get-RemoteListRowsFor $batchList 3)
    Log ("batch validation: " + (($batchRows | ForEach-Object { $_[1] + '=' + $_[2] }) -join ' | '))
    Check ($batchRows[0][1] -eq 'HTTP' -and $batchRows[0][2] -eq '有效') 'line 1 HTTP valid'
    Check ($batchRows[1][1] -eq 'HTTP' -and $batchRows[1][2] -eq '有效') 'line 2 HTTP valid'
    Check ($batchRows[2][1] -eq 'HTTPS' -and $batchRows[2][2] -match '尚未验收') 'line 3 HTTPS valid with honest transfer boundary'
    Check ($batchRows[3][1] -eq 'BitTorrent' -and $batchRows[3][2] -match '暂不支持') 'magnet unsupported, not silently accepted'
    Check ($batchRows[4][1] -eq 'FTP' -and $batchRows[4][2] -match '暂不支持') 'FTP unsupported, not silently accepted'
    Check ($batchRows[5][1] -eq [char]0x2014 -and $batchRows[5][2] -match '未知地址类型') 'typo scheme invalid with reason'
    $addButton = [E2E4]::GetDlgItem($batchDialog, $IDOK)
    Check (-not [E2E4]::IsWindowEnabled($addButton)) 'Add is disabled while partial input has no explicit only-valid choice'
    [void][E2E4]::SendMessageInt([E2E4]::GetDlgItem($batchDialog, $IDC_BATCH_AUTO_START), $BM_CLICK, 0, 0)
    [void][E2E4]::SendMessageInt([E2E4]::GetDlgItem($batchDialog, $IDC_BATCH_ONLY_VALID), $BM_CLICK, 0, 0)
    Start-Sleep -Milliseconds 300
    Check ([E2E4]::IsWindowEnabled($addButton)) 'Add enables after choosing only valid links'
    Capture-Window $batchDialog '04-batch-add.png'
    [void][E2E4]::PostMessageW($batchDialog, $WM_COMMAND, [IntPtr]$IDOK, $addButton)
    Wait-For -timeoutMs 15000 -label 'batch result dialog' -condition {
        [E2E4]::FindWindowW('#32770', '批量添加结果') -ne [IntPtr]::Zero
    } | Out-Null
    $resultDialog = [E2E4]::FindWindowW('#32770', '批量添加结果')
    $resultList = Find-Child $resultDialog 'SysListView32'
    Wait-For -timeoutMs 3000 -label 'batch result rows' -condition {
        [int][E2E4]::SendMessagePtr($resultList, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 6
    } | Out-Null
    $resultRows = @(Get-RemoteListRowsFor $resultList 3)
    $resultJoined = ($resultRows | ForEach-Object { $_ -join ' | ' }) -join "`n"
    Log ("batch result: " + $resultJoined)
    Check ($resultJoined -match '提交失败') 'batch result reports the per-line commit failure'
    Check ($resultJoined -match '未提交') 'batch result reports invalid/unsupported lines as not submitted'
    Capture-Window $resultDialog '04-batch-result.png'
    [void][E2E4]::PostMessageW($resultDialog, $WM_COMMAND, [IntPtr]$IDOK, [E2E4]::GetDlgItem($resultDialog, $IDOK))
    Wait-DialogClosed $resultDialog 'batch result closed'
    Wait-For -timeoutMs 8000 -label 'batch rows in main list' -condition {
        [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 9
    } | Out-Null
    $tasksAfterBatch = @(Get-Tasks $database)
    Check ($tasksAfterBatch.Count -eq 9) "batch added exactly the two accepted lines (got $($tasksAfterBatch.Count))"
    $batchNames = @($tasksAfterBatch | ForEach-Object { Split-Path -Leaf $_.destination })
    Check ($batchNames -contains 'batch-new.bin') 'batch-new.bin record created'
    Check ($batchNames -contains 'batch-https.bin') 'HTTPS record created but not started'
    Check ($batchNames -notcontains 'batch-fail.bin') 'pre-existing target line was rejected, not silently overwritten'
    Check ($batchNames -notcontains 'archive.zip') 'FTP line was not created'
    Check (([System.IO.File]::ReadAllText($batchFailFile)).Trim() -eq 'keep-this-file') 'pre-existing target file content preserved'
    Check ((Get-FileHash -LiteralPath $batchFailFile -Algorithm SHA256).Hash -eq $batchFailHash) 'pre-existing target file hash preserved'

    # --- real HTTP transfer started from the list, then file hash ----------------
    $batchIndex = Find-RowIndex $list 'batch-new.bin' 5
    Check ($batchIndex -ge 0) 'batch-added HTTP task is visible before start'
    Select-Only-Row $list $batchIndex
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_TASK_START, [IntPtr]::Zero)
    Wait-For -timeoutMs 30000 -label 'list-started HTTP download completed' -condition {
        $currentRows = @(Get-RemoteListRowsFor $list 5)
        @($currentRows | Where-Object { $_[0] -eq 'batch-new.bin' -and $_[4] -eq '已完成' }).Count -eq 1
    } | Out-Null
    $batchPath = Join-Path $downloads 'batch-new.bin'
    Check (Test-Path -LiteralPath $batchPath) 'list-started HTTP download created the file'
    $batchHash = (Get-FileHash -LiteralPath $batchPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Check ($batchHash -eq $batchFixture.Sha256) "list-started HTTP SHA-256 matches the peer ($batchHash)"

    # --- delete record keeps the file --------------------------------------------
    $smallIndex = Find-RowIndex $list 'zzz-small.bin' 5
    Check ($smallIndex -ge 0) 'completed small task row is present before delete'
    Select-Only-Row $list $smallIndex
    $beforeDelete = @(Get-Tasks $database).Count
    Log ("list count at delete: " + [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero))
    Log ("status at delete: " + ((@(Get-RemoteStatusTexts $window)) -join ' | '))
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_TASK_DELETE, [IntPtr]::Zero)
    Wait-For -timeoutMs 5000 -label 'delete confirmation' -condition {
        [E2E4]::FindWindowW('#32770', '删除任务') -ne [IntPtr]::Zero
    } | Out-Null
    $deleteDialog = [E2E4]::FindWindowW('#32770', '删除任务')
    $deleteTexts = @(Get-DialogTexts $deleteDialog)
    $deleteJoined = $deleteTexts -join "`n"
    Log ("delete confirmation: " + ($deleteTexts -join ' | '))
    Check ($deleteJoined -match '1 个任务') 'delete confirmation states the exact selected count'
    Capture-Window $deleteDialog '04-delete-confirm.png'
    [void][E2E4]::PostMessageW($deleteDialog, $WM_COMMAND, [IntPtr]$IDOK, [E2E4]::GetDlgItem($deleteDialog, $IDOK))
    Wait-DialogClosed $deleteDialog 'delete confirmation closed'
    Start-Sleep -Milliseconds 1000
    $afterDeleteStatus = @(Get-RemoteStatusTexts $window)
    Log ("after delete confirmation status: " + ($afterDeleteStatus -join ' | '))
    Log ("after delete confirmation DB count: " + (@(Get-Tasks $database).Count))
    $deleteErrorDialog = [E2E4]::FindWindowW('#32770', '删除任务记录失败')
    if ($deleteErrorDialog -ne [IntPtr]::Zero) {
        $deleteErrorTexts = @(Get-DialogTexts $deleteErrorDialog) -join "`n"
        Log ("delete error dialog: " + $deleteErrorTexts)
        Click-DialogButton $deleteErrorDialog '确定'
    }
    Wait-For -timeoutMs 8000 -label 'record removed after delete' -condition {
        @(Get-Tasks $database).Count -eq ($beforeDelete - 1)
    } | Out-Null
    Check ((@(Get-Tasks $database).Count) -eq ($beforeDelete - 1)) 'delete removed exactly one record'
    Check (Test-Path -LiteralPath (Join-Path $downloads 'zzz-small.bin')) 'delete record kept the file'
    $smallHash = (Get-FileHash -LiteralPath (Join-Path $downloads 'zzz-small.bin') -Algorithm SHA256).Hash.ToLowerInvariant()
    Check ($smallHash -eq $hashes['zzz-small.bin']) 'delete record did not alter the file hash'

    # --- open eligibility and failure reason --------------------------------------
    $singleIndex = Find-RowIndex $list 'batch-new.bin' 5
    Select-Only-Row $list $singleIndex
    $openFileState = [E2E4]::GetMenuState($menu, $ID_TASK_OPEN_FILE, 0)
    $openFolderState = [E2E4]::GetMenuState($menu, $ID_TASK_OPEN_FOLDER, 0)
    Check (($openFileState -band $MF_GRAYED) -eq 0) 'open file is enabled for a completed existing file'
    Check (($openFolderState -band $MF_GRAYED) -eq 0) 'open folder is enabled for a completed existing file'
    $queuedIndex = Find-RowIndex $list 'queued.bin' 5
    Select-Only-Row $list $queuedIndex
    $openFileState = [E2E4]::GetMenuState($menu, $ID_TASK_OPEN_FILE, 0)
    $openFolderState = [E2E4]::GetMenuState($menu, $ID_TASK_OPEN_FOLDER, 0)
    Check (($openFileState -band $MF_GRAYED) -ne 0) 'open file is disabled for an incomplete task'
    Check (($openFolderState -band $MF_GRAYED) -eq 0) 'open folder is allowed when the parent exists'
    $noFileIndex = Find-RowIndex $list 'nofile.bin' 5
    Select-Only-Row $list $noFileIndex
    $openFolderState = [E2E4]::GetMenuState($menu, $ID_TASK_OPEN_FOLDER, 0)
    Check (($openFolderState -band $MF_GRAYED) -ne 0) 'open folder is disabled when the parent directory is missing'
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_TASK_OPEN_FOLDER, [IntPtr]::Zero)
    Wait-For -timeoutMs 5000 -label 'open folder error dialog' -condition {
        [E2E4]::FindWindowW('#32770', '打开文件夹') -ne [IntPtr]::Zero
    } | Out-Null
    $folderDialog = [E2E4]::FindWindowW('#32770', '打开文件夹')
    $folderTexts = @(Get-DialogTexts $folderDialog) -join "`n"
    Log ("open folder failure: " + $folderTexts)
    Check ($folderTexts -match '文件夹不存在或无法访问') 'open folder reports the real missing-folder reason'
    Capture-Window $folderDialog '04-open-folder-failure.png'
    [void][E2E4]::PostMessageW($folderDialog, $WM_COMMAND, [IntPtr]$IDOK, [E2E4]::GetDlgItem($folderDialog, $IDOK))

    # --- filter AND search --------------------------------------------------------
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_VIEW_FILTER_FAILED, [IntPtr]::Zero)
    Wait-For -timeoutMs 3000 -label 'failed filter' -condition {
        [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1
    } | Out-Null
    $failedRows = @(Get-RemoteListRowsFor $list 5)
    Check ($failedRows.Count -eq 1 -and $failedRows[0][4] -eq '下载失败') 'failed filter shows only 下载失败'
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_EDIT_FIND, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    Set-DialogText $window 2100 'zzz'
    Notify-EditChange $window 2100 $search
    Wait-For -timeoutMs 3000 -label 'filter AND empty search' -condition {
        [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 0
    } | Out-Null
    $statusTexts = @(Get-RemoteStatusTexts $window)
    Log ("filtered status after AND: " + ($statusTexts -join ' | '))
    Check ($statusTexts[0] -match '可见: 0 /') 'query AND status filter shows zero visible of the queue total'
    [void][E2E4]::SendMessageInt($search, $WM_KEYDOWN, $VK_ESCAPE, 0)
    Wait-For -timeoutMs 3000 -label 'failed filter restored after Esc' -condition {
        [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1
    } | Out-Null
    [void][E2E4]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_VIEW_FILTER_ALL, [IntPtr]::Zero)
    Wait-For -timeoutMs 3000 -label 'all filter' -condition {
        [int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) -eq 8
    } | Out-Null
    Check (([int][E2E4]::SendMessagePtr($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)) -eq 8) 'all filter restores every record'

    Close-App $app $window 'final'
    $app = $null
    Check (-not (Get-Process -Name 'litefetch-win32' -ErrorAction SilentlyContinue)) 'no LiteFetch process remains'
    Log 'RESULT: PASS'
    exit 0
} catch {
    Log ("EXCEPTION: " + $_.Exception.Message)
    if ($_.ScriptStackTrace) { Log ($_.ScriptStackTrace) }
    Log 'RESULT: FAIL'
    exit 1
} finally {
    if ($app -and -not $app.HasExited) { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
    if ($batchFixture) { Stop-Fixture $batchFixture }
}
