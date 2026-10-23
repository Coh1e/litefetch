param()
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$allow=Get-Content -LiteralPath (Join-Path $root 'boost-allowlist.json') -Raw | ConvertFrom-Json
$raw=Get-Content -LiteralPath (Join-Path $root '.deps/installed/vcpkg/status') -Raw
$boostCount=0
foreach($paragraph in ($raw -split '\r?\n\r?\n')){
    $fields=@{}
    foreach($line in ($paragraph -split '\r?\n')){if($line -match '^([^:]+): (.*)$'){$fields[$matches[1]]=$matches[2]}}
    if($fields['Status'] -ne 'install ok installed'){continue}
    $name=$fields['Package']
    if($name -eq 'wxwidgets' -or $name -eq 'boost'){throw "Forbidden dependency: $name"}
    if($name -eq 'boost-asio' -and $fields['Feature'] -in @('spawn','deadline-timer')){throw "Unapproved Asio feature: $($fields['Feature'])"}
    if($name -like 'boost*' -and $fields.ContainsKey('Version')){
        if($name -notin $allow.modules){throw "Boost module requires review: $name"}
        if($fields['Version'] -ne $allow.version){throw "Boost version requires review: $name"}
        $boostCount++
    }
}
Write-Output "Dependency guard passed: $boostCount allowed Boost packages; no wxWidgets or Asio coroutine/timer defaults."
