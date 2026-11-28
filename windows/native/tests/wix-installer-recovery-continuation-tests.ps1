#Requires -Version 5.1

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'wix-installer-scenarios.ps1') -ContractOnly

$script:RecoveryContinuationAssertions = 0
$script:RecoveryContinuationFailures = [Collections.Generic.List[string]]::new()
function Assert-RecoveryContinuation {
    param([bool]$Condition, [string]$Message)
    $script:RecoveryContinuationAssertions++
    if (-not $Condition) { $script:RecoveryContinuationFailures.Add($Message) | Out-Null }
}
function Copy-RecoveryContinuation {
    param($Value)
    return ($Value | ConvertTo-Json -Depth 16 | ConvertFrom-Json)
}
function New-RecoveryContinuationProof {
    param([string]$BeforeBoot, [string]$AfterBoot, [string]$Observed, [string]$Ended)
    return [pscustomobject]@{
        owned_sandbox_id = 'synthetic-owned-sandbox'
        sentinel_path = 'C:\LiuKaiOwned\sentinel.txt'
        sentinel_sha256 = ('a' * 64)
        before_boot_utc = $BeforeBoot
        after_boot_utc = $AfterBoot
        observed_utc = $Observed
        process_ended_utc = $Ended
    }
}
function New-RecoveryContinuationEvidence {
    return [pscustomobject]@{
        evidence_class = 'synthetic_contract_only'
        first_msi_commits = 1
        restart_required = $true
        burn_version = '70.1.2'
        msi_product_version = '70.1.2'
        readback = [pscustomobject]@{
            expected_runtime_version = '0.1.2'
            expected_msi_version = '70.1.2'
            observed_runtime_version = '0.1.2'
            observed_payload_version = '0.1.2'
            observed_com_version = '0.1.2'
            manifest_integrity_ok = $true
            registration_status_ok = $true
            new_bundle_registered = $true
            old_bundle_and_msi_registrations_absent = $true
            settings_preserved = $true
            doctor_preserved = $true
            injection_attempts = 1
            injection_origin = 'candidate_ba_ui'
            bundle_consistent_at_interrupt = $false
            normal_ui_rerun_mode = 'normal_double_click'
            rerun_arguments = @()
            rerun_converged = $true
            uninstall_succeeded = $true
            owned_sandbox_id = 'synthetic-owned-sandbox'
            sentinel_path = 'C:\LiuKaiOwned\sentinel.txt'
            sentinel_sha256 = ('a' * 64)
            recovery_process = [pscustomobject]@{
                exited = $true; exit_code = -2147024664
                ended_utc = '2026-11-28T01:00:10Z'
            }
            recovery_reboot = New-RecoveryContinuationProof `
                '2026-11-28T00:00:00Z' '2026-11-28T01:01:00Z' `
                '2026-11-28T01:01:10Z' '2026-11-28T01:00:10Z'
            normal_rerun_process = [pscustomobject]@{
                exited = $true; exit_code = 3010
                started_utc = '2026-11-28T01:02:00Z'; ended_utc = '2026-11-28T01:03:00Z'
            }
            normal_rerun_reboot = New-RecoveryContinuationProof `
                '2026-11-28T01:01:00Z' '2026-11-28T01:04:00Z' `
                '2026-11-28T01:04:10Z' '2026-11-28T01:03:00Z'
            normal_uninstall_process = [pscustomobject]@{
                exited = $true; exit_code = '0x0BC2'
                started_utc = '2026-11-28T01:05:00Z'; ended_utc = '2026-11-28T01:06:00Z'
            }
            normal_uninstall_reboot = New-RecoveryContinuationProof `
                '2026-11-28T01:04:00Z' '2026-11-28T01:07:00Z' `
                '2026-11-28T01:07:10Z' '2026-11-28T01:06:00Z'
        }
    }
}
function Assert-RecoveryContinuationVerdict {
    param($Evidence, [string]$Expected, [string]$Message)
    $verdict = Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $Evidence
    Assert-RecoveryContinuation ($verdict.verdict -ceq $Expected) (
        "$Message (expected $Expected, actual $($verdict.verdict)).")
    Assert-RecoveryContinuation (-not $verdict.actual_scenario_pass) 'Synthetic evidence became an actual scenario PASS.'
    Assert-RecoveryContinuation ($verdict.receipt_status -ceq 'SYNTHETIC_CONTRACT_ONLY') 'Synthetic continuation lost its evidence label.'
}

$complete = New-RecoveryContinuationEvidence
$originalJson = $complete | ConvertTo-Json -Depth 16
Assert-RecoveryContinuationVerdict $complete PASS 'Three independently observed stage reboots did not complete recovery'
Assert-RecoveryContinuation (($complete | ConvertTo-Json -Depth 16) -ceq $originalJson) 'Verdict mutated original evidence or native exits.'

