param([ValidateSet('package','build')][string]$Target='package',[string]$DataDir)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($PSScriptRoot)
$evidence=Join-Path $root 'out/verification'
New-Item -ItemType Directory -Force $evidence | Out-Null
$appPath=if($Target -eq 'package'){Join-Path $root 'out/package/bin/native-downloader.exe'}else{Join-Path $root 'out/build/windows-local/native-downloader.exe'}
$reportPath=Join-Path $evidence "qt-smoke-$Target.json"
$p=[Diagnostics.ProcessStartInfo]::new()
$p.FileName=$appPath;$p.WorkingDirectory=Split-Path $appPath
$p.UseShellExecute=$false;$p.CreateNoWindow=$true
$p.RedirectStandardOutput=$true;$p.RedirectStandardError=$true
$p.Environment['PATH']='C:\Windows\System32;C:\Windows'
$p.Environment['QT_QPA_PLATFORM']='windows'
$p.Environment.Remove('QT_PLUGIN_PATH')|Out-Null
$p.Environment.Remove('QT_QPA_PLATFORM_PLUGIN_PATH')|Out-Null
$p.Environment['TEMP']=Join-Path $root '.deps/tmp';$p.Environment['TMP']=$p.Environment['TEMP']
$p.ArgumentList.Add('--smoke-report');$p.ArgumentList.Add($reportPath)
$p.ArgumentList.Add('--data-dir');$p.ArgumentList.Add($(if($DataDir){[IO.Path]::GetFullPath($DataDir)}else{Join-Path $evidence "smoke-data-$Target"}))
$proc=[Diagnostics.Process]::Start($p)
$stderr=$proc.StandardError.ReadToEndAsync()
try{
    if(-not $proc.WaitForExit(15000)){$proc.Kill();throw 'GUI startup timed out'}
    $stderr.GetAwaiter().GetResult()|Set-Content "$evidence/qt-smoke-$Target.stderr.log"
    if($proc.ExitCode -ne 0){throw "GUI startup failed: $($proc.ExitCode)"}
    $report=Get-Content $reportPath -Raw|ConvertFrom-Json
    if($report.platform -ne 'windows' -or -not $report.window_visible -or -not $report.window_capture_saved){throw 'Native GUI evidence incomplete'}
    Get-Content $reportPath
}finally{$proc.Dispose()}
