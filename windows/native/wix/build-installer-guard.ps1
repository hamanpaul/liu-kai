param(
    [Parameter(Mandatory=$true)][string]$ToolchainBin,
    [Parameter(Mandatory=$true)][string]$OutputFile
)
$ErrorActionPreference='Stop'
$bin=(Resolve-Path -LiteralPath $ToolchainBin).Path
$compiler=(Resolve-Path -LiteralPath (Join-Path $bin 'g++.exe')).Path
$windowsTargetFlags=& (Join-Path $PSScriptRoot '..\windows-target-flags.ps1')
$sdk=Join-Path $PSScriptRoot 'sdk'
$manifestPath=Join-Path $sdk 'SOURCE.json'
$manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8|ConvertFrom-Json
$allowed=[ordered]@{
 'BootstrapperApplicationTypes.h'='db0f123723e240c9d4f925d29b6441016d605576abf1b91bd306ff7ab67cdc1a'
 'BootstrapperEngineTypes.h'='1ca0c9267c89eb8611cc31af6b9894e8873e48c80585bc8e826d7c821f1a06c5'
 'BAFunctions.h'='658ea9f8a6a387f8188e94bb3f439d1c2cd06c5b988f95277644b9f7063c49a6'
 'IBootstrapperEngine.h'='8a136b220af601cd200c4eb601d463991e9f5864ddcdc58b79b509c9ab8c7932'
}
if($manifest.repository -cne 'wixtoolset/wix' -or $manifest.tag -cne 'v6.0.2' -or @($manifest.files).Count -ne 4){throw 'Unexpected WiX BAFunctions SDK manifest.'}
$seen=@{}
foreach($row in @($manifest.files)){
 $name=[string]$row.name
 if($name -ne [IO.Path]::GetFileName($name) -or $name.Contains('/') -or $name.Contains('\') -or (-not $allowed.Contains($name)) -or $seen.ContainsKey($name)){throw 'Unexpected or unsafe WiX BAFunctions SDK header name.'}
 $hash=([string]$row.sha256).ToLowerInvariant()
 $header=Join-Path $sdk $name
 if($hash -cne $allowed[$name] -or -not(Test-Path -LiteralPath $header -PathType Leaf) -or (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash){throw "WiX BAFunctions SDK hash mismatch: $name"}
 $seen[$name]=$true
}
$output=[IO.Path]::GetFullPath($OutputFile)
if(Test-Path -LiteralPath $output){throw 'Guard DLL output already exists.'}
$parent=Split-Path -Parent $output
if(-not(Test-Path -LiteralPath $parent -PathType Container)){throw 'Guard DLL output parent must already exist.'}
$oldPath=$env:PATH
try{
 $env:PATH=$bin+';'+$oldPath
 # Match the existing MSI action DLL: no external MinGW runtime DLLs.
 & $compiler @windowsTargetFlags -std=c++17 -static -shared -O2 '-Wl,--no-insert-timestamp' '-Wl,--exclude-all-symbols' "-I$sdk" (Join-Path $PSScriptRoot 'installer-bafunctions.cpp') -ladvapi32 -luser32 -o $output
 if($LASTEXITCODE -ne 0){throw 'Installer guard DLL compilation failed.'}
}finally{$env:PATH=$oldPath}
if(-not(Test-Path -LiteralPath $output -PathType Leaf)){throw 'Installer guard DLL was not produced.'}
