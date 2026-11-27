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
$hostSource = Join-Path $native 'tests\doctor_host_tests.cpp'
$installerSource = Join-Path $native 'tests\doctor_installer_tests.cpp'
if (-not (Test-Path -LiteralPath $hostSource) -or
    -not (Test-Path -LiteralPath $installerSource)) {
    throw 'RepoRoot does not contain the Doctor host and installer tests.'
}
if (-not $WorkDir) {
    $WorkDir = Join-Path $env:TEMP (
        'liu-kai-doctor-installer-tests-' +
        [Guid]::NewGuid().ToString('N'))
}
$work = [IO.Path]::GetFullPath($WorkDir)
if (Test-Path -LiteralPath $work) {
    throw 'WorkDir must not already exist.'
}
[IO.Directory]::CreateDirectory($work) | Out-Null

$hostExe = Join-Path $work 'doctor_host_tests.exe'
& $compiler @windowsTargetFlags -std=c++17 -O2 -static `
    $hostSource `
    -lole32 -loleaut32 -luuid -lgdi32 -ladvapi32 -lbcrypt -lshlwapi `
    -lmsi -luserenv -lwtsapi32 `
    -o $hostExe
if ($LASTEXITCODE -ne 0) {
    throw 'Doctor host compilation failed.'
}

$installerExe = Join-Path $work 'doctor_installer_tests.exe'
& $compiler @windowsTargetFlags -std=c++17 -O2 -static `
    $installerSource -municode `
    -lole32 -loleaut32 -luuid -lgdi32 -ladvapi32 -lbcrypt -lshlwapi `
    -lmsi -luserenv -lwtsapi32 `
    -o $installerExe
if ($LASTEXITCODE -ne 0) {
    throw 'Doctor installer compilation failed.'
}

if ($Case) {
    & $installerExe $hostExe $Case
} else {
    & $installerExe $hostExe
}
if ($LASTEXITCODE -ne 0) {
    throw 'Doctor installer test failed.'
}

[pscustomobject]@{
    work_dir = $work
    selected_case = if ($Case) { $Case } else { 'all' }
    doctor_installer_tests = $true
} | ConvertTo-Json
