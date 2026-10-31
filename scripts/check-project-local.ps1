# Keeps project-local paths honest: no drive-letter paths in product or tooling
# code, and no /tmp writes in hooks or scripts. Test fixtures are exempt because
# they use drive letters as sample data.
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

$scope = git ls-files | Where-Object { $_ -match '^(src/|scripts/|browser/|\.githooks/)' }
$binary = '\.(png|jpg|jpeg|ico|zip|db|exe|dll|lib|obj|pdb|woff2?)$'
# The check itself carries the patterns, so scanning it would always fail.
$exempt = @('scripts/check-project-local.ps1')
# A drive letter only counts when it starts a token; "ed2k://" and "https://"
# must not look like a path.
$driveLetter = '(?<![A-Za-z0-9])[A-Za-z]:[\\/]'

$findings = @()
foreach ($file in $scope) {
    if ($file -match $binary) { continue }
    if ($exempt -contains $file) { continue }
    $isDoc = $file -match '\.md$'
    $path = Join-Path $root $file
    if (-not (Test-Path $path)) { continue }
    $lines = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8) -split "`n"
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        if (-not $isDoc -and $lines[$index] -match $driveLetter) {
            $findings += "$file`:$($index + 1): absolute drive path"
        }
        if (($file -like '.githooks/*' -or $file -like 'scripts/*') -and $lines[$index] -match '/tmp/') {
            $findings += "$file`:$($index + 1): /tmp path"
        }
    }
}

if ($findings.Count -gt 0) {
    Write-Host 'Project-local path gate failed:'
    $findings | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host 'no absolute drive paths or /tmp writes outside the tests'
