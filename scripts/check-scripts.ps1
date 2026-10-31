# Parses every tracked PowerShell script and rejects non-ASCII files without a
# UTF-8 BOM. Windows PowerShell 5.1 reads a BOM-less script as ANSI, which turned
# a Chinese string into a syntax error during this session's CI work.
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

$findings = @()
foreach ($file in (git ls-files '*.ps1')) {
    $path = Join-Path $root $file
    if (-not (Test-Path $path)) { continue }
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$null, [ref]$errors)
    if ($errors) {
        $findings += "$file`: $($errors[0].Message)"
        continue
    }
    $bytes = [IO.File]::ReadAllBytes($path)
    $text = [Text.Encoding]::UTF8.GetString($bytes)
    $nonAscii = $false
    foreach ($character in $text.ToCharArray()) {
        if ([int]$character -gt 127) { $nonAscii = $true; break }
    }
    $bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    if ($nonAscii -and -not $bom) {
        $findings += "$file`: non-ASCII content without a UTF-8 BOM"
    }
}

if ($findings.Count -gt 0) {
    Write-Host 'PowerShell scripts failed the gate:'
    $findings | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host 'every PowerShell script parses; non-ASCII files carry a BOM'
