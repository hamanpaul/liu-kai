#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$RegistrationStatusExe,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Run only after the owned guest's ordinary uninstall. This script never
# installs, unregisters, changes product registry keys, or reads user data.
$harness = Join-Path $PSScriptRoot 'wix-installer-scenarios.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($harness, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw '[SCA000] Harness syntax errors.' }
foreach ($name in @(
    'Get-WixInstallerScenarioProperty', 'Test-WixInstallerScenarioProperty',
    'Get-WixInstallerScenarioProductRoot', 'Get-WixInstallerScenarioMsiProducts',
    'Get-WixInstallerScenarioUninstallEntries', 'Get-WixInstallerScenarioComServer',
    'Get-WixInstallerScenarioSnapshot', 'Test-WixInstallerScenarioInstalledAbsent'
)) {
    $definition = $ast.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
    }, $false)
    if ($null -eq $definition) { throw "[SCA000] Missing $name." }
    . ([scriptblock]::Create($definition.Extent.Text))
}
# Retention is unrelated to absence. Keep its private settings/Doctor data out
# of this test while using the real registry, filesystem, and status observer.
function Get-WixInstallerScenarioStateRetention {
    param([string]$FixtureExe, [string]$OutputDir)
    return [pscustomobject]@{ files = @(); fingerprint = ''; fixture = $null }
}
function Require-Absence($Condition, [string]$Message) {
    if (-not $Condition) { throw "[SCA001] $Message" }
}
$root = Get-WixInstallerScenarioProductRoot
if (Test-Path -LiteralPath $root) { throw '[SCA000] Ordinary guest uninstall must remove the product root before this test.' }
$observer = (Resolve-Path -LiteralPath $RegistrationStatusExe).Path
$work = [IO.Path]::GetFullPath($WorkDir)
if (Test-Path -LiteralPath $work) { throw '[SCA000] WorkDir must not exist.' }
[void][IO.Directory]::CreateDirectory($work)
$cached = Join-Path $work 'registration-status.exe'
[IO.File]::Copy($observer, $cached, $false)
$hash = (Get-FileHash -LiteralPath $cached -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($cached + '.sha256'), $hash, (New-Object Text.UTF8Encoding($false)))

$snapshot = Get-WixInstallerScenarioSnapshot -StateFixture $observer -OutputDir $work
foreach ($field in @('registration_absent', 'product_directory_absent', 'owned_pending_deletes', 'owned_retired_files')) {
    Require-Absence (Test-WixInstallerScenarioProperty $snapshot $field) "Runtime snapshot did not observe $field."
}
Require-Absence ((Test-WixInstallerScenarioInstalledAbsent $snapshot) -eq $true) 'Real post-uninstall snapshot did not prove complete absence.'
Require-Absence ($snapshot.registration.profile.absent -eq $true) 'Native TSF profile absence was not observed.'
Require-Absence ($snapshot.registration.com_server.absent -eq $true) 'Explicit COM registry-view absence was not observed.'
Require-Absence (@($snapshot.registration.com_server.views).Count -eq 4) 'COM absence did not read HKCU/HKLM in both registry views.'

# A second snapshot must re-read the observer and filesystem, not reuse the
# previous absence decision. Tamper only with our private observer sidecar.
[IO.File]::WriteAllText(($cached + '.sha256'), ('0' * 64))
$unknown = Get-WixInstallerScenarioSnapshot -StateFixture $observer -OutputDir $work
Require-Absence ($null -eq (Test-WixInstallerScenarioInstalledAbsent $unknown)) 'An unverified observer became proven absence.'
[IO.File]::WriteAllText(($cached + '.sha256'), $hash)

$remnant = Join-Path $work 'product-remnant'
[void][IO.Directory]::CreateDirectory($remnant)
function Get-WixInstallerScenarioProductRoot { return $remnant }
$remnantSnapshot = Get-WixInstallerScenarioSnapshot -StateFixture $observer -OutputDir $work
Require-Absence ((Test-WixInstallerScenarioInstalledAbsent $remnantSnapshot) -eq $false) 'A real product-directory remnant was accepted.'

'wix_installer_absence_snapshot_passed=true; runtime_snapshot_chain=true; installer_runs=0; registry_writes=0; user_data_reads=0'
