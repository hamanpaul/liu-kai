#Requires -Version 5.1

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'wix-installer-scenarios.ps1') -ContractOnly

$script:PostExitAssertions = 0
$script:PostExitFailures = [Collections.Generic.List[string]]::new()
function Assert-PostExit {
    param([bool]$Condition, [string]$Message)
    $script:PostExitAssertions++
    if (-not $Condition) { $script:PostExitFailures.Add($Message) | Out-Null }
}
function New-PostExitMsiLog {
    param([string]$Text, [string]$Run = 'first')
    return [pscustomobject]@{ run = $Run; kind = 'msi'; path = 'synthetic-candidate-msi.log'; text = $Text }
}
function Read-PostExitEvidence {
    param([object[]]$Logs)
    return ConvertFrom-WixInstallerScenarioLogs -Scenario Recovery -BurnLogs @() `
        -MsiLogs $Logs -Readback ([pscustomobject]@{ expected_msi_version = '70.1.2' }) `
        -EvidenceClass synthetic_contract_only
}
function Read-PostExitState {
    param($Evidence)
    if (Get-Command Get-WixInstallerScenarioRecoveryPostExitState -CommandType Function -ErrorAction SilentlyContinue) {
        return Get-WixInstallerScenarioRecoveryPostExitState -Evidence $Evidence
    }
    return [pscustomobject]@{ known = $false; native_exit = $null; restart_required = $null }
}

$partial = @'
MSI (s) (AA:AA) [13:00:00]: ProductVersion = 70.1.2
Action ended 13:00:01: InstallFinalize. Return value 1.
MSI (s) (AA:AA) [13:00:02]: Windows Installer installed the product. Product Name: Synthetic LiuKai. Product Version: 70.1.2. Product Language: 1033. Manufacturer: LiuKai. Installation success or error status: 0.
'@
$native3010 = 'MSI (s) (AA:AA) [13:00:03]: MainEngineThread is returning 3010'
$native0 = 'MSI (s) (AA:AA) [13:00:04]: MainEngineThread is returning 0'
$oldTransaction = @'
MSI (s) (BB:BB) [12:00:00]: ProductVersion = 70.1.1
Action ended 12:00:01: InstallFinalize. Return value 1.
MSI (s) (BB:BB) [12:00:02]: Windows Installer installed the product. Product Name: Synthetic LiuKai. Product Version: 70.1.1. Product Language: 1033. Manufacturer: LiuKai. Installation success or error status: 3010.
MSI (s) (BB:BB) [12:00:03]: MainEngineThread is returning 3010
'@

$early = Read-PostExitEvidence @(New-PostExitMsiLog $partial)
$earlyJson = $early | ConvertTo-Json -Depth 12
Assert-PostExit ($early.first_msi_commits -eq 1) 'The early observation lost the real InstallFinalize commit.'
Assert-PostExit ($early.restart_required -eq $false) 'The legacy parser changed summary-zero semantics.'
Assert-PostExit (Test-WixInstallerScenarioProperty $early 'first_msi_native_exit') 'Parser does not preserve first_msi_native_exit.'
Assert-PostExit ($null -eq (Get-WixInstallerScenarioProperty $early 'first_msi_native_exit')) 'Partial MSI log invented a native result.'
$state = Read-PostExitState $early
Assert-PostExit (-not $state.known -and $null -eq $state.restart_required) 'Summary 0 without a native result allowed recovery to continue.'

$late = Read-PostExitEvidence @(New-PostExitMsiLog ($partial + "`r`n" + $native3010))
Assert-PostExit ((Get-WixInstallerScenarioProperty $late 'first_msi_native_exit') -ceq '3010') 'Post-exit parser missed appended native 3010.'
Assert-PostExit ($late.first_msi_commits -eq 1) 'Re-reading one complete log duplicated the candidate commit.'
$state = Read-PostExitState $late
Assert-PostExit ($state.known -and $state.restart_required -eq $true -and $state.native_exit -ceq '3010') 'Appended native 3010 was not a known reboot requirement.'

