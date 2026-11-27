param(
    [Parameter(Mandatory=$true)][string]$ToolchainBin,
    [Parameter(Mandatory=$true)][string]$WorkDir,
    [switch]$UseEphemeralTestUser
)
$ErrorActionPreference = 'Stop'
$work = [IO.Path]::GetFullPath($WorkDir)
if (Test-Path -LiteralPath $work) { throw 'Doctor test directory already exists.' }
[IO.Directory]::CreateDirectory($work) | Out-Null

if ($UseEphemeralTestUser) {
    # Hosted CI runs elevated. Exercise the real non-administrator boundary in
    # a disposable local account rather than weakening the product or tests.
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Creating the disposable CI test account requires an elevated CI runner.'
    }
    $name = 'LKDoc_' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $secret = ConvertTo-SecureString ('aA!' + [Guid]::NewGuid().ToString('N')) -AsPlainText -Force
    $created = $false
    try {
        New-LocalUser -Name $name -Password $secret -AccountNeverExpires -PasswordNeverExpires -UserMayNotChangePassword | Out-Null
        $created = $true
        $sid = (Get-LocalUser -Name $name).SID.Value
        if (-not (Get-LocalGroupMember -SID 'S-1-5-32-545' | Where-Object { $_.SID.Value -eq $sid })) {
            Add-LocalGroupMember -SID 'S-1-5-32-545' -Member $name
        }
        & icacls.exe $work /grant (('*' + $sid) + ':(OI)(CI)M') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Could not grant the test account access to its own output folder.' }
        $credential = [PSCredential]::new(($env:COMPUTERNAME + '\' + $name), $secret)
        $script = Join-Path $PSScriptRoot 'run-doctor-tests.ps1'
        $bin = (Resolve-Path -LiteralPath $ToolchainBin).Path
        foreach ($path in @($script, $bin, $work)) {
            if ($path.Contains('"')) { throw 'Unexpected quote in a test path.' }
        }
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $script + '"'),
            '-ToolchainBin', ('"' + $bin + '"'), '-WorkDir', ('"' + (Join-Path $work 'normal-user') + '"'))
        $powershell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $process = Start-Process -FilePath $powershell -ArgumentList $arguments `
            -Credential $credential -LoadUserProfile -WorkingDirectory $work -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $work 'stdout.log') -RedirectStandardError (Join-Path $work 'stderr.log') `
            -Wait -PassThru
        Get-Content -LiteralPath (Join-Path $work 'stdout.log')
        Get-Content -LiteralPath (Join-Path $work 'stderr.log')
        if ($process.ExitCode -ne 0) { throw "Doctor standard-user tests failed: $($process.ExitCode)" }
        # A standard account may lack symlink creation permission. Exercise
        # the real link fixture from this elevated disposable-test entry too;
        # the selected case fails rather than skipping if it cannot create one.
        $storeTest = Join-Path $work 'normal-user\store\doctor_store_tests.exe'
        $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
        & $storeTest $repoRoot 'clear-staging-symlink'
        if ($LASTEXITCODE -ne 0) { throw 'Required Doctor symlink fixture failed.' }
    } finally {
        if ($created) { Remove-LocalUser -Name $name }
        $secret.Dispose()
    }
    return
}

$bin = (Resolve-Path -LiteralPath $ToolchainBin).Path
# Start-Process -Credential uses the test account's environment rather than the
# caller's PATH. GCC's helper executables also need this directory for MSYS2 DLLs.
$env:PATH = $bin + ';' + $env:PATH
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
try {
    $scratch = Join-Path $work 'temp'
    [IO.Directory]::CreateDirectory($scratch) | Out-Null
    $env:TEMP = $scratch
    $env:TMP = $scratch
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $PSScriptRoot 'run-doctor-p0-tests.ps1') `
        -ToolchainBin $bin -WorkDir (Join-Path $work 'p0')
    if ($LASTEXITCODE -ne 0) { throw 'Doctor transport and TSF tests failed.' }
    foreach ($suite in @('store', 'runtime', 'host', 'settings', 'installer')) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $PSScriptRoot ("run-doctor-$suite-tests.ps1")) `
            -ToolchainBin $bin -RepoRoot $repo -WorkDir (Join-Path $work $suite)
        if ($LASTEXITCODE -ne 0) { throw "Doctor $suite tests failed." }
    }
} finally {
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
}
Write-Output 'doctor_standard_user_tests_passed=true'
