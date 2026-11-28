#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$RegistrationStatusExe,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Run after ordinary guest uninstall. Only private WorkDir fixture files are
# written or removed; Session Manager/product registry keys remain read-only.
$harness = Join-Path $PSScriptRoot 'wix-installer-scenarios.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($harness, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw '[SCR000] Harness syntax errors.' }
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
    if ($null -eq $definition) { throw "[SCR000] Missing runtime helper: $name." }
    . ([scriptblock]::Create($definition.Extent.Text))
}
function Get-WixInstallerScenarioStateRetention {
    param([string]$FixtureExe, [string]$OutputDir)
    return [pscustomobject]@{ files = @(); fingerprint = ''; fixture = $null }
}
$script:Assertions = 0
$script:Failures = [Collections.Generic.List[string]]::new()
function Require-Retirement([bool]$Condition, [string]$Message) {
    $script:Assertions++
    if (-not $Condition) { $script:Failures.Add($Message) }
}
function Read-RetiredFiles($Snapshot) {
    $property = $Snapshot.PSObject.Properties['owned_retired_files']
    if ($null -eq $property) { return $null }
    return ,$property.Value
}

$actualRoot = Get-WixInstallerScenarioProductRoot
if (Test-Path -LiteralPath $actualRoot) { throw '[SCR000] Ordinary guest uninstall must remove the actual product root first.' }
$observer = (Resolve-Path -LiteralPath $RegistrationStatusExe).Path
$work = [IO.Path]::GetFullPath($WorkDir).TrimEnd('\')
if (Test-Path -LiteralPath $work) { throw '[SCR000] WorkDir must not exist.' }
[void][IO.Directory]::CreateDirectory($work)
$cached = Join-Path $work 'registration-status.exe'
[IO.File]::Copy($observer, $cached, $false)
$hash = (Get-FileHash -LiteralPath $cached -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($cached + '.sha256'), $hash, (New-Object Text.UTF8Encoding($false)))
$privateParent = Join-Path $work 'private-program-files'
[void][IO.Directory]::CreateDirectory($privateParent)
$privateRoot = Join-Path $privateParent 'LiuKai'
function Get-WixInstallerScenarioProductRoot { return $privateRoot }

$clean = Get-WixInstallerScenarioSnapshot -StateFixture $observer -OutputDir $work
$cleanFiles = Read-RetiredFiles $clean
Require-Retirement ($null -ne $cleanFiles -and @($cleanFiles).Count -eq 0) 'Runtime snapshot did not explicitly observe an empty retired-file inventory.'
Require-Retirement ((Test-WixInstallerScenarioInstalledAbsent $clean) -eq $true) 'Clean runtime snapshot did not prove complete absence.'

$stem = '.liukai-retired-' + ('a' * 32)
$tsf = Join-Path $privateParent ($stem + '-tsf.dll')
$branding = Join-Path $privateParent ($stem + '-branding.dll')
$nearMissNames = @(
    ('.liukai-retired-' + ('a' * 31) + '-tsf.dll'),
    ('.liukai-retired-' + ('g' * 32) + '-tsf.dll'),
    ($stem + '-other.dll'),
    ($stem + '-tsf.dll.extra')
)
foreach ($path in @($tsf, $branding) + @($nearMissNames | ForEach-Object { Join-Path $privateParent $_ })) {
    [IO.File]::WriteAllText($path, 'private non-executable retirement fixture')
}
$nested = Join-Path $privateParent 'foreign-child'
[void][IO.Directory]::CreateDirectory($nested)
[IO.File]::WriteAllText((Join-Path $nested ($stem + '-tsf.dll')), 'private nested near miss')
$withFiles = Get-WixInstallerScenarioSnapshot -StateFixture $observer -OutputDir $work
$observedFiles = Read-RetiredFiles $withFiles
Require-Retirement ($null -ne $observedFiles -and @($observedFiles).Count -eq 2) 'Runtime snapshot did not read exactly the two dedicated sibling retirement files.'
Require-Retirement (@($observedFiles | Where-Object { $_ -ieq $tsf }).Count -eq 1) 'Runtime snapshot missed the private TSF retirement file.'
Require-Retirement (@($observedFiles | Where-Object { $_ -ieq $branding }).Count -eq 1) 'Runtime snapshot missed the private branding retirement file.'
Require-Retirement ((Test-WixInstallerScenarioInstalledAbsent $withFiles) -eq $false) 'Actual sibling retirement files became proven absence.'

# Remove only the two exact private fixtures, then require a fresh filesystem read.
foreach ($path in @($tsf, $branding)) {
    if (-not [IO.Path]::GetFullPath($path).StartsWith($work + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw '[SCR000] Private fixture path escaped WorkDir.'
    }
    [IO.File]::Delete($path)
}
$after = Get-WixInstallerScenarioSnapshot -StateFixture $observer -OutputDir $work
$afterFiles = Read-RetiredFiles $after
Require-Retirement ($null -ne $afterFiles -and @($afterFiles).Count -eq 0) 'Fresh snapshot retained deleted fixtures or included unrelated names/subdirectories.'
Require-Retirement ((Test-WixInstallerScenarioInstalledAbsent $after) -eq $true) 'Fresh clean runtime snapshot did not recover absence.'

# Missing inventory must remain unknown even when every other observation is clean.
$missing = $after | ConvertTo-Json -Depth 16 | ConvertFrom-Json
$missing.PSObject.Properties.Remove('owned_retired_files')
Require-Retirement ($null -eq (Test-WixInstallerScenarioInstalledAbsent $missing)) 'A missing retired-file inventory became proven absence.'
$unknown = $after | ConvertTo-Json -Depth 16 | ConvertFrom-Json
$unknown | Add-Member -NotePropertyName owned_retired_files -NotePropertyValue $null -Force
Require-Retirement ($null -eq (Test-WixInstallerScenarioInstalledAbsent $unknown)) 'A null retired-file inventory became proven absence.'

# The same pure path classifier is used by runtime pending-operation projection.
$classifier = $ast.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Test-WixInstallerScenarioOwnedCleanupPath'
}, $true)
Require-Retirement ($null -ne $classifier) 'Runtime owned-cleanup path classifier is missing.'
if ($null -ne $classifier) {
    . ([scriptblock]::Create($classifier.Extent.Text))
    foreach ($case in @(
        @{ path = $privateRoot; owned = $true },
        @{ path = (Join-Path $privateRoot 'versions\fixture.dll'); owned = $true },
        @{ path = ($privateRoot + '-foreign\fixture.dll'); owned = $false },
        @{ path = $tsf; owned = $true },
        @{ path = $branding; owned = $true },
        @{ path = ('\??\' + $tsf); owned = $true },
        @{ path = ('!\??\' + $branding); owned = $true },
        @{ path = ('\\?\' + $tsf); owned = $true },
        @{ path = (Join-Path $nested ($stem + '-tsf.dll')); owned = $false },
        @{ path = ''; owned = $false }
    ) + @($nearMissNames | ForEach-Object {
        @{ path = (Join-Path $privateParent $_); owned = $false }
    })) {
        $classified = Test-WixInstallerScenarioOwnedCleanupPath -Path $case.path -ProductRoot $privateRoot
        Require-Retirement ($classified -is [bool] -and $classified -eq $case.owned) 'Owned pending-operation path classification contradicted a private fixture.'
    }
}

if ($script:Failures.Count) {
    throw ("[SCR900] assertions=$($script:Assertions); failures=$($script:Failures.Count)`n" + ($script:Failures -join "`n"))
}
"wix_installer_retirement_snapshot_passed=$($script:Assertions); runtime_filesystem_chain=true; installer_runs=0; registry_writes=0; product_file_actions=0"
