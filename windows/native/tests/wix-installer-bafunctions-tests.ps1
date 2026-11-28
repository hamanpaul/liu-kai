#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$ToolchainBin,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [Parameter(Mandatory = $true)][string]$GuardDll
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$compiler = (Resolve-Path -LiteralPath (
    Join-Path $ToolchainBin 'g++.exe')).Path
$guardDllPath = (Resolve-Path -LiteralPath $GuardDll).Path
if (-not (Test-Path -LiteralPath $guardDllPath -PathType Leaf)) {
    throw 'GuardDll must be an existing DLL file.'
}
$source = (Resolve-Path -LiteralPath (
    Join-Path $PSScriptRoot 'wix-installer-bafunctions-tests.cpp')).Path
$work = [IO.Path]::GetFullPath($WorkDir)
if (Test-Path -LiteralPath $work) {
    throw 'WorkDir must not already exist.'
}
[IO.Directory]::CreateDirectory($work) | Out-Null

$guardDllSha256 = (Get-FileHash -LiteralPath $guardDllPath -Algorithm SHA256).Hash.ToLowerInvariant()
$shaFile = Join-Path $work 'guard-dll-sha256.txt'
[IO.File]::WriteAllText(
    $shaFile,
    $guardDllSha256 + [Environment]::NewLine,
    (New-Object Text.UTF8Encoding($false)))

$windowsTargetFlags = & (Join-Path $PSScriptRoot '..\windows-target-flags.ps1')
$exe = Join-Path $work 'wix-installer-bafunctions-tests.exe'
$oldPath = $env:PATH
$process = $null
$started = $false
try {
    $env:PATH = (Split-Path -Parent $compiler) + ';' + $oldPath
    & $compiler @windowsTargetFlags `
        -std=c++17 -O2 -static -municode `
        $source -lole32 -loleaut32 -luuid -ladvapi32 -o $exe
    if ($LASTEXITCODE -ne 0) {
        throw 'WiX BAFunctions fixture compilation failed.'
    }
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw 'WiX BAFunctions fixture was not produced.'
    }

    $stdout = Join-Path $work 'stdout.txt'
    $stderr = Join-Path $work 'stderr.txt'
    $exitcode = Join-Path $work 'exitcode.txt'
    $timedOut = $false
    $stdoutTask = $null
    $stderrTask = $null
    $stdoutText = ''
    $stderrText = ''

    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $exe
    $startInfo.Arguments = '--controller "' + $guardDllPath + '"'
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $startInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw 'WiX BAFunctions native test process did not start.'
    }
    $started = $true
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(120000)) {
        $timedOut = $true
        $process.Kill()
        $process.WaitForExit(5000) | Out-Null
    }
    if (-not $stdoutTask.Wait(10000) -or -not $stderrTask.Wait(10000)) {
        throw 'WiX BAFunctions native test streams did not close.'
    }
    $stdoutText = [string]$stdoutTask.Result
    $stderrText = [string]$stderrTask.Result
    $utf8 = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($stdout, $stdoutText, $utf8)
    [IO.File]::WriteAllText($stderr, $stderrText, $utf8)
    $code = $process.ExitCode
    if ($null -eq $code) {
        throw 'WiX BAFunctions native test process returned no exit code.'
    }
    [IO.File]::WriteAllText($exitcode, [string]$code, $utf8)
    if ($timedOut) {
        throw 'WiX BAFunctions native tests timed out after 120 seconds; the controller job owns cleanup of its fixture subtree.'
    }
    if ($code -ne 0) {
        throw "WiX BAFunctions native tests failed with exit $code.`n$stderrText`n$stdoutText"
    }
    if ($stdoutText.IndexOf(
            'component-observed=true',
            [StringComparison]::Ordinal) -lt 0) {
        throw 'WiX BAFunctions fixture exited successfully without its component-observed marker.'
    }
} finally {
    $env:PATH = $oldPath
    if ($null -ne $process) {
        try {
            if ($started -and -not $process.HasExited) {
                $process.Kill()
                $process.WaitForExit(5000) | Out-Null
            }
        } finally {
            $process.Dispose()
        }
    }
}

[pscustomobject]@{
    'component-observed' = $true
    guard_dll = $guardDllPath
    guard_dll_sha256 = $guardDllSha256
    guard_dll_sha256_file = $shaFile
    work_dir = $work
    executable = $exe
    stdout = $stdout
    stderr = $stderr
    exitcode = $exitcode
    installer_runs = 0
    msi_actions = 0
    end_to_end_scope = 'Component observation only; this cannot replace a real MSI/Burn interrupted-recovery end-to-end test.'
    wix_installer_bafunctions_tests = $true
} | ConvertTo-Json
