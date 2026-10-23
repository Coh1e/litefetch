$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($PSScriptRoot)
$evidence=Join-Path $root 'out/verification'
New-Item -ItemType Directory -Force $evidence | Out-Null
$start=[Diagnostics.ProcessStartInfo]::new()
$start.FileName=Join-Path $root 'out/build/windows-local/download-torrent-ui-tests.exe'
$start.WorkingDirectory=Split-Path $start.FileName
$start.UseShellExecute=$false;$start.CreateNoWindow=$true
$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
$start.Environment['PATH']=(Join-Path $root '.deps/installed/x64-windows/debug/bin')+';C:\Windows\System32;C:\Windows'
$start.Environment['TEMP']=Join-Path $root '.deps/tmp';$start.Environment['TMP']=$start.Environment['TEMP']
$start.Environment['QT_QPA_PLATFORM']='windows'
$start.Environment.Remove('QT_PLUGIN_PATH')|Out-Null
$start.Environment.Remove('QT_QPA_PLATFORM_PLUGIN_PATH')|Out-Null
$capture=Join-Path $evidence 'torrent-file-selection.png'
$start.ArgumentList.Add($capture)
$process=[Diagnostics.Process]::Start($start)
$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
try {
    if(-not $process.WaitForExit(10000)){$process.Kill();throw 'Native file dialog test timed out'}
    $stdout.GetAwaiter().GetResult()|Set-Content (Join-Path $evidence 'torrent-file-selection.stdout.log')
    $stderr.GetAwaiter().GetResult()|Set-Content (Join-Path $evidence 'torrent-file-selection.stderr.log')
    if($process.ExitCode -ne 0){throw "Native file dialog test failed: $($process.ExitCode)"}
    if(-not (Test-Path -LiteralPath $capture)){throw 'Native file dialog image missing'}
    Get-Content (Join-Path $evidence 'torrent-file-selection.stdout.log')
} finally {$process.Dispose()}
