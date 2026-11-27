param(
    [Parameter(Mandatory=$true)][string]$ToolchainBin,
    [string]$OutputRoot = ''
)
$ErrorActionPreference='Stop'
$windowsRoot=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if(-not $OutputRoot){
    $OutputRoot=Join-Path $windowsRoot 'build\doctor-p0'
}
$outputRoot=[IO.Path]::GetFullPath($OutputRoot)
$buildRoot=[IO.Path]::GetFullPath((Join-Path $windowsRoot 'build')).TrimEnd('\')
if(-not $outputRoot.StartsWith($buildRoot+'\',[StringComparison]::OrdinalIgnoreCase)){
    throw 'Doctor probe output must stay below windows\build.'
}
if(-not (Test-Path -LiteralPath $outputRoot)){
    [IO.Directory]::CreateDirectory($outputRoot)|Out-Null
}
$output=Join-Path $outputRoot ('probe-'+[Guid]::NewGuid().ToString('N'))
& (Join-Path $PSScriptRoot 'build-fresh-release.ps1') `
    -ToolchainBin $ToolchainBin `
    -Version '0.1.1-doctor-p0' `
    -OutputDir $output `
    -SetupName 'WuGai-doctor-p0-x64-Setup.exe'
if($LASTEXITCODE -ne 0){throw 'Doctor probe build failed.'}
[pscustomobject]@{
    output=$output
    setup=Join-Path $output 'WuGai-doctor-p0-x64-Setup.exe'
    install_note='Sandbox/dev environment only; this script does not register or install the DLL.'
} | ConvertTo-Json
