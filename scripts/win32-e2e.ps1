# End-to-end Win32 acceptance run for Coh1e/litefetch #3.
#
# Starts a controlled HTTP peer, drives the real window through real window
# messages (menu command, native dialog controls, toolbar command), checks the
# resulting file hash and persisted task state, then closes the window while a
# transfer is running and verifies an orderly exit with no partial file.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts/win32-e2e.ps1
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
$evidence = Join-Path $root '.pi/evidence/issue-03'
$work = Join-Path $evidence 'e2e'
$log = Join-Path $evidence 'win32-e2e.log'
$env:PATH = "$depsBin;$env:PATH"
$env:TEMP = Join-Path $root '.deps/tmp'
$env:TMP = $env:TEMP

$WM_COMMAND = 0x0111
$WM_CLOSE = 0x0010
$WM_SETTEXT = 0x000C
$WM_GETTEXT = 0x000D
$WM_GETTEXTLENGTH = 0x000E
$BM_SETCHECK = 0x00F1
$ID_FILE_NEW = 1001
$ID_TASK_START = 1030
$ID_TASK_PAUSE = 1031
$ID_TASK_STOP = 1032
$ID_TASK_DELETE = 1033
$ID_TASK_OPEN_FOLDER = 1034
$ID_TOOLS_OPTIONS = 1040
$IDC_URL = 2001
$IDC_SAVE_DIR = 2003
$IDC_AUTO_START = 2005
$IDOK = 1
$MF_GRAYED = 0x00000001
$TB_ISBUTTONENABLED = 0x0409
$SB_GETPARTS = 0x0406
$LVM_FIRST = 0x1000

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class E2E {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
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
    public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
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
}
"@

