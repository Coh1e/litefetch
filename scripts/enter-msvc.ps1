# Loads the MSVC x64 developer environment into the current PowerShell process.
#
# Both a developer shell and GitHub Actions need the same contract: after this
# script runs, cl.exe, link.exe and dumpbin.exe are on PATH and INCLUDE/LIB point
# at the same Windows SDK. Dot-source it to affect the caller:
#
#   . .\scripts\enter-msvc.ps1
#
# It is a no-op when cl.exe is already available, so callers can always run it.
[CmdletBinding()]
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

function Test-MsvcEnvironment {
    $null -ne (Get-Command cl.exe -ErrorAction SilentlyContinue) -and
    $null -ne (Get-Command dumpbin.exe -ErrorAction SilentlyContinue)
}

if (Test-MsvcEnvironment) {
    if (-not $Quiet) { Write-Host 'MSVC environment already loaded' }
    return
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio Build Tools with the 'Desktop development with C++' workload."
}

$installation = & $vswhere -latest -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $installation) {
    throw "No Visual Studio installation with the MSVC x64 toolset was found."
}

$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found under $installation"
}

# `set` after vcvars is the supported way to read the environment the batch file
# prepares. Only the variables the compiler and linker need are imported, so the
# caller's prompt, working directory and unrelated settings stay untouched.
$wanted = '^(PATH|INCLUDE|LIB|LIBPATH|VCINSTALLDIR|VSINSTALLDIR|VCToolsInstallDir|VCToolsVersion|VCToolsRedistDir|WindowsSdkDir|WindowsSDKVersion|WindowsSdkBinPath|WindowsSdkVerBinPath|UCRTContentRoot|UniversalCRTSdkDir|Platform|VSCMD_ARG_TGT_ARCH|VSCMD_VER|VSAPPIDDIR)$'
$lines = & cmd.exe /d /c "`"$vcvars`" >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed with exit code $LASTEXITCODE" }

foreach ($line in $lines) {
    $separator = $line.IndexOf('=')
    if ($separator -le 0) { continue }
    $name = $line.Substring(0, $separator)
    if ($name -notmatch $wanted) { continue }
    Set-Item -Path "env:$name" -Value $line.Substring($separator + 1)
}

if (-not (Test-MsvcEnvironment)) {
    throw "vcvars64.bat ran, but cl.exe or dumpbin.exe is still not available."
}
if (-not $Quiet) {
    $cl = (Get-Command cl.exe).Source
    Write-Host "MSVC environment loaded: $cl"
}
