param(
    [Parameter(Mandatory=$true)][string]$ToolchainBin,
    [string]$WorkDir = ''
)
$ErrorActionPreference='Stop'
$compiler=(Resolve-Path -LiteralPath (Join-Path $ToolchainBin 'g++.exe')).Path
$native=(Resolve-Path -LiteralPath $PSScriptRoot).Path
if(-not $WorkDir){
    $repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
    $root=Join-Path $repo 'build\doctor-p0-tests'
    if(-not (Test-Path -LiteralPath $root)){[IO.Directory]::CreateDirectory($root)|Out-Null}
    $WorkDir=Join-Path $root ('run-'+[Guid]::NewGuid().ToString('N'))
}
$work=[IO.Path]::GetFullPath($WorkDir)
if(Test-Path -LiteralPath $work){throw 'WorkDir must not already exist.'}
[IO.Directory]::CreateDirectory($work)|Out-Null

foreach($name in @('doctor_transport_tests','doctor_collector_tests','doctor_language_menu_tests','legacy_runtime_diagnostics_tests')){
    $exe=Join-Path $work ($name+'.exe')
    & $compiler -std=c++17 -O2 -static `
        (Join-Path $native ('tests\'+$name+'.cpp')) `
        -lole32 -loleaut32 -luuid -lgdi32 -ladvapi32 -lbcrypt -lshlwapi -o $exe
    if($LASTEXITCODE -ne 0){throw "Doctor P0 compilation failed: $name"}
    & $exe
    if($LASTEXITCODE -ne 0){throw "Doctor P0 test failed: $name"}
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $native 'tests\run-tsf-uiless-input-tests.ps1') `
    -ToolchainBin $ToolchainBin -WorkDir (Join-Path $work 'tsf-uiless')
if($LASTEXITCODE -ne 0){throw 'Doctor P0 TSF integration failed.'}

[pscustomobject]@{work_dir=$work;doctor_native=$true} | ConvertTo-Json
