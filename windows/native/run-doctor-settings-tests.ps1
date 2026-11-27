param(
    [Parameter(Mandatory=$true)][string]$ToolchainBin,
    [string]$RepoRoot = '',
    [string]$WorkDir = '',
    [string]$Case = ''
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
$source = Join-Path $native 'tests\doctor_settings_tests.cpp'
if (-not (Test-Path -LiteralPath $source)) {
    throw 'RepoRoot does not contain windows\native\tests\doctor_settings_tests.cpp.'
}
if (-not $WorkDir) {
    $WorkDir = Join-Path $env:TEMP (
        'liu-kai-doctor-settings-tests-' + [Guid]::NewGuid().ToString('N'))
}
$work = [IO.Path]::GetFullPath($WorkDir)
if (Test-Path -LiteralPath $work) {
    throw 'WorkDir must not already exist.'
}
[IO.Directory]::CreateDirectory($work) | Out-Null

$exe = Join-Path $work 'doctor_settings_tests.exe'
& $compiler @windowsTargetFlags -std=c++17 -O2 -static `
    $source `
    -lole32 -loleaut32 -luuid -lcomdlg32 -lcomctl32 -lshell32 `
    -lgdi32 -ladvapi32 -lbcrypt -lshlwapi `
    -o $exe
if ($LASTEXITCODE -ne 0) {
    throw 'Doctor settings compilation failed.'
}
if ($Case) {
    & $exe $Case
} else {
    & $exe
}
if ($LASTEXITCODE -ne 0) {
    throw 'Doctor settings test failed.'
}

[pscustomobject]@{
    work_dir = $work
    selected_case = if ($Case) { $Case } else { 'all' }
    doctor_settings_tests = $true
} | ConvertTo-Json