$lastZero = Read-PostExitEvidence @(New-PostExitMsiLog ($partial + "`r`n" + $native3010 + "`r`n" + $native0))
$state = Read-PostExitState $lastZero
Assert-PostExit ((Get-WixInstallerScenarioProperty $lastZero 'first_msi_native_exit') -ceq '0') 'The last native return did not take precedence.'
Assert-PostExit ($state.known -and $state.restart_required -eq $false -and $state.native_exit -ceq '0') 'Final native 0 was not a known non-reboot result.'

$sameLogPartial = Read-PostExitEvidence @(New-PostExitMsiLog ($oldTransaction + "`r`n" + $partial))
Assert-PostExit ($null -eq (Get-WixInstallerScenarioProperty $sameLogPartial 'first_msi_native_exit')) 'An older transaction native return contaminated an unfinished candidate in the same log.'
Assert-PostExit (-not (Read-PostExitState $sameLogPartial).known) 'An older transaction allowed a partial candidate to continue.'
$sameLogComplete = Read-PostExitEvidence @(New-PostExitMsiLog ($oldTransaction + "`r`n" + $partial + "`r`n" + $native0))
Assert-PostExit ((Get-WixInstallerScenarioProperty $sameLogComplete 'first_msi_native_exit') -ceq '0') 'An older same-log native 3010 overrode candidate native 0.'
$followingOld = Read-PostExitEvidence @(New-PostExitMsiLog ($partial + "`r`n" + $oldTransaction))
Assert-PostExit ($null -eq (Get-WixInstallerScenarioProperty $followingOld 'first_msi_native_exit')) 'A following foreign transaction supplied the missing candidate native exit.'
Assert-PostExit (-not (Read-PostExitState $followingOld).known) 'A following foreign transaction allowed a partial candidate to continue.'

$separateOld = Read-PostExitEvidence @(
    (New-PostExitMsiLog ($partial + "`r`n" + $native0)),
    (New-PostExitMsiLog $oldTransaction)
)
Assert-PostExit ($separateOld.first_msi_commits -eq 1) 'A historical MSI became a second candidate commit.'
Assert-PostExit ((Get-WixInstallerScenarioProperty $separateOld 'first_msi_native_exit') -ceq '0') 'A historical MSI log changed candidate native 0.'
Assert-PostExit ((Read-PostExitState $separateOld).known -and -not (Read-PostExitState $separateOld).restart_required) 'Historical native 3010 polluted the current post-exit decision.'

foreach ($unusable in @(
    (Read-PostExitEvidence @(New-PostExitMsiLog ($partial + "`r`nMSI (s) (AA:AA) [13:00:05]: MainEngineThread is returning 1603"))),
    (Read-PostExitEvidence @(New-PostExitMsiLog ($partial + "`r`n" + $native0) 'second')),
    (Read-PostExitEvidence @(New-PostExitMsiLog (($partial -replace 'Return value 1\.', 'Return value 3.') + "`r`n" + $native0)))
)) {
    $state = Read-PostExitState $unusable
    Assert-PostExit (-not $state.known -and $null -eq $state.restart_required) 'Unusable, failed, or unrelated native MSI evidence allowed recovery to continue.'
}
Assert-PostExit (($early | ConvertTo-Json -Depth 12) -ceq $earlyJson) 'Reading post-exit evidence rewrote the first observation.'
Assert-PostExit ($null -ne (Get-Command Get-WixInstallerScenarioRecoveryPostExitState -CommandType Function -ErrorAction SilentlyContinue)) 'Pure post-exit decision helper is missing.'

if ($script:PostExitFailures.Count -gt 0) {
    throw ('[SPE001] Recovery post-exit contract failed: ' + ($script:PostExitFailures -join ' | '))
}
"wix_installer_recovery_post_exit_passed=true; assertions=$script:PostExitAssertions; installer_runs=0; kill_actions=0; registry_writes=0"