New-Item -ItemType Directory -Force $work | Out-Null
$transcript = [System.Collections.Generic.List[string]]::new()
function Log([string]$text) {
    Write-Host $text
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

function Find-Child([IntPtr]$parent, [string]$className) {
    $script:childResult = [IntPtr]::Zero
    $callback = [E2E+EnumProc]{
        param([IntPtr]$hWnd, [IntPtr]$lParam)
        $name = New-Object System.Text.StringBuilder 128
        [void][E2E]::GetClassNameW($hWnd, $name, 128)
        if ($name.ToString() -eq $className) {
            $script:childResult = $hWnd
            return $false
        }
        return $true
    }
    [void][E2E]::EnumChildWindows($parent, $callback, [IntPtr]::Zero)
    return $script:childResult
}

function Find-TopWindow([string]$className, [string]$titleFragment) {
    $script:topResult = [IntPtr]::Zero
    $callback = [E2E+EnumProc]{
        param([IntPtr]$hWnd, [IntPtr]$lParam)
        if (-not [E2E]::IsWindowVisible($hWnd)) { return $true }
        $name = New-Object System.Text.StringBuilder 128
        [void][E2E]::GetClassNameW($hWnd, $name, 128)
        if ($name.ToString() -ne $className) { return $true }
        $title = New-Object System.Text.StringBuilder 512
        [void][E2E]::GetWindowTextW($hWnd, $title, 512)
        if ($title.ToString().Contains($titleFragment)) {
            $script:topResult = $hWnd
            return $false
        }
        return $true
    }
    [void][E2E]::EnumWindows($callback, [IntPtr]::Zero)
    return $script:topResult
}

function Window-Text([IntPtr]$handle) {
    $buffer = New-Object System.Text.StringBuilder 1024
    [void][E2E]::GetWindowTextW($handle, $buffer, 1024)
    return $buffer.ToString()
}

function Capture-Window([IntPtr]$handle, [string]$file) {
    [void][E2E]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 250
    $rect = New-Object E2E+RECT
    if (-not [E2E]::GetWindowRect($handle, [ref]$rect)) { Fail "GetWindowRect failed for $file" }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    try { $ok = [E2E]::PrintWindow($handle, $hdc, 2) } finally { $graphics.ReleaseHdc($hdc) }
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

function Start-App([string]$database, [string]$downloads) {
    $process = Start-Process -FilePath $appPath -ArgumentList @('--db', $database, '--download-dir', $downloads) -PassThru
    Log "app pid=$($process.Id) db=$database downloads=$downloads"
    Wait-For -timeoutMs 20000 -label 'main window' -condition {
        [E2E]::FindWindowW('LiteFetchMainWindow', 'lite fetch') -ne [IntPtr]::Zero
    } | Out-Null
    return $process
}

function Get-Tasks([string]$database) {
    $raw = & $cliPath list --db $database 2>$null
    if ($LASTEXITCODE -ne 0) { return @() }
    $parsed = $raw | ConvertFrom-Json
    return @($parsed.tasks)
}

function Open-NewDownloadDialog([IntPtr]$window) {
    [void][E2E]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_FILE_NEW, [IntPtr]::Zero)
    Wait-For -timeoutMs 10000 -label 'new download dialog' -condition {
        [E2E]::FindWindowW('#32770', '新建下载') -ne [IntPtr]::Zero
    } | Out-Null
    return [E2E]::FindWindowW('#32770', '新建下载')
}

function Set-DialogText([IntPtr]$dialog, [int]$id, [string]$text) {
    $control = [E2E]::GetDlgItem($dialog, $id)
    if ($control -eq [IntPtr]::Zero) { Fail "dialog control $id not found" }
    [void][E2E]::SendMessageString($control, $WM_SETTEXT, [IntPtr]::Zero, $text)
}

function Close-App($process, [IntPtr]$window, [string]$label) {
    [void][E2E]::PostMessageW($window, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $process.WaitForExit(30000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        Fail "${label}: process did not exit within 30 s"
    }
    Check ($process.ExitCode -eq 0) "${label}: process exited with code $($process.ExitCode)"
    Check (-not (Get-Process -Id $process.Id -ErrorAction SilentlyContinue)) "${label}: no background process remains"
}

$PROCESS_VM = 0x0008 -bor 0x0010 -bor 0x0020 -bor 0x0400
$MEM_COMMIT_RESERVE = 0x1000 -bor 0x2000
$PAGE_READWRITE = 0x04
$MEM_RELEASE = 0x8000
$LVM_GETITEMTEXTW = $LVM_FIRST + 115
$SB_GETTEXTLENGTHW = 0x0400 + 12
$SB_GETTEXTW = 0x0400 + 13

function Open-ProcessForWindow([IntPtr]$window) {
    $processId = 0
    [void][E2E]::GetWindowThreadProcessId($window, [ref]$processId)
    $handle = [E2E]::OpenProcess($PROCESS_VM, $false, $processId)
    if ($handle -eq [IntPtr]::Zero) { throw "OpenProcess failed for pid $processId" }
    return $handle
}

function Read-RemoteString([IntPtr]$process, [IntPtr]$address, [int]$maxChars) {
    $buffer = New-Object byte[] ($maxChars * 2)
    $read = [UIntPtr]::Zero
    if (-not [E2E]::ReadProcessMemory($process, $address, $buffer, [System.UIntPtr]::new($buffer.Length), [ref]$read)) {
        throw 'ReadProcessMemory failed'
    }
    $length = 0
    while ($length -lt $maxChars -and $buffer[$length * 2] -ne 0) { $length += 1 }
    return [System.Text.Encoding]::Unicode.GetString($buffer, 0, $length * 2)
}

function Get-RemoteListRows([IntPtr]$window) {
    $list = Find-Child $window 'SysListView32'
    if ($list -eq [IntPtr]::Zero) { return @() }
    $process = Open-ProcessForWindow $window
    $remote = [E2E]::VirtualAllocEx($process, [IntPtr]::Zero, [System.UIntPtr]::new(4096), $MEM_COMMIT_RESERVE, $PAGE_READWRITE)
    if ($remote -eq [IntPtr]::Zero) { [void][E2E]::CloseHandle($process); throw 'VirtualAllocEx failed' }
    try {
        $count = [int][E2E]::SendMessagePtr($list, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero) # LVM_GETITEMCOUNT
        $rows = @()
        for ($row = 0; $row -lt $count; $row += 1) {
            $texts = @()
            for ($column = 0; $column -lt 6; $column += 1) {
                $item = New-Object E2E+LVITEMW
                $item.mask = 0x1 # LVIF_TEXT
                $item.iItem = $row
                $item.iSubItem = $column
                $item.pszText = [IntPtr]($remote.ToInt64() + 512)
                $item.cchTextMax = 256
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
                [void][E2E]::WriteProcessMemory($process, $remote, $bytes, [System.UIntPtr]::new($size), [ref]$written)
                [void][E2E]::SendMessagePtr($list, $LVM_GETITEMTEXTW, [IntPtr]$row, $remote)
                $texts += Read-RemoteString $process ([IntPtr]($remote.ToInt64() + 512)) 256
            }
            $rows += ,$texts
        }
        return $rows
    } finally {
        [void][E2E]::VirtualFreeEx($process, $remote, [UIntPtr]::Zero, $MEM_RELEASE)
        [void][E2E]::CloseHandle($process)
    }
}

function Get-RemoteStatusTexts([IntPtr]$window) {
    $status = Find-Child $window 'msctls_statusbar32'
    if ($status -eq [IntPtr]::Zero) { return @() }
    $process = Open-ProcessForWindow $window
    $remote = [E2E]::VirtualAllocEx($process, [IntPtr]::Zero, [System.UIntPtr]::new(4096), $MEM_COMMIT_RESERVE, $PAGE_READWRITE)
    if ($remote -eq [IntPtr]::Zero) { [void][E2E]::CloseHandle($process); throw 'VirtualAllocEx failed' }
    try {
        $texts = @()
        for ($part = 0; $part -lt 4; $part += 1) {
            [void][E2E]::SendMessagePtr($status, $SB_GETTEXTW, [IntPtr]$part, $remote)
            $texts += Read-RemoteString $process $remote 256
        }
        return $texts
    } finally {
        [void][E2E]::VirtualFreeEx($process, $remote, [UIntPtr]::Zero, $MEM_RELEASE)
        [void][E2E]::CloseHandle($process)
    }
}

function Invoke-CompletedDownload([string]$label) {
    $folder = Join-Path $work $label
    $database = Join-Path $folder 'tasks.db'
    $downloads = Join-Path $folder '下载目录'
    New-Item -ItemType Directory -Force $downloads | Out-Null
    $fixture = Start-Fixture -size 1048576
    $app = $null
    try {
        $app = Start-App -database $database -downloads $downloads
        $window = [E2E]::FindWindowW('LiteFetchMainWindow', 'lite fetch')

        $menu = [E2E]::GetMenu($window)
        $menuCount = [E2E]::GetMenuItemCount($menu)
        Check ($menuCount -eq 6) "main window has six top-level menus (got $menuCount)"
        $titles = @()
        for ($index = 0; $index -lt $menuCount; $index += 1) {
            $buffer = New-Object System.Text.StringBuilder 64
            [void][E2E]::GetMenuStringW($menu, $index, $buffer, 64, 0x400)
            $titles += $buffer.ToString().Replace('&', '')
        }
        Log ("menu titles: " + ($titles -join ' | '))

        $status = Find-Child $window 'msctls_statusbar32'
        Check ($status -ne [IntPtr]::Zero) 'status bar control exists'
        $parts = [E2E]::SendMessagePtr($status, $SB_GETPARTS, [IntPtr]::Zero, [IntPtr]::Zero)
        Check ([int]$parts -eq 4) "status bar has four sections (got $parts)"

        $list = Find-Child $window 'SysListView32'
        Check ($list -ne [IntPtr]::Zero) 'Details ListView exists'
        $columnCount = 0
        $header = Find-Child $list 'SysHeader32'
        if ($header -ne [IntPtr]::Zero) {
            $columnCount = [int][E2E]::SendMessagePtr($header, 0x1200, [IntPtr]::Zero, [IntPtr]::Zero) # HDM_GETITEMCOUNT
        }
        Check ($columnCount -eq 6) "list has six columns (got $columnCount)"
        $toolbar = Find-Child $window 'ToolbarWindow32'
        Check ($toolbar -ne [IntPtr]::Zero) 'toolbar control exists'

        $dialog = Open-NewDownloadDialog $window
        $url = "http://127.0.0.1:$($fixture.Port)/files/%E5%B7%A5%E5%85%B7%E5%8C%85.zip"
        Set-DialogText $dialog $IDC_URL $url
        # WM_SETTEXT does not raise EN_CHANGE, so send the same notification the
        # edit control sends while a user types; the hint must identify HTTPS.
        $urlControl = [E2E]::GetDlgItem($dialog, $IDC_URL)
        [void][E2E]::SendMessagePtr($dialog, $WM_COMMAND, [IntPtr](2001 -bor (0x0300 -shl 16)), $urlControl)
        Start-Sleep -Milliseconds 400
        Capture-Window $dialog 'win32-dialog.png'
        [void][E2E]::SendMessageInt([E2E]::GetDlgItem($dialog, $IDC_AUTO_START), $BM_SETCHECK, 0, 0)
        [void][E2E]::PostMessageW($dialog, $WM_COMMAND, [IntPtr]$IDOK, [E2E]::GetDlgItem($dialog, $IDOK))

        Wait-For -timeoutMs 20000 -label 'queued task row' -condition {
            $tasks = @(Get-Tasks $database)
            return ($tasks.Count -eq 1) -and ($tasks[0].state -eq 'queued')
        } | Out-Null
        Log 'task added without auto-start reports queued state'
        Start-Sleep -Milliseconds 600
        Capture-Window $window 'win32-queued.png'
        $rows = @(Get-RemoteListRows $window)
        Check ($rows.Count -eq 1) "list contains one real task row (got $($rows.Count))"
        Log ("queued row: " + ($rows[0] -join ' | '))
        Check ($rows[0][0] -eq '工具包.zip') "row name column ($($rows[0][0]))"
        Check ($rows[0][5] -eq '等待中') "row status column before start ($($rows[0][5]))"
        $statusTexts = @(Get-RemoteStatusTexts $window)
        Check ($statusTexts.Count -eq 4) "status bar exposes four texts (got $($statusTexts.Count))"
        Check ($statusTexts[0] -eq '任务: 1') "status task count ($($statusTexts[0]))"
        Check ($statusTexts[1] -eq '活动: 0') "status active count before start ($($statusTexts[1]))"
        $startEnabled = [E2E]::SendMessageInt($toolbar, $TB_ISBUTTONENABLED, $ID_TASK_START, 0)
        $stopEnabled = [E2E]::SendMessageInt($toolbar, $TB_ISBUTTONENABLED, $ID_TASK_STOP, 0)
        Check ([int]$startEnabled -ne 0) 'start button is enabled for the queued task'
        Check ([int]$stopEnabled -ne 0) 'stop button is enabled for the queued task'
        $pauseEnabled = [E2E]::SendMessageInt($toolbar, $TB_ISBUTTONENABLED, $ID_TASK_PAUSE, 0)
        $deleteEnabled = [E2E]::SendMessageInt($toolbar, $TB_ISBUTTONENABLED, $ID_TASK_DELETE, 0)
        $optionsEnabled = [E2E]::SendMessageInt($toolbar, $TB_ISBUTTONENABLED, $ID_TOOLS_OPTIONS, 0)
        Check ([int]$pauseEnabled -eq 0) 'pause button stays disabled'
        Check ([int]$deleteEnabled -eq 0) 'delete button stays disabled'
        Check ([int]$optionsEnabled -eq 0) 'options button stays disabled'
        $pauseMenu = [E2E]::GetMenuState($menu, $ID_TASK_PAUSE, 0)
        Check (($pauseMenu -band $MF_GRAYED) -ne 0) 'pause menu item is grayed'
        $startMenu = [E2E]::GetMenuState($menu, $ID_TASK_START, 0)
        Check (($startMenu -band $MF_GRAYED) -eq 0) 'start menu item is enabled for the queued task'

        [void][E2E]::PostMessageW($window, $WM_COMMAND, [IntPtr]1010, [IntPtr]::Zero) # ID_EDIT_COPY_LINK
        Start-Sleep -Milliseconds 400
        $clipboard = (Get-Clipboard -Raw).TrimEnd()
        Check ($clipboard -eq (Get-Tasks $database)[0].source) 'copy link puts the real task source on the clipboard'

        [void][E2E]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_TASK_START, [IntPtr]::Zero)
        Wait-For -timeoutMs 30000 -label 'completed download' -condition {
            $tasks = @(Get-Tasks $database)
            return ($tasks.Count -eq 1) -and ($tasks[0].state -eq 'completed')
        } | Out-Null
        $task = @(Get-Tasks $database)[0]
        Check ($task.destination -eq (Join-Path $downloads '工具包.zip')) "dialog default folder and Chinese file name produced the destination ($($task.destination))"
        Check ([int64]$task.received -eq $fixture.Size) "persisted received bytes ($($task.received))"
        Check ([int64]$task.total -eq $fixture.Size) "persisted total bytes ($($task.total))"
        $file = Join-Path $downloads '工具包.zip'
        Check (Test-Path -LiteralPath $file) 'downloaded file exists at the Chinese path'
        $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        Check ($hash -eq $fixture.Sha256) "downloaded SHA-256 matches the controlled peer ($hash)"
        Start-Sleep -Milliseconds 600
        Capture-Window $window 'win32-completed.png'
        $rows = @(Get-RemoteListRows $window)
        Check ($rows.Count -eq 1) "list still contains one row (got $($rows.Count))"
        Log ("completed row: " + ($rows[0] -join ' | '))
        Check ($rows[0][0] -eq '工具包.zip') "row name after completion ($($rows[0][0]))"
        Check ($rows[0][1] -eq '1.00 MB') "row size comes from the real total ($($rows[0][1]))"
        Check ($rows[0][2] -eq '100.0%') "row progress after completion ($($rows[0][2]))"
        Check ($rows[0][5] -eq '已完成') "row status after completion ($($rows[0][5]))"
        $statusTexts = @(Get-RemoteStatusTexts $window)
        Log ("status parts: " + ($statusTexts -join ' | '))
        Check ($statusTexts[0] -eq '任务: 1') "status task count reflects the real row ($($statusTexts[0]))"
        Check ($statusTexts[1] -eq '活动: 0') "status active count is zero after completion ($($statusTexts[1]))"
        $startEnabled = [E2E]::SendMessageInt($toolbar, $TB_ISBUTTONENABLED, $ID_TASK_START, 0)
        $stopEnabled = [E2E]::SendMessageInt($toolbar, $TB_ISBUTTONENABLED, $ID_TASK_STOP, 0)
        Check ([int]$startEnabled -eq 0) 'start button is disabled for a completed task'
        Check ([int]$stopEnabled -eq 0) 'stop button is disabled for a completed task'

        [void][E2E]::PostMessageW($window, $WM_COMMAND, [IntPtr]$ID_TASK_OPEN_FOLDER, [IntPtr]::Zero)
        $script:explorer = [IntPtr]::Zero
        Wait-For -timeoutMs 10000 -label 'explorer window for the downloaded folder' -condition {
            $script:explorer = Find-TopWindow 'CabinetWClass' '下载目录'
            return $script:explorer -ne [IntPtr]::Zero
        } | Out-Null
        Check ($script:explorer -ne [IntPtr]::Zero) 'open folder launches Explorer at the real download folder'
        if ($script:explorer -ne [IntPtr]::Zero) {
            [void][E2E]::PostMessageW($script:explorer, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 500
        }

        Close-App $app $window 'completed run'
    } finally {
        if ($app -and -not $app.HasExited) { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
        Stop-Fixture $fixture
    }
}

function Invoke-CloseDuringDownload {
    $folder = Join-Path $work 'close-during'
    $database = Join-Path $folder 'tasks.db'
    $downloads = Join-Path $folder '下载目录'
    New-Item -ItemType Directory -Force $downloads | Out-Null
    $fixture = Start-Fixture -size (4 * 1024 * 1024) -rate 131072
    $app = $null
    try {
        $app = Start-App -database $database -downloads $downloads
        $window = [E2E]::FindWindowW('LiteFetchMainWindow', 'lite fetch')
        $dialog = Open-NewDownloadDialog $window
        $url = "http://127.0.0.1:$($fixture.Port)/files/slow.bin"
        Set-DialogText $dialog $IDC_URL $url
        [void][E2E]::PostMessageW($dialog, $WM_COMMAND, [IntPtr]$IDOK, [E2E]::GetDlgItem($dialog, $IDOK))

        Wait-For -timeoutMs 30000 -label 'running transfer with progress' -condition {
            $tasks = @(Get-Tasks $database)
            return ($tasks.Count -eq 1) -and ($tasks[0].state -eq 'running') -and ([int64]$tasks[0].received -gt 0)
        } | Out-Null
        Start-Sleep -Milliseconds 800
        Capture-Window $window 'win32-downloading.png'
        $rows = @(Get-RemoteListRows $window)
        Log ("downloading row: " + ($rows[0] -join ' | '))
        Check ($rows[0][5] -eq '下载中') "row status during transfer ($($rows[0][5]))"
        Check ($rows[0][2] -ne '—') "row progress during transfer ($($rows[0][2]))"
        Check ($rows[0][3] -ne '—') "row speed during transfer ($($rows[0][3]))"
        $statusTexts = @(Get-RemoteStatusTexts $window)
        Log ("downloading status: " + ($statusTexts -join ' | '))
        Check ($statusTexts[1] -eq '活动: 1') "status active count during transfer ($($statusTexts[1]))"
        Check ($statusTexts[2] -ne '↓ —') "status download speed during transfer ($($statusTexts[2]))"

        Close-App $app $window 'close during download'
        $task = @(Get-Tasks $database)[0]
        Check ($task.state -eq 'cancelled') "closing persisted the cancelled state (got $($task.state))"
        Check (-not (Test-Path -LiteralPath (Join-Path $downloads 'slow.bin'))) 'closing did not publish a partial file'
        $partials = Get-ChildItem -LiteralPath $downloads -Filter '*.part-*' -ErrorAction SilentlyContinue
        Check ($partials.Count -eq 0) 'no staging file remains after closing'
    } finally {
        if ($app -and -not $app.HasExited) { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
        Stop-Fixture $fixture
    }
}

$failure = $null
try {
    Log "repository=$root build=$build"
    Log "app=$appPath (exists=$(Test-Path $appPath))"
    Invoke-CompletedDownload 'completed'
    Invoke-CloseDuringDownload
    Log 'E2E: ALL CHECKS PASSED'
} catch {
    $failure = $_
    Log ("E2E FAILED: " + $_.Exception.Message)
} finally {
    $transcript | Set-Content -LiteralPath $log -Encoding UTF8
}
if ($failure) { exit 1 }