foreach ($stage in @('recovery', 'normal_rerun', 'normal_uninstall')) {
    $single = New-RecoveryContinuationEvidence
    $single.restart_required = $stage -ceq 'recovery'
    if ($stage -cne 'normal_rerun') { $single.readback.normal_rerun_process.exit_code = 0 }
    if ($stage -cne 'normal_uninstall') { $single.readback.normal_uninstall_process.exit_code = 0 }
    Assert-RecoveryContinuationVerdict $single PASS "$stage reboot did not discharge its own restart requirement"

    $proofName = $stage + '_reboot'
    $processName = $stage + '_process'
    foreach ($bad in @(
        @{ name = 'missing proof'; field = ''; value = $null },
        @{ name = 'boolean bypass'; field = ''; value = [pscustomobject]@{ verified = $true } },
        @{ name = 'wrong sandbox'; field = 'owned_sandbox_id'; value = 'another-sandbox' },
        @{ name = 'wrong sentinel'; field = 'sentinel_path'; value = 'C:\Other\sentinel.txt' },
        @{ name = 'wrong hash'; field = 'sentinel_sha256'; value = ('b' * 64) },
        @{ name = 'stale boot'; field = 'after_boot_utc'; value = '2026-11-28T00:00:00Z' },
        @{ name = 'future boot'; field = 'after_boot_utc'; value = '2026-11-28T02:00:00Z' },
        @{ name = 'invalid date'; field = 'before_boot_utc'; value = 'unknown' },
        @{ name = 'wrong process'; field = 'process_ended_utc'; value = '2026-11-28T00:00:00Z' }
    )) {
        $changed = Copy-RecoveryContinuation $single
        if ($bad.field -ceq '') { $changed.readback.$proofName = $bad.value }
        else { $changed.readback.$proofName.PSObject.Properties[$bad.field].Value = $bad.value }
        Assert-RecoveryContinuationVerdict $changed NOT_COVERED "$stage accepted $($bad.name)"
    }
    foreach ($field in @('owned_sandbox_id', 'sentinel_path', 'sentinel_sha256', 'before_boot_utc',
                          'after_boot_utc', 'observed_utc', 'process_ended_utc')) {
        $changed = Copy-RecoveryContinuation $single
        $changed.readback.$proofName.PSObject.Properties.Remove($field)
        Assert-RecoveryContinuationVerdict $changed NOT_COVERED "$stage accepted missing $field"
    }
    foreach ($field in @('exited', 'exit_code', 'ended_utc')) {
        $changed = Copy-RecoveryContinuation $single
        $changed.readback.$processName.PSObject.Properties.Remove($field)
        Assert-RecoveryContinuationVerdict $changed NOT_COVERED "$stage accepted missing native process $field"
    }
    $changed = Copy-RecoveryContinuation $single
    $changed.readback.$processName.exited = 'true'
    Assert-RecoveryContinuationVerdict $changed NOT_COVERED "$stage accepted a string exited flag"
}

foreach ($case in @(
    @{ process = 'normal_rerun_process'; start = '2026-11-28T01:00:00Z' },
    @{ process = 'normal_uninstall_process'; start = '2026-11-28T01:03:30Z' }
)) {
    $changed = New-RecoveryContinuationEvidence
    $changed.readback.PSObject.Properties[$case.process].Value.started_utc = $case.start
    Assert-RecoveryContinuationVerdict $changed NOT_COVERED 'The next installer ran before its required guest reboot'
}
foreach ($field in @('normal_rerun_process', 'normal_uninstall_process')) {
    $changed = New-RecoveryContinuationEvidence
    $changed.readback.$field.exit_code = 1603
    Assert-RecoveryContinuationVerdict $changed FAIL "$field failure was excused by reboot proof"
}
$changed = New-RecoveryContinuationEvidence
$changed.readback.registration_status_ok = $false
Assert-RecoveryContinuationVerdict $changed FAIL 'Reboot proof bypassed existing registration validation'
$changed = New-RecoveryContinuationEvidence
$changed.readback.rerun_arguments = @('/repair')
Assert-RecoveryContinuationVerdict $changed FAIL 'Reboot proof bypassed ordinary zero-argument UI validation'
$changed = New-RecoveryContinuationEvidence
$changed.first_msi_commits = 0
Assert-RecoveryContinuationVerdict $changed NOT_COVERED 'Reboot proof invented a real MSI commit'

$helper = Get-Command -Name Test-WixInstallerScenarioRecoveryRebootProof -CommandType Function -ErrorAction SilentlyContinue
Assert-RecoveryContinuation ($null -ne $helper) 'Pure recovery reboot proof helper is missing.'
if ($null -ne $helper) {
    $readback = (New-RecoveryContinuationEvidence).readback
    Assert-RecoveryContinuation (Test-WixInstallerScenarioRecoveryRebootProof `
        -Readback $readback -Process $readback.recovery_process -Reboot $readback.recovery_reboot) 'Exact reboot proof was rejected.'
    foreach ($field in @('owned_sandbox_id', 'sentinel_path', 'sentinel_sha256')) {
        $changed = Copy-RecoveryContinuation $readback
        $changed.PSObject.Properties.Remove($field)
        Assert-RecoveryContinuation (-not (Test-WixInstallerScenarioRecoveryRebootProof `
            -Readback $changed -Process $changed.recovery_process -Reboot $changed.recovery_reboot)) "Missing identity anchor $field was accepted."
    }
}

if ($script:RecoveryContinuationFailures.Count -gt 0) {
    throw ('[SRC001] Recovery continuation contract failed: ' + ($script:RecoveryContinuationFailures -join ' | '))
}
"wix_installer_recovery_continuation_passed=true; assertions=$script:RecoveryContinuationAssertions; installer_runs=0; kill_actions=0; registry_writes=0"
