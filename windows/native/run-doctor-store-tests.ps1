param(
    [Parameter(Mandatory=$true)][string]$ToolchainBin,
    [string]$RepoRoot = '',
    [string]$WorkDir = ''
)

$ErrorActionPreference = 'Stop'
$windowsTargetFlags = & (Join-Path $PSScriptRoot 'windows-target-flags.ps1')
$compiler = (Resolve-Path -LiteralPath (
    Join-Path $ToolchainBin 'g++.exe')).Path
$native = (Resolve-Path -LiteralPath $PSScriptRoot).Path
if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path -LiteralPath (
        Join-Path $PSScriptRoot '..\..')).Path
}
$repo = (Resolve-Path -LiteralPath $RepoRoot).Path
if (-not (Test-Path -LiteralPath (
    Join-Path $repo 'shared\testdata\doctor-v1\ordinary-touch.jsonl'))) {
    throw 'RepoRoot does not contain shared\testdata\doctor-v1.'
}
if (-not $WorkDir) {
    $WorkDir = Join-Path $env:TEMP (
        'liu-kai-doctor-store-tests-' + [Guid]::NewGuid().ToString('N'))
}
$work = [IO.Path]::GetFullPath($WorkDir)
if (Test-Path -LiteralPath $work) {
    throw 'WorkDir must not already exist.'
}
[IO.Directory]::CreateDirectory($work) | Out-Null

foreach ($name in @('doctor_event_tests', 'doctor_store_tests')) {
    $exe = Join-Path $work ($name + '.exe')
    & $compiler @windowsTargetFlags -std=c++17 -O2 -static `
        (Join-Path $native ('tests\' + $name + '.cpp')) `
        -lole32 -loleaut32 -luuid -lgdi32 -ladvapi32 -lbcrypt -lshlwapi `
        -o $exe
    if ($LASTEXITCODE -ne 0) {
        throw "Doctor store compilation failed: $name"
    }
    & $exe $repo
    if ($LASTEXITCODE -ne 0) {
        throw "Doctor store test failed: $name"
    }
}

[pscustomobject]@{
    work_dir = $work
    doctor_store_tests = $true
} | ConvertTo-Json
