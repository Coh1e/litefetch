# Fails when a tracked file still refers to the package manager the project left.
#
# The move to CMake FetchContent left a pile of files behind - vcpkg manifests,
# overlays, bootstrap scripts - and nothing complained, because a file that no
# longer builds anything is invisible to the build. History and research notes are
# exempt: they describe what was true then.
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

$exempt = @(
    'docs/history/',
    'docs/research/',
    'docs/specs/final-report.md',
    # The check carries the pattern itself; scanning it would always fail.
    'scripts/check-no-vcpkg.ps1'
)

# The names a live build entry would use. A sentence explaining the move away from
# the package manager is not a leftover; a command pointing at one is.
$pattern = 'VCPKG_ROOT|vcpkg\.json|vcpkg install|vcpkg\.cmake|bootstrap-core-deps\.ps1'
$files = git ls-files
$findings = @()
foreach ($file in $files) {
    if ($exempt | Where-Object { $file.StartsWith($_) }) { continue }
    if (-not (Test-Path $file)) { continue }
    if ($file -match '\.(png|jpg|ico|zip|db|dll|exe|lib|obj|pdb)$') { continue }
    $matches = Select-String -Path $file -Pattern $pattern -ErrorAction SilentlyContinue
    foreach ($match in $matches) {
        $findings += "$file`:$($match.LineNumber): $($match.Line.Trim())"
    }
}

if ($findings.Count -gt 0) {
    Write-Host 'Tracked files still mention the retired package manager:'
    $findings | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host 'no tracked file refers to vcpkg'
