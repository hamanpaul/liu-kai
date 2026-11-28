#Requires -Version 5.1

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Load only pure helpers. Never invoke the runtime harness or an installer.
$harness = Join-Path $PSScriptRoot 'wix-installer-scenarios.ps1'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    $harness, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw '[BUNDLE000] Harness syntax errors.' }
foreach ($name in @(
    'Get-WixInstallerScenarioProperty',
    'Test-WixInstallerScenarioProperty',
    'Test-WixInstallerScenarioBundleMatches'
)) {
    $definition = $ast.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq $name
    }, $false)
    if ($null -eq $definition) { throw "[BUNDLE000] Missing pure helper: $name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}

$script:Assertions = 0
$script:Failures = [Collections.Generic.List[string]]::new()
$identity = [pscustomobject]@{ msi_version = '70.1.4' }

function New-BundleSnapshot {
    return [pscustomobject]@{
        uninstall_registry_issues = @()
        uninstall_entries = @([pscustomobject]@{
            kind = 'burn_bundle'
            display_version = '70.1.4'
            values = [pscustomobject]@{ Installed = 1; Resume = 3 }
        })
    }
}

function Require-BundleState {
    param($Snapshot, [AllowNull()]$Expected, [string]$Label)

    $script:Assertions++
    try {
        $actual = Test-WixInstallerScenarioBundleMatches -Snapshot $Snapshot -Identity $identity
        $matches = if ($null -eq $Expected) {
            $null -eq $actual
        }
        else {
            $actual -is [bool] -and $actual -eq $Expected
        }
        if (-not $matches) {
            $expectedText = if ($null -eq $Expected) { 'unknown' } else { [string]$Expected }
            $actualText = if ($null -eq $actual) { 'unknown' } else { [string]$actual }
            $script:Failures.Add("$Label expected=$expectedText actual=$actualText")
        }
    }
    catch {
        $script:Failures.Add("$Label threw: $($_.Exception.Message)")
    }
}

Require-BundleState (New-BundleSnapshot) $true 'Full registration with quiescent ARP resume'
foreach ($resume in @(1, 2)) {
    $snapshot = New-BundleSnapshot
    $snapshot.uninstall_entries[0].values.Resume = $resume
    Require-BundleState $snapshot $false "Full registration with interrupted/suspended Resume=$resume"
}
foreach ($resume in @(0, 4, 5, -1, 'invalid', '', $null)) {
    $snapshot = New-BundleSnapshot
    $snapshot.uninstall_entries[0].values.Resume = $resume
    Require-BundleState $snapshot $null "Unknown or reboot-pending Resume='$resume'"
}
$snapshot = New-BundleSnapshot
$snapshot.uninstall_entries[0].values.PSObject.Properties.Remove('Resume')
Require-BundleState $snapshot $null 'Missing Resume is unknown'

$snapshot = New-BundleSnapshot
$snapshot.uninstall_registry_issues = @('synthetic read failure')
Require-BundleState $snapshot $null 'Registry read failure is unknown'
foreach ($field in @('uninstall_registry_issues', 'uninstall_entries')) {
    $snapshot = New-BundleSnapshot
    $snapshot.PSObject.Properties.Remove($field)
    Require-BundleState $snapshot $null "Missing registry observation $field"
    $snapshot = New-BundleSnapshot
    $snapshot.PSObject.Properties[$field].Value = $null
    Require-BundleState $snapshot $null "Null registry observation $field"
}

$snapshot = New-BundleSnapshot
$snapshot.uninstall_entries = @()
Require-BundleState $snapshot $false 'Complete registry inventory contains no bundle'
$snapshot = New-BundleSnapshot
$snapshot.uninstall_entries[0].display_version = '70.1.3'
Require-BundleState $snapshot $false 'Complete registry inventory contains only the old bundle'
$snapshot = New-BundleSnapshot
$snapshot.uninstall_entries[0].values.Installed = 0
Require-BundleState $snapshot $false 'Candidate registration is not Full'

$dictionarySnapshot = @{
    uninstall_registry_issues = @()
    uninstall_entries = @(@{
        kind = 'burn_bundle'
        display_version = '70.1.4'
        values = @{ Installed = 1; Resume = 3 }
    })
}
Require-BundleState $dictionarySnapshot $true 'Dictionary snapshot retains observed empty issue array'
$dictionarySnapshot.uninstall_entries = @()
Require-BundleState $dictionarySnapshot $false 'Dictionary snapshot retains observed empty entry array'
$dictionarySnapshot.uninstall_registry_issues = @('synthetic read failure')
Require-BundleState $dictionarySnapshot $null 'Read failure does not prove bundle absence'

if ($script:Failures.Count -ne 0) {
    throw ("[BUNDLE900] assertions=$($script:Assertions); failures=$($script:Failures.Count)`n" +
        ($script:Failures -join "`n"))
}
Write-Output ("wix_installer_bundle_state_contracts_passed=$($script:Assertions); " +
    'evidence=synthetic_contract_only; installer_runs=0; process_actions=0; registry_actions=0')
