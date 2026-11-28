#Requires -Version 5.1

. (Join-Path $PSScriptRoot 'wix-installer-recovery-continuation-tests.ps1') | Out-Null
$script:RetainedAssertions = 0
$script:RetainedFailures = [Collections.Generic.List[string]]::new()
function Assert-RetainedRecovery {
    param([bool]$Condition, [string]$Message)
    $script:RetainedAssertions++
    if (-not $Condition) { $script:RetainedFailures.Add($Message) | Out-Null }
}
function Invoke-RetainedRecoveryVerdict {
    param($Evidence, [switch]$AcceptRetainedBundle)
    $command = Get-Command Get-WixInstallerScenarioVerdict -CommandType Function
    if ($AcceptRetainedBundle -and $command.Parameters.ContainsKey('AcceptRetainedBundle')) {
        return Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $Evidence -AcceptRetainedBundle
    }
    return Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $Evidence
}

$retained = New-RecoveryContinuationEvidence
$retained.readback.bundle_consistent_at_interrupt = $true
$retained.readback | Add-Member -NotePropertyName verified_ba_stop -NotePropertyValue $true
$retained.readback | Add-Member -NotePropertyName original_engine_exited -NotePropertyValue $true
$original = $retained | ConvertTo-Json -Depth 16
$strict = Invoke-RetainedRecoveryVerdict $retained
Assert-RetainedRecovery ($strict.verdict -ceq 'NOT_COVERED') 'Default acceptance admitted a retained Full bundle.'
Assert-RetainedRecovery ((Get-WixInstallerScenarioProperty $strict 'acceptance_mode') -ceq 'repair_required') 'Default Recovery acceptance mode was not explicit.'
$accepted = Invoke-RetainedRecoveryVerdict $retained -AcceptRetainedBundle
Assert-RetainedRecovery ($accepted.verdict -ceq 'PASS') 'Explicit retained-bundle acceptance rejected complete proof.'
Assert-RetainedRecovery ((Get-WixInstallerScenarioProperty $accepted 'acceptance_mode') -ceq 'retained_or_repaired') 'Opt-in Recovery acceptance mode was not explicit.'
Assert-RetainedRecovery ($accepted.reason -match 'retained') 'Retained-bundle PASS was described as a repair.'
Assert-RetainedRecovery (-not $accepted.actual_scenario_pass -and $accepted.receipt_status -ceq 'SYNTHETIC_CONTRACT_ONLY') 'Synthetic retained-bundle evidence became an actual PASS.'
Assert-RetainedRecovery (($retained | ConvertTo-Json -Depth 16) -ceq $original) 'Opt-in acceptance rewrote original evidence.'

foreach ($field in @('verified_ba_stop', 'original_engine_exited')) {
    foreach ($bad in @($false, $null, 'true', 1)) {
        $changed = Copy-RecoveryContinuation $retained
        $changed.readback.$field = $bad
        Assert-RetainedRecovery ((Invoke-RetainedRecoveryVerdict $changed -AcceptRetainedBundle).verdict -ceq 'NOT_COVERED') "Retained bundle accepted invalid $field proof."
    }
    $changed = Copy-RecoveryContinuation $retained
    $changed.readback.PSObject.Properties.Remove($field)
    Assert-RetainedRecovery ((Invoke-RetainedRecoveryVerdict $changed -AcceptRetainedBundle).verdict -ceq 'NOT_COVERED') "Retained bundle accepted missing $field proof."
}
foreach ($bad in @($null, 'true', 1)) {
    $changed = Copy-RecoveryContinuation $retained
    $changed.readback.bundle_consistent_at_interrupt = $bad
    Assert-RetainedRecovery ((Invoke-RetainedRecoveryVerdict $changed -AcceptRetainedBundle).verdict -ceq 'NOT_COVERED') 'Retained bundle accepted unknown or coerced consistency.'
}
foreach ($field in @('normal_rerun_process', 'normal_uninstall_process')) {
    $changed = Copy-RecoveryContinuation $retained
    $changed.readback.$field.exit_code = 1603
    Assert-RetainedRecovery ((Invoke-RetainedRecoveryVerdict $changed -AcceptRetainedBundle).verdict -ceq 'FAIL') "Retained acceptance bypassed $field native failure."
}
$changed = Copy-RecoveryContinuation $retained
$changed.readback.registration_status_ok = $false
Assert-RetainedRecovery ((Invoke-RetainedRecoveryVerdict $changed -AcceptRetainedBundle).verdict -ceq 'FAIL') 'Retained acceptance bypassed final registration failure.'
$changed = Copy-RecoveryContinuation $retained
$changed.readback.normal_uninstall_reboot = $null
Assert-RetainedRecovery ((Invoke-RetainedRecoveryVerdict $changed -AcceptRetainedBundle).verdict -ceq 'NOT_COVERED') 'Retained acceptance bypassed uninstall 3010 reboot proof.'
$repaired = Invoke-RetainedRecoveryVerdict (New-RecoveryContinuationEvidence) -AcceptRetainedBundle
Assert-RetainedRecovery ($repaired.verdict -ceq 'PASS' -and $repaired.reason -match 'repair') 'Opt-in broke strict repaired-bundle acceptance or its description.'
$upgrade = Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence (New-RecoveryContinuationEvidence)
Assert-RetainedRecovery (-not (Test-WixInstallerScenarioProperty $upgrade 'acceptance_mode')) 'Recovery acceptance mode leaked into Upgrade.'
Assert-RetainedRecovery ((Get-Command Get-WixInstallerScenarioVerdict).Parameters.ContainsKey('AcceptRetainedBundle')) 'Explicit retained-bundle opt-in switch is missing.'

if ($script:RetainedFailures.Count -gt 0) {
    throw ('[SRB001] Retained recovery contract failed: ' + ($script:RetainedFailures -join ' | '))
}
"wix_installer_retained_recovery_passed=true; assertions=$script:RetainedAssertions; installer_runs=0; kill_actions=0; registry_writes=0"
