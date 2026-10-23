param()
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath($PSScriptRoot)
$sourceRoot=Join-Path $projectRoot 'browser'
$destination=Join-Path $projectRoot 'out/package/browser-extension'
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$files=@('manifest.json','worker.mjs','handoff.mjs','adapters.mjs','request-observer.mjs','settings.mjs','popup.html','popup.css','popup.mjs')
$manifest=Get-Content -LiteralPath (Join-Path $sourceRoot 'manifest.json') -Raw | ConvertFrom-Json
if($manifest.manifest_version -ne 3 -or $manifest.background.service_worker -ne 'worker.mjs' -or $manifest.background.type -ne 'module'){
    throw 'Unexpected browser extension manifest'
}
foreach($name in $files){
    Copy-Item -LiteralPath (Join-Path $sourceRoot $name) -Destination (Join-Path $destination $name) -Force
}
$archive=Join-Path $projectRoot 'out/package/browser-extension-development.zip'
# Explicit allowlist: never sweep tests, journals, or downloaded files.
Compress-Archive -LiteralPath ($files | ForEach-Object {Join-Path $destination $_}) -DestinationPath $archive -Force
[pscustomobject]@{Directory=$destination;Archive=$archive;Files=$files.Count;NativeHostIncluded=$false} | ConvertTo-Json -Compress
