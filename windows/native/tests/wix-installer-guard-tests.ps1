param(
    [Parameter(Mandatory = $true)][string]$ToolchainBin,
    [Parameter(Mandatory = $true)][string]$WorkDir
)

$ErrorActionPreference = 'Stop'
$compiler = (Resolve-Path -LiteralPath (Join-Path $ToolchainBin 'g++.exe')).Path
$source = (Resolve-Path -LiteralPath (
    Join-Path $PSScriptRoot 'wix-installer-guard-tests.cpp')).Path
$work = [IO.Path]::GetFullPath($WorkDir)
if (Test-Path -LiteralPath $work) {
    throw 'WorkDir must not already exist.'
}
[IO.Directory]::CreateDirectory($work) | Out-Null

$exe = Join-Path $work 'wix-installer-guard-tests.exe'
& $compiler -std=c++17 -municode -static-libgcc -static-libstdc++ `
    $source -ladvapi32 -o $exe
if ($LASTEXITCODE -ne 0) {
    throw 'WiX installer guard test compilation failed. The intended RED is a missing installer-session-guard.hpp or product API.'
}

$stdout = Join-Path $work 'stdout.txt'
$stderr = Join-Path $work 'stderr.txt'
$exitcode = Join-Path $work 'exitcode.txt'
$process = $null
$timedOut = $false
try {
    # Keep the handle from Process.Start itself. PS5 Start-Process can return
    # a lazy Process wrapper whose ExitCode is null for a short-lived child.
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $exe
    $startInfo.Arguments = '--controller'
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $startInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw 'WiX installer guard native test process did not start.'
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(90000)) {
        $timedOut = $true
        $process.Kill()
        $process.WaitForExit(5000) | Out-Null
    }
    if (-not $stdoutTask.Wait(5000) -or -not $stderrTask.Wait(5000)) {
        throw 'WiX installer guard test streams did not close after process exit.'
    }
    [IO.File]::WriteAllText($stdout, $stdoutTask.Result, (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText($stderr, $stderrTask.Result, (New-Object Text.UTF8Encoding($false)))
    $code = $process.ExitCode
    if ($null -eq $code) {
        throw 'WiX installer guard test process returned no exit code.'
    }
    [IO.File]::WriteAllText($exitcode, [string]$code, (New-Object Text.UTF8Encoding($false)))
    if ($timedOut) {
        throw 'WiX installer guard native tests timed out after 90 seconds.'
    }
    if ($code -ne 0) {
        $details = ''
        if (Test-Path -LiteralPath $stderr) {
            $details = [IO.File]::ReadAllText($stderr)
        }
        throw "WiX installer guard native tests failed with exit $code.`n$details"
    }
} finally {
    if ($null -ne $process) {
        try {
            if (-not $process.HasExited) {
                $process.Kill()
                $process.WaitForExit(5000) | Out-Null
            }
        } finally {
            $process.Dispose()
        }
    }
}

[pscustomobject]@{
    work_dir = $work
    executable = $exe
    stdout = $stdout
    stderr = $stderr
    exitcode = $exitcode
    wix_installer_guard_tests = $true
} | ConvertTo-Json
