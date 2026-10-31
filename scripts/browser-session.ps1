# Starts what the browser handoff needs and leaves it running:
#   - LiteFetch with a throwaway database and download folder next to it
#   - Chrome with the DevTools endpoint and the unpacked extension loaded
#   - a slow test site that serves a 2 MiB file at about 200 KiB/s
#
# Everything stays inside the project. Pids are written to tmp\browser-probe so
# browser-session-stop.ps1 can clean up without guessing.
param(
    [int]$SitePort = 47670,
    [int]$DevToolsPort = 9334
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$probe = Join-Path $root 'tmp\browser-probe'
New-Item -ItemType Directory -Force $probe | Out-Null

# Previous processes would hold the pipe and the ports.
Get-Process litefetch-win32, litefetch-browser-host -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process chrome -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -like '*Chrome*' } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$app = Join-Path $root 'out\build\windows-core\litefetch-win32.exe'
$appInfo = New-Object System.Diagnostics.ProcessStartInfo
$appInfo.FileName = $app
$appInfo.Arguments = "--db `"$(Join-Path $probe 'tasks.db')`" --download-dir `"$(Join-Path $probe 'downloads')`""
$appInfo.UseShellExecute = $false
$appProcess = [System.Diagnostics.Process]::Start($appInfo)
$appProcess.Id | Set-Content (Join-Path $probe 'app-pid.txt')
Start-Sleep -Seconds 3
if ($appProcess.HasExited) { throw "LiteFetch exited with $($appProcess.ExitCode)" }

$chromeCandidates = @(
    (Join-Path ${env:ProgramFiles} 'Google\Chrome\Application\chrome.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Google\Chrome\Application\chrome.exe'),
    (Join-Path $env:LOCALAPPDATA 'Google\Chrome\Application\chrome.exe')
)
$chrome = $chromeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $chrome) { throw 'Chrome is not installed in a standard location' }
$chromeInfo = New-Object System.Diagnostics.ProcessStartInfo
$chromeInfo.FileName = $chrome
$chromeInfo.Arguments = "--user-data-dir=`"$(Join-Path $root '.deps\chrome-profile')`""
    + " --remote-debugging-port=$DevToolsPort --enable-unsafe-extension-debugging"
    + " --no-first-run --no-default-browser-check about:blank"
$chromeInfo.UseShellExecute = $false
$chromeProcess = [System.Diagnostics.Process]::Start($chromeInfo)
$chromeProcess.Id | Set-Content (Join-Path $probe 'chrome-pid.txt')
Start-Sleep -Seconds 6

& node (Join-Path $root 'scripts\load-extension.mjs') $DevToolsPort (Join-Path $root 'browser')

$server = Start-Job -ScriptBlock {
    param($port)
    $listener = New-Object System.Net.HttpListener
    $listener.Prefixes.Add("http://127.0.0.1:$port/")
    $listener.Start()
    $bytes = [byte[]]::new(2097152)
    for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [byte]($i % 251) }
    $deadline = (Get-Date).AddSeconds(900)
    while ((Get-Date) -lt $deadline) {
        $context = $listener.GetContext()
        if ($context.Request.Url.AbsolutePath -eq '/') {
            $html = '<!doctype html><meta charset="utf-8"><a href="/handoff.bin">download</a>'
            $buffer = [Text.Encoding]::UTF8.GetBytes($html)
            $context.Response.ContentType = 'text/html'
            $context.Response.ContentLength64 = $buffer.Length
            $context.Response.OutputStream.Write($buffer, 0, $buffer.Length)
        } else {
            $context.Response.ContentType = 'application/octet-stream'
            $context.Response.ContentLength64 = $bytes.Length
            $chunk = 32768
            $offset = 0
            # About 200 KiB/s, so a download is still in flight when the
            # extension looks at it.
            while ($offset -lt $bytes.Length) {
                $size = [Math]::Min($chunk, $bytes.Length - $offset)
                $context.Response.OutputStream.Write($bytes, $offset, $size)
                $context.Response.OutputStream.Flush()
                $offset += $size
                Start-Sleep -Milliseconds 160
            }
        }
        $context.Response.Close()
    }
    $listener.Stop()
} -ArgumentList $SitePort
$server.Id | Set-Content (Join-Path $probe 'server-job.txt')

Start-Sleep -Seconds 2
Write-Host "app=$($appProcess.Id) chrome=$($chromeProcess.Id) site=http://127.0.0.1:$SitePort/ devtools=$DevToolsPort"
