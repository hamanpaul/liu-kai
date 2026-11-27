param(
    [Parameter(Mandatory=$true)][string]$ToolchainBin,
    [Parameter(Mandatory=$true)][string]$WorkDir
)
$ErrorActionPreference='Stop'
$compiler=(Resolve-Path (Join-Path $ToolchainBin 'g++.exe')).Path
$work=[IO.Path]::GetFullPath($WorkDir)
if(Test-Path $work){throw 'Target test directory already exists.'}
[IO.Directory]::CreateDirectory($work)|Out-Null
$source=Join-Path $work 'windows-target.cpp'
[IO.File]::WriteAllText($source,@'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <securityappcontainer.h>
static_assert(_WIN32_WINNT >= 0x0A00, "Windows 10 is the product baseline");
static_assert(NTDDI_VERSION >= 0x0A000000, "SDK declarations must match the baseline");
int main() { auto api = &GetAppContainerNamedObjectPath; return api ? 0 : 1; }
'@,[Text.UTF8Encoding]::new($false))
$oldTarget=Join-Path $work 'old-target.exe'
$previousPreference=$ErrorActionPreference
try {
    $ErrorActionPreference='Continue'
    & $compiler -std=c++17 -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000 $source -o $oldTarget 2> (Join-Path $work 'expected-old-target-error.log')
    $oldExit=$LASTEXITCODE
} finally { $ErrorActionPreference=$previousPreference }
if($oldExit -eq 0){throw 'Regression fixture unexpectedly accepted the old SDK target.'}
if((Get-Content (Join-Path $work 'expected-old-target-error.log') -Raw) -notmatch 'GetAppContainerNamedObjectPath'){throw 'Old-target failure did not reproduce the API declaration error.'}
$flags=& (Join-Path $PSScriptRoot '..\windows-target-flags.ps1')
$exe=Join-Path $work 'supported-target.exe'
& $compiler @flags -std=c++17 -static $source -o $exe
if($LASTEXITCODE -ne 0){throw 'Supported Windows target compilation failed.'}
& $exe
if($LASTEXITCODE -ne 0){throw 'Supported Windows API import test failed.'}
Write-Output 'Explicit Windows 10 target and AppContainer API import passed'
