# Bootstrap the project-local vcpkg tree used by the Qt-free core.
# All tools, downloads, caches, temporary files and installed packages stay
# under <repo>/.deps; no global install or user/system environment change.
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $RepositoryRoot).Path
$deps = Join-Path $root '.deps'
$vcpkg = Join-Path $deps 'vcpkg'
$installed = Join-Path $deps 'installed-core'
$tmp = Join-Path $deps 'tmp'
$downloads = Join-Path $deps 'downloads'
$binaryCache = Join-Path $deps 'binary-cache'
$registries = Join-Path $deps 'registries'
New-Item -ItemType Directory -Force $deps, $tmp, $downloads, $binaryCache, $registries | Out-Null

$env:TEMP = $tmp
$env:TMP = $tmp
$env:VCPKG_DOWNLOADS = $downloads
$env:VCPKG_DEFAULT_BINARY_CACHE = $binaryCache
$env:VCPKG_DISABLE_METRICS = '1'
$env:X_VCPKG_REGISTRIES_CACHE = $registries

if (-not (Test-Path (Join-Path $vcpkg '.git'))) {
    git clone --depth 1 https://github.com/microsoft/vcpkg.git $vcpkg
    if ($LASTEXITCODE -ne 0) { throw "vcpkg clone failed with exit code $LASTEXITCODE" }
}
& (Join-Path $vcpkg 'bootstrap-vcpkg.bat') -disableMetrics
if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed with exit code $LASTEXITCODE" }

& (Join-Path $vcpkg 'vcpkg.exe') install --classic `
    'curl:x64-windows' 'sqlite3:x64-windows' 'zlib:x64-windows' `
    --triplet x64-windows `
    "--x-install-root=$installed" `
    "--x-buildtrees-root=$(Join-Path $deps 'buildtrees')" `
    "--x-packages-root=$(Join-Path $deps 'packages')"
exit $LASTEXITCODE
