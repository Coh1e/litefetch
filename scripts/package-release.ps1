# Assembles a portable Release package for LiteFetch.
#
# Everything stays inside the project: the build lands in out/build/windows-release
# and the package in out/package/<version>. Dependency DLLs are discovered with
# dumpbin and copied from the project dependency tree, never from the system.
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDir = Join-Path $root 'out/build/windows-release'
$packageRoot = Join-Path $root 'out/package'
$version = '0.1.0'
$packageDir = Join-Path $packageRoot ("litefetch-$version")

Write-Host "configuring $buildDir"
& cmake -S $root -B $buildDir --preset windows-release
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

Write-Host "building"
& cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Join-Path $buildDir 'litefetch-win32.exe'
if (-not (Test-Path $exe)) { throw "missing $exe" }

if (Test-Path $packageDir) { Remove-Item $packageDir -Recurse -Force }
New-Item -ItemType Directory -Force $packageDir | Out-Null
Copy-Item $exe $packageDir
Copy-Item (Join-Path $root 'LICENSE') $packageDir

# Dependency closure: dumpbin reports names, the file is looked up in the
# project dependency tree first, then beside the exe.
$dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
if (-not $dumpbin) {
    $vsRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools'
    $dumpbin = (Get-ChildItem $vsRoot -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName)
}
if (-not $dumpbin) { throw "dumpbin not found" }

$searchRoots = @(
    (Join-Path $root '.deps/installed-core/x64-windows/bin'),
    (Join-Path $root '.deps/installed-core/x64-windows/debug/bin'),
    $buildDir
)
$copied = @{}
$queue = New-Object System.Collections.Queue
$queue.Enqueue($exe)
while ($queue.Count -gt 0) {
    $file = $queue.Dequeue()
    $lines = & $dumpbin /nologo /dependents $file 2>$null
    foreach ($line in $lines) {
        if ($line -notmatch '^\s+([\w\-\.]+\.dll)\s*$') { continue }
        $dll = $Matches[1]
        if ($copied.ContainsKey($dll)) { continue }
        $copied[$dll] = $true
        $source = $null
        foreach ($root2 in $searchRoots) {
            $candidate = Join-Path $root2 $dll
            if (Test-Path $candidate) { $source = $candidate; break }
        }
        if (-not $source) { continue }  # system DLL, not shipped
        Copy-Item $source $packageDir
        $queue.Enqueue($source)
    }
}

@'
LiteFetch (portable)
====================

Run litefetch-win32.exe. Tasks and settings live next to the database file in
the folder you start it from; nothing is written outside this package.

Dependencies shipped beside the executable are the ones the build requires;
system libraries come from Windows itself.
'@ | Set-Content -LiteralPath (Join-Path $packageDir 'README.txt') -Encoding UTF8

@'
Third-party components
======================

libcurl        https://curl.se/            MIT-like (see curl licence)
SQLite         https://sqlite.org/         Public domain
libtorrent     https://libtorrent.org/     BSD-3-Clause
OpenSSL        https://www.openssl.org/    Apache-2.0
zlib           https://zlib.net/           Zlib

LiteFetch itself is MIT licensed; see LICENSE in this folder.
No Qt components and no closed-source download engine are part of this package.
'@ | Set-Content -LiteralPath (Join-Path $packageDir 'THIRD-PARTY.txt') -Encoding UTF8

$size = (Get-ChildItem $packageDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("package: {0} files, {1:N1} MiB" -f (Get-ChildItem $packageDir -Recurse -File).Count, ($size / 1MB))
Get-ChildItem $packageDir | Select-Object Name, Length