param([Parameter(Mandatory)][string]$Url,[Parameter(Mandatory)][string]$Sha512,[Parameter(Mandatory)][string]$Destination)
$ErrorActionPreference='Stop'
# vcpkg's GitHub archive URL redirects here. Keep the same archive bytes and verify them.
$source=$Url
if($Url -match '^https://github\.com/([^/]+)/([^/]+)/archive/(.+)\.tar\.gz$'){
    $source="https://codeload.github.com/$($Matches[1])/$($Matches[2])/tar.gz/$($Matches[3])"
}
$project=[IO.Path]::GetFullPath($PSScriptRoot)
$target=[IO.Path]::GetFullPath($Destination)
if(-not $target.StartsWith($project+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){
    throw 'Asset output must stay inside this project.'
}
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference='SilentlyContinue'
$candidates=@($source)
if($source.StartsWith('https://mirror.msys2.org/')){
    # Mirrors also listed by vcpkg's MSYS2 acquisition helper.
    $candidates=@($source.Replace('https://mirror.msys2.org/','https://repo.msys2.org/'),
        $source.Replace('https://mirror.msys2.org/','https://mirrors.ustc.edu.cn/msys2/'))
}
foreach($candidate in $candidates){
    try{
        Invoke-WebRequest -UseBasicParsing -Uri $candidate -OutFile $target -TimeoutSec 180
        # vcpkg sanitizes PSModulePath; avoid depending on Get-FileHash autoload.
        $stream=[IO.File]::OpenRead($target)
        $hasher=[Security.Cryptography.SHA512]::Create()
        try{$actual=[BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','')}
        finally{$stream.Dispose();$hasher.Dispose()}
        if($actual -ne $Sha512){throw 'SHA-512 mismatch'}
        exit 0
    }catch{
        [Console]::Error.WriteLine('Asset fetch failed for host '+([uri]$candidate).Host+' ('+$_.Exception.GetType().Name+'): '+$_.Exception.Message.Split("`n")[0])
    }
}
throw 'No configured source returned the pinned asset.'
