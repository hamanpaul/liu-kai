#Requires -Version 5.1

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$harnessPath = Join-Path $PSScriptRoot 'wix-installer-scenarios.ps1'
if (-not (Test-Path -LiteralPath $harnessPath -PathType Leaf)) {
    throw "[SC000] Missing installer scenario harness: $harnessPath. Implement -ContractOnly before running these tests."
}

. $harnessPath -ContractOnly

foreach ($name in @(
    'Assert-WixInstallerScenarioPreflight',
    'ConvertFrom-WixInstallerScenarioLogs',
    'Get-WixInstallerScenarioVerdict',
    'Test-WixInstallerScenarioRebootContinuation',
    'Test-WixInstallerScenarioKillEligible'
)) {
    if (-not (Get-Command -Name $name -CommandType Function -ErrorAction SilentlyContinue)) {
        throw "[SC001] Installer scenario harness is missing required contract function: $name"
    }
}

$script:ContractAssertions = 0
$script:ContractFailures = [System.Collections.Generic.List[string]]::new()

function Require-Contract {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        $script:ContractFailures.Add($Message) | Out-Null
        return
    }
    $script:ContractAssertions++
}

function Require-ContractEqual {
    param(
        $Actual,
        $Expected,
        [string]$Message
    )

    Require-Contract ($Actual -ceq $Expected) "$Message (expected '$Expected', actual '$Actual')."
}

function Require-ContractRejection {
    param(
        [scriptblock]$Action,
        [string]$Code,
        [string]$Message
    )

    $errorMessage = $null
    try {
        & $Action | Out-Null
    }
    catch {
        $errorMessage = $_.Exception.Message
    }

    Require-Contract (
        -not [string]::IsNullOrEmpty($errorMessage) -and
        $errorMessage.StartsWith("[$Code]", [StringComparison]::Ordinal)
    ) "$Message (expected [$Code], actual: $errorMessage)."
}

function Copy-ContractObject {
    param([Parameter(Mandatory = $true)][pscustomobject]$Value)

    $copy = [ordered]@{}
    foreach ($property in $Value.PSObject.Properties) {
        $copy[$property.Name] = $property.Value
    }
    return [pscustomobject]$copy
}

function Get-ContractHash {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function New-ContractLog {
    param(
        [Parameter(Mandatory = $true)][string]$Run,
        [Parameter(Mandatory = $true)][string[]]$Lines
    )

    return [pscustomobject]@{
        run  = $Run
        text = $Lines -join "`r`n"
    }
}

function New-ContractReadback {
    return [pscustomobject]@{
        expected_runtime_version = '1.0.1'
        expected_msi_version = '70.1.1'
        observed_runtime_version = '1.0.1'
        observed_payload_version = '1.0.1'
        observed_com_version = '1.0.1'
        manifest_integrity_ok = $true
        registration_status_ok = $true
        new_bundle_registered = $true
        settings_preserved = $true
        doctor_preserved = $true
        old_bundle_and_msi_registrations_absent = $true
        targeted_reentry_retries = 0
    }
}

function Convert-ContractLocalTimestampToUtc {
    param([Parameter(Mandatory = $true)][string]$Value)

    $unspecified = [DateTime]::SpecifyKind(
        [DateTime]::ParseExact(
            $Value,
            'yyyy-MM-ddTHH:mm:ss',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::None),
        [DateTimeKind]::Unspecified)
    return [TimeZoneInfo]::ConvertTimeToUtc($unspecified, [TimeZoneInfo]::Local).ToString('o')
}

function Require-SyntheticContractVerdict {
    param(
        [Parameter(Mandatory = $true)][string]$Scenario,
        [Parameter(Mandatory = $true)][pscustomobject]$Verdict,
        [Parameter(Mandatory = $true)][string]$Expected,
        [string]$Context = ''
    )

    $prefix = if ($Context) { "$Context`: " } else { '' }
    Require-ContractEqual $Verdict.scenario $Scenario ($prefix + 'Verdict scenario changed.')
    Require-ContractEqual $Verdict.verdict $Expected ($prefix + "The $Scenario verdict was dishonest.")
    Require-ContractEqual $Verdict.evidence_class 'synthetic_contract_only' ($prefix + 'Synthetic evidence was relabeled.')
    Require-ContractEqual $Verdict.receipt_status 'SYNTHETIC_CONTRACT_ONLY' ($prefix + 'Synthetic receipt status was not explicit.')
    Require-Contract (-not $Verdict.actual_scenario_pass) ($prefix + 'Synthetic evidence was labeled as an actual scenario PASS.')
}

$harnessTokens = $null
$harnessParseErrors = $null
$harnessAst = [Management.Automation.Language.Parser]::ParseFile(
    $harnessPath,
    [ref]$harnessTokens,
    [ref]$harnessParseErrors)
if ($harnessParseErrors.Count -ne 0) {
    $details = @($harnessParseErrors | Select-Object -First 4 | ForEach-Object {
        "$($_.Extent.Text): $($_.Message)"
    }) -join [Environment]::NewLine
    throw "[SC002] Harness parser errors block contract extraction.$([Environment]::NewLine)$details"
}
$trustedBaFunction = $harnessAst.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Get-WixInstallerScenarioTrustedBaHashes'
}, $false)
if ($null -eq $trustedBaFunction) {
    throw '[SC002] Harness is missing Get-WixInstallerScenarioTrustedBaHashes.'
}
. ([scriptblock]::Create($trustedBaFunction.Extent.Text))
$bundleMatchFunction = $harnessAst.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Test-WixInstallerScenarioBundleMatches'
}, $false)
if ($null -eq $bundleMatchFunction) {
    throw '[SC002] Harness is missing Test-WixInstallerScenarioBundleMatches.'
}
. ([scriptblock]::Create($bundleMatchFunction.Extent.Text))
$installedAbsentFunction = $harnessAst.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Test-WixInstallerScenarioInstalledAbsent'
}, $false)
if ($null -eq $installedAbsentFunction) {
    throw '[SC002] Harness is missing Test-WixInstallerScenarioInstalledAbsent.'
}
. ([scriptblock]::Create($installedAbsentFunction.Extent.Text))

foreach ($functionName in @(
    'Get-WixInstallerScenarioUtf8',
    'Get-WixInstallerScenarioJson',
    'Write-WixInstallerScenarioJson',
    'Read-WixInstallerScenarioSharedLog',
    'Get-WixInstallerScenarioMsiPathsFromBurn',
    'Get-WixInstallerScenarioDiscoveredLogFiles',
    'Get-WixInstallerScenarioLiveLogs'
)) {
    $functionAst = $harnessAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $functionName
    }, $false)
    if ($null -eq $functionAst) { throw "[SC002] Harness is missing $functionName." }
    . ([scriptblock]::Create($functionAst.Extent.Text))
}

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$testRoot = Join-Path $tempBase ('liu-kai-installer-contract-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot) | Out-Null

try {
    $baselineSetup = Join-Path $testRoot 'baseline-setup.exe'
    $candidateSetup = Join-Path $testRoot 'candidate-setup.exe'
    $tamperedSetup = Join-Path $testRoot 'tampered-setup.exe'
    $baPayloadPath = Join-Path $testRoot 'candidate-ba-fixture.exe'
    $sentinelPath = Join-Path $testRoot 'owned-guest-sentinel.bin'
    $manifestPath = Join-Path $testRoot 'inputs.json'
    $outputPath = Join-Path $testRoot 'scenario-output'

    [IO.File]::WriteAllText($baselineSetup, 'synthetic baseline setup')
    [IO.File]::WriteAllText($candidateSetup, 'synthetic candidate setup')
    [IO.File]::WriteAllText($tamperedSetup, 'synthetic candidate changed')
    [IO.File]::WriteAllText($baPayloadPath, 'synthetic extracted candidate BA')
    [IO.File]::WriteAllText($sentinelPath, 'owned guest fixture sentinel')

    $cleanAbsentSnapshot = [pscustomobject]@{
        installed = $false
        msi_identity = $null
        msi_product_issues = @()
        msi_products = @()
        uninstall_registry_issues = @()
        uninstall_entries = @()
        registration = [pscustomobject]@{
            absent = $true
            profile = [pscustomobject]@{ absent = $true }
            com_server = [pscustomobject]@{ absent = $true }
        }
        registration_absent = $true
        registration_status_ok = $true
        product_directory_absent = $true
        owned_pending_deletes = @()
    }
    Require-ContractEqual (
        Test-WixInstallerScenarioInstalledAbsent $cleanAbsentSnapshot
    ) $true 'Complete explicit absence did not prove installed state absent.'

    foreach ($unknownField in @(
        'msi_product_issues',
        'uninstall_registry_issues',
        'registration_absent',
        'registration',
        'product_directory_absent',
        'owned_pending_deletes'
    )) {
        $missingUnknown = Copy-ContractObject $cleanAbsentSnapshot
        $missingUnknown.PSObject.Properties.Remove($unknownField) | Out-Null
        Require-ContractEqual (
            Test-WixInstallerScenarioInstalledAbsent $missingUnknown
        ) $null "Missing $unknownField was treated as proven absence."

        $nullUnknown = Copy-ContractObject $cleanAbsentSnapshot
        $nullUnknown.PSObject.Properties[$unknownField].Value = $null
        Require-ContractEqual (
            Test-WixInstallerScenarioInstalledAbsent $nullUnknown
        ) $null "Null $unknownField was treated as proven absence."
    }
    foreach ($readErrorField in @('msi_product_issues', 'uninstall_registry_issues')) {
        $readError = Copy-ContractObject $cleanAbsentSnapshot
        $readError.PSObject.Properties[$readErrorField].Value = @('read failed')
        Require-ContractEqual (
            Test-WixInstallerScenarioInstalledAbsent $readError
        ) $null "Unreadable $readErrorField became proven absence."
    }
    foreach ($remnantCase in @(
        @{ field = 'installed'; value = $true },
        @{ field = 'msi_identity'; value = [pscustomobject]@{ product_code = 'foreign' } },
        @{ field = 'msi_products'; value = @([pscustomobject]@{ product_code = 'foreign' }) },
        @{ field = 'uninstall_entries'; value = @([pscustomobject]@{ kind = 'burn_bundle' }) },
        @{ field = 'registration_status_ok'; value = $false },
        @{ field = 'registration_absent'; value = $false },
        @{ field = 'registration'; value = [pscustomobject]@{
            absent = $false
            profile = [pscustomobject]@{ absent = $true }
            com_server = [pscustomobject]@{ absent = $true }
        } },
        @{ field = 'registration'; value = [pscustomobject]@{
            absent = $true
            profile = [pscustomobject]@{ absent = $false }
            com_server = [pscustomobject]@{ absent = $true }
        } },
        @{ field = 'registration'; value = [pscustomobject]@{
            absent = $true
            profile = [pscustomobject]@{ absent = $true }
            com_server = [pscustomobject]@{ absent = $false }
        } },
        @{ field = 'product_directory_absent'; value = $false },
        @{ field = 'owned_pending_deletes'; value = @(
            [pscustomobject]@{ path = 'C:\LiuKai\pending.bin' }
        ) }
    )) {
        $remnant = Copy-ContractObject $cleanAbsentSnapshot
        $remnant.PSObject.Properties[$remnantCase.field].Value = $remnantCase.value
        Require-ContractEqual (
            Test-WixInstallerScenarioInstalledAbsent $remnant
        ) $false "Observed $($remnantCase.field) remnant became proven absence."
    }

    $logFixtureRoot = Join-Path $testRoot "Burn Log's 繁中 Fixture"
    [IO.Directory]::CreateDirectory($logFixtureRoot) | Out-Null
    $commandLog = Join-Path $logFixtureRoot "first-burn O'Brien 嘸改.log"
    $candidateLog = Join-Path $logFixtureRoot "candidate O'Brien 嘸改_000_LiuKaiMsi.log"
    $rollbackLog = Join-Path $logFixtureRoot "candidate O'Brien 嘸改_000_LiuKaiMsi_rollback.log"
    $safeLog = Join-Path $logFixtureRoot 'safe-msi.log'
    foreach ($path in @($commandLog, $candidateLog, $rollbackLog, $safeLog)) {
        [IO.File]::WriteAllText($path, '')
    }
    $resolvedCommandLog = (Resolve-Path -LiteralPath $commandLog).Path
    $resolvedCandidateLog = (Resolve-Path -LiteralPath $candidateLog).Path
    $resolvedRollbackLog = (Resolve-Path -LiteralPath $rollbackLog).Path
    $resolvedSafeLog = (Resolve-Path -LiteralPath $safeLog).Path

    $fakeSetupPath = Join-Path $logFixtureRoot 'candidate setup.exe'
    $quotedCommandLine = @(
        "[1C40:1C34][2026-11-28T01:35:36]i009: Command Line: '$fakeSetupPath' /passive /norestart /log '$commandLog'"
    ) -join "`r`n"
    $quotedCommandError = $null
    $quotedCommandPaths = @()
    try {
        $quotedCommandPaths = @(Get-WixInstallerScenarioMsiPathsFromBurn $quotedCommandLine)
    }
    catch {
        $quotedCommandError = $_.Exception.Message
    }
    Require-Contract (
        $null -eq $quotedCommandError
    ) "Quoted EXE plus log command line crashed path extraction: $quotedCommandError"
    Require-ContractEqual $quotedCommandPaths.Count 1 (
        'Quoted EXE plus log command line did not yield exactly the real log.'
    )
    if ($quotedCommandPaths.Count -eq 1) {
        Require-ContractEqual $quotedCommandPaths[0] $resolvedCommandLog (
            'Quoted EXE plus log command line returned the wrong path.'
        )
    }

    $validMarkerText = @(
        "Setting string variable 'WixBundleLog_LiuKaiMsi' to value '$candidateLog'",
        "Variable: WixBundleRollbackLog_LiuKaiMsi = $rollbackLog"
    ) -join "`r`n"
    $validMarkerPaths = @(Get-WixInstallerScenarioMsiPathsFromBurn $validMarkerText)
    Require-ContractEqual $validMarkerPaths.Count 2 (
        'Legal apostrophe, space, and Unicode MSI log markers changed cardinality.'
    )
    Require-Contract (
        $validMarkerPaths -contains $resolvedCandidateLog -and
        $validMarkerPaths -contains $resolvedRollbackLog
    ) 'Legal Burn MSI and rollback log markers lost one or more exact paths.'

    $invalidDrivePath = 'C:\Bogus Root C:\Nested\bad-msi.log'
    $invalidControlPath = 'C:\Bogus' + [char]1 + 'control-msi.log'
    $invalidMarkerText = @(
        "Setting string variable 'WixBundleLog_LiuKaiMsi' to value '$invalidDrivePath'",
        "Variable: WixBundleRollbackLog_LiuKaiMsi = $invalidControlPath",
        "Setting string variable 'WixBundleLog_LiuKaiMsi' to value '$safeLog'"
    ) -join "`r`n"
    $invalidMarkerError = $null
    $invalidMarkerPaths = @()
    try {
        $invalidMarkerPaths = @(Get-WixInstallerScenarioMsiPathsFromBurn $invalidMarkerText)
    }
    catch {
        $invalidMarkerError = $_.Exception.Message
    }
    Require-Contract ($null -eq $invalidMarkerError) (
        "Illegal or multi-drive candidate path crashed extraction: $invalidMarkerError"
    )
    Require-ContractEqual $invalidMarkerPaths.Count 1 (
        'Illegal candidates were not ignored while a valid log remained.'
    )
    if ($invalidMarkerPaths.Count -eq 1) {
        Require-ContractEqual $invalidMarkerPaths[0] $resolvedSafeLog (
            'An illegal candidate replaced the real MSI log path.'
        )
    }

    $fixtureSandboxId = 'contract-owned-' + [Guid]::NewGuid().ToString('N')
    $manifest = [ordered]@{
        schema_version = 1
        owned_instance = [ordered]@{
            authorization_kind = 'owned-windows-sandbox-v1'
            sandbox_id = $fixtureSandboxId
            sentinel_path = $sentinelPath
            sentinel_sha256 = Get-ContractHash $sentinelPath
        }
        baseline = [ordered]@{
            setup_sha256 = Get-ContractHash $baselineSetup
            runtime_version = '1.0.0'
            burn_version = '70.1.0'
            msi_version = '70.1.0'
        }
        candidate = [ordered]@{
            setup_sha256 = Get-ContractHash $candidateSetup
            runtime_version = '1.0.1'
            burn_version = '70.1.1'
            msi_version = '70.1.1'
        }
    }
    $manifestJson = $manifest | ConvertTo-Json -Depth 5
    [IO.File]::WriteAllText(
        $manifestPath,
        $manifestJson,
        (New-Object Text.UTF8Encoding($false))
    )

    $guest = [pscustomobject]@{
        authorization_kind = 'owned-windows-sandbox-v1'
        sandbox_id = $fixtureSandboxId
        is_windows_sandbox_guest = $true
        sentinel_present = $true
        sentinel_path = $sentinelPath
        sentinel_sha256 = Get-ContractHash $sentinelPath
    }

    $preflight = Assert-WixInstallerScenarioPreflight `
        -BaselineSetup $baselineSetup `
        -CandidateSetup $candidateSetup `
        -OutputDir $outputPath `
        -Scenario Upgrade `
        -InputManifest $manifestPath `
        -GuestObservation $guest
    Require-ContractEqual $preflight.scenario 'Upgrade' 'Preflight changed the requested scenario.'
    Require-ContractEqual $preflight.candidate.runtime_version '1.0.1' 'Preflight did not retain the candidate version.'
    Require-Contract (-not (Test-Path -LiteralPath $outputPath)) 'Preflight created the runtime output directory.'

    $hostGuest = Copy-ContractObject $guest
    $hostGuest.is_windows_sandbox_guest = $false
    Require-ContractRejection {
        Assert-WixInstallerScenarioPreflight $baselineSetup $candidateSetup $outputPath Upgrade $manifestPath $hostGuest
    } 'SC010' 'A host was accepted as the installer guest.'

    $missingSentinel = Copy-ContractObject $guest
    $missingSentinel.sentinel_present = $false
    Require-ContractRejection {
        Assert-WixInstallerScenarioPreflight $baselineSetup $candidateSetup $outputPath Upgrade $manifestPath $missingSentinel
    } 'SC010' 'An absent owned-guest sentinel was accepted.'

    $wrongSandbox = Copy-ContractObject $guest
    $wrongSandbox.sandbox_id = 'another-sandbox'
    Require-ContractRejection {
        Assert-WixInstallerScenarioPreflight $baselineSetup $candidateSetup $outputPath Upgrade $manifestPath $wrongSandbox
    } 'SC010' 'A VM was accepted without the explicitly authorized Sandbox ID.'

    $wrongSentinel = Copy-ContractObject $guest
    $wrongSentinel.sentinel_sha256 = ('b' * 64)
    Require-ContractRejection {
        Assert-WixInstallerScenarioPreflight $baselineSetup $candidateSetup $outputPath Upgrade $manifestPath $wrongSentinel
    } 'SC010' 'An owned-guest sentinel hash mismatch was accepted.'

    Require-ContractRejection {
        Assert-WixInstallerScenarioPreflight $baselineSetup $tamperedSetup $outputPath Upgrade $manifestPath $guest
    } 'SC011' 'A setup hash mismatch was accepted.'

    [IO.Directory]::CreateDirectory($outputPath) | Out-Null
    Require-ContractRejection {
        Assert-WixInstallerScenarioPreflight $baselineSetup $candidateSetup $outputPath Upgrade $manifestPath $guest
    } 'SC013' 'A preexisting output directory was accepted.'

    $oldManifestPath = Join-Path $testRoot 'not-increasing.json'
    $oldManifest = Copy-ContractObject ([pscustomobject]$manifest)
    $oldManifest.candidate = [ordered]@{
        setup_sha256 = $manifest['candidate']['setup_sha256']
        runtime_version = '1.0.0'
        burn_version = '70.1.0'
        msi_version = '70.1.0'
    }
    $oldManifestJson = $oldManifest | ConvertTo-Json -Depth 5
    [IO.File]::WriteAllText(
        $oldManifestPath,
        $oldManifestJson,
        (New-Object Text.UTF8Encoding($false))
    )
    Require-ContractRejection {
        Assert-WixInstallerScenarioPreflight $baselineSetup $candidateSetup (Join-Path $testRoot 'unused-output') Upgrade $oldManifestPath $guest
    } 'SC012' 'Non-increasing runtime/MSI versions were accepted.'

    $setupHash = Get-ContractHash $candidateSetup
    $missingBa = [pscustomobject]@{
        setup_sha256 = $setupHash
    }
    Require-ContractRejection {
        Get-WixInstallerScenarioTrustedBaHashes $missingBa
    } 'SC034' 'A missing extracted BA payload fell back to the setup image.'

    $emptyBa = [pscustomobject]@{
        setup_sha256 = $setupHash
        ba_payloads = @()
    }
    Require-ContractRejection {
        Get-WixInstallerScenarioTrustedBaHashes $emptyBa
    } 'SC034' 'An empty extracted BA payload list fell back to the setup image.'

    $sameBaHash = [pscustomobject]@{
        setup_sha256 = $setupHash
        ba_payloads = @([pscustomobject]@{
            sha256 = $setupHash
            path = $candidateSetup
        })
    }
    Require-ContractRejection {
        Get-WixInstallerScenarioTrustedBaHashes $sameBaHash
    } 'SC034' 'A BA payload identical to the setup image was trusted.'

    $expectedBaHash = Get-ContractHash $baPayloadPath
    $distinctBa = [pscustomobject]@{
        setup_sha256 = $setupHash
        ba_payloads = @([pscustomobject]@{
            sha256 = $expectedBaHash
            path = $baPayloadPath
        })
    }
    $trustedBa = @(Get-WixInstallerScenarioTrustedBaHashes $distinctBa)
    Require-ContractEqual $trustedBa.Count 1 'The distinct BA payload allowlist changed cardinality.'
    Require-ContractEqual $trustedBa[0] $expectedBaHash 'The trusted BA allowlist did not use the extracted payload hash.'

    $firstBurn = New-ContractLog 'first' @(
        '[0001:0001][2026-11-27T12:00:00Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0001:0001][2026-11-27T12:00:01Z]i410: Variable: WixBundleVersion = 70.1.1',
        '[0001:0001][2026-11-27T12:00:02Z]i300: Apply begin',
        '[0001:0001][2026-11-27T12:00:10Z]i399: Apply complete, result: 0x0',
        '[0001:0001][2026-11-27T12:00:11Z]i500: Shutting down, exit code: 0x0'
    )
    $secondLockBurn = New-ContractLog 'second' @(
        '[0002:0002][2026-11-27T12:00:04Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0002:0002][2026-11-27T12:00:04Z]i410: Variable: WixBundleVersion = 70.1.1',
        '[0002:0002][2026-11-27T12:00:04Z]e000: LIUKAI_GUARD_BUSY stage=acquire hr=0x80070652',
        '[0002:0002][2026-11-27T12:00:05Z]i500: Shutting down, exit code: 0x80070652'
    )
    $firstMsi = New-ContractLog 'first' @(
        'Action start 12:00:08: InstallFinalize.',
        'Action ended 12:00:09: InstallFinalize. Return value 1.',
        'MSI (s) (AA:AA) [12:00:09]: Product: Synthetic LiuKai -- Installation completed successfully.',
        'MSI (s) (AA:AA) [12:00:09]: Property(S): ProductVersion = 70.1.1'
    )
    $historicalBaselineMsi = New-ContractLog 'first' @(
        'Action ended 11:00:09: InstallFinalize. Return value 1.',
        'MSI (s) (AA:AA) [11:00:09]: Product: Synthetic LiuKai -- Installation completed successfully.',
        'MSI (s) (AA:AA) [11:00:09]: Property(S): ProductVersion = 70.1.0'
    )
    $candidateMsiBegin = New-ContractLog 'first' @(
        'Action start 12:00:08: InstallFinalize.',
        'MSI (s) (AA:AA) [12:00:08]: Property(S): ProductVersion = 70.1.1'
    )
    $candidateMsi3010 = New-ContractLog 'first' @(
        'Action ended 12:00:09: InstallFinalize. Return value 1.',
        'MSI (s) (AA:AA) [12:00:09]: Product Name: Synthetic LiuKai. Installation success or error status: 3010.',
        'MSI (s) (AA:AA) [12:00:09]: Property(S): ProductVersion = 70.1.1'
    )
    $candidateRemoval = New-ContractLog 'first' @(
        'Action ended 12:00:09: InstallFinalize. Return value 1.',
        'MSI (s) (AA:AA) [12:00:09]: Product: Synthetic LiuKai -- Removal completed successfully.',
        'MSI (s) (AA:AA) [12:00:09]: Property(S): ProductVersion = 70.1.1'
    )
    $candidateMsiZhTw = New-ContractLog 'first' @(
        'MSI (s) (6C:0C) [12:00:09]: Doing action: InstallFinalize',
        '動作開始 12:00:09: InstallFinalize。',
        '動作結束 12:00:12: InstallFinalize。傳回值 1。',
        'MSI (s) (6C:0C) [12:00:12]: 產品: 嘸．改 -- 成功完成安裝。',
        'MSI (s) (6C:0C) [12:00:12]: Windows Installer 已安裝該產品。產品名稱: 嘸．改。產品版本: 70.1.1。產品語言: 0。製造商: LiuKai。安裝成功或錯誤狀態: 0。',
        'MSI (s) (6C:0C) [12:00:12]: Property(S): ProductVersion = 70.1.1',
        'MSI (s) (6C:0C) [12:00:12]: MainEngineThread is returning 0'
    )
    $candidateMsiZhTw3010 = New-ContractLog 'first' @(
        'MSI (s) (6C:0C) [12:00:09]: Doing action: InstallFinalize',
        '動作開始 12:00:09: InstallFinalize。',
        '動作結束 12:00:12: InstallFinalize。傳回值 1。',
        'MSI (s) (6C:0C) [12:00:12]: 產品: 嘸．改 -- 成功完成安裝。',
        'MSI (s) (6C:0C) [12:00:12]: Windows Installer 已安裝該產品。產品名稱: 嘸．改。產品版本: 70.1.1。產品語言: 0。製造商: LiuKai。安裝成功或錯誤狀態: 3010。',
        'MSI (s) (6C:0C) [12:00:12]: Property(S): ProductVersion = 70.1.1',
        'MSI (s) (6C:0C) [12:00:12]: MainEngineThread is returning 3010'
    )
    $candidateMsiZhTwFailure = New-ContractLog 'first' @(
        'MSI (s) (6C:0C) [12:00:09]: Doing action: InstallFinalize',
        '動作開始 12:00:09: InstallFinalize。',
        '動作結束 12:00:12: InstallFinalize。傳回值 3。',
        'MSI (s) (6C:0C) [12:00:12]: Windows Installer 已安裝該產品。產品名稱: 嘸．改。產品版本: 70.1.1。產品語言: 0。製造商: LiuKai。安裝成功或錯誤狀態: 1603。',
        'MSI (s) (6C:0C) [12:00:12]: Property(S): ProductVersion = 70.1.1',
        'MSI (s) (6C:0C) [12:00:12]: MainEngineThread is returning 0'
    )
    $candidateMsiZhTwRemoval = New-ContractLog 'first' @(
        '動作開始 12:00:09: InstallFinalize。',
        '動作結束 12:00:12: InstallFinalize。傳回值 1。',
        'MSI (s) (6C:0C) [12:00:12]: 產品: 嘸．改 -- 移除完成。',
        'MSI (s) (6C:0C) [12:00:12]: Property(S): ProductVersion = 70.1.1'
    )
    $historicalMsiZhTw = New-ContractLog 'first' @(
        '動作結束 11:00:12: InstallFinalize。傳回值 1。',
        'MSI (s) (6C:0C) [11:00:12]: 產品: 嘸．改 -- 成功完成安裝。',
        'MSI (s) (6C:0C) [11:00:12]: Windows Installer 已安裝該產品。產品名稱: 嘸．改。產品版本: 70.1.0。產品語言: 0。製造商: LiuKai。安裝成功或錯誤狀態: 0。',
        'MSI (s) (6C:0C) [11:00:12]: Property(S): ProductVersion = 70.1.0'
    )

    $realHeaderEvidence = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade `
        -BurnLogs @($firstBurn) `
        -MsiLogs @($firstMsi) `
        -Readback (New-ContractReadback) `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $realHeaderEvidence 'burn_version'
    ) '70.1.1' 'The product version was not read from WixBundleVersion.'
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $realHeaderEvidence 'burn_engine_version'
    ) '6.0.2' 'The Burn engine version was not separated from the product version.'

    $noArchitectureBurn = New-ContractLog 'first' @(
        '[0001:0001][2026-11-27T12:00:00Z]i001: Burn v6.0.2, Windows v10.0 x64',
        '[0001:0001][2026-11-27T12:00:01Z]i410: Variable: WixBundleVersion = 70.1.1',
        '[0001:0001][2026-11-27T12:00:02Z]i300: Apply begin',
        '[0001:0001][2026-11-27T12:00:10Z]i399: Apply complete, result: 0x0',
        '[0001:0001][2026-11-27T12:00:11Z]i500: Shutting down, exit code: 0x0'
    )
    $noArchitectureEvidence = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade -BurnLogs @($noArchitectureBurn) -MsiLogs @($firstMsi) `
        -Readback (New-ContractReadback) -EvidenceClass synthetic_contract_only
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $noArchitectureEvidence 'burn_engine_version'
    ) '6.0.2' 'The common architecture-free Burn header was not recognized.'
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $noArchitectureEvidence 'burn_version'
    ) '70.1.1' 'The architecture-free Burn header replaced the product version.'

    $missingProductBurn = New-ContractLog 'first' @(
        '[0001:0001][2026-11-27T12:00:00Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0001:0001][2026-11-27T12:00:02Z]i300: Apply begin',
        '[0001:0001][2026-11-27T12:00:10Z]i399: Apply complete, result: 0x0',
        '[0001:0001][2026-11-27T12:00:11Z]i500: Shutting down, exit code: 0x0'
    )
    $missingProduct = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade -BurnLogs @($missingProductBurn) -MsiLogs @($firstMsi) `
        -Readback (New-ContractReadback) -EvidenceClass synthetic_contract_only
    Require-Contract (
        $null -eq (Get-WixInstallerScenarioProperty $missingProduct 'burn_version')
    ) 'The missing WixBundleVersion was backfilled from the engine or expected MSI version.'
    Require-SyntheticContractVerdict Upgrade (
        Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $missingProduct
    ) NOT_COVERED -Context 'Missing WixBundleVersion'

    $wrongProductBurn = New-ContractLog 'first' @(
        '[0001:0001][2026-11-27T12:00:00Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0001:0001][2026-11-27T12:00:01Z]i410: Variable: WixBundleVersion = 70.1.0',
        '[0001:0001][2026-11-27T12:00:02Z]i300: Apply begin',
        '[0001:0001][2026-11-27T12:00:10Z]i399: Apply complete, result: 0x0',
        '[0001:0001][2026-11-27T12:00:11Z]i500: Shutting down, exit code: 0x0'
    )
    $wrongProduct = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade -BurnLogs @($wrongProductBurn) -MsiLogs @($firstMsi) `
        -Readback (New-ContractReadback) -EvidenceClass synthetic_contract_only
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $wrongProduct 'burn_version'
    ) '70.1.0' 'The observed WixBundleVersion was replaced with an expected value.'
    Require-SyntheticContractVerdict Upgrade (
        Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $wrongProduct
    ) FAIL -Context 'Wrong WixBundleVersion'

    $liveBurnPath = Join-Path $testRoot 'synthetic-burn-x64.log'
    [IO.File]::WriteAllText(
        $liveBurnPath,
        $firstBurn.text,
        (New-Object Text.UTF8Encoding($false)))
    $liveBurnLogs = @(Get-WixInstallerScenarioLiveLogs `
        -StartedUtc ([DateTime]::UtcNow.AddDays(-1)) `
        -ExplicitBurnLog $liveBurnPath)
    Require-ContractEqual $liveBurnLogs.Count 1 (
        'A real x64 Burn header in an explicit local fixture was not recognized.'
    )
    if ($liveBurnLogs.Count -eq 1) {
        Require-ContractEqual $liveBurnLogs[0].kind 'burn' (
            'The real x64 Burn header was classified as a non-Burn log.'
        )
    }

    $stalePlusBegin = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade `
        -BurnLogs @($firstBurn) `
        -MsiLogs @($historicalBaselineMsi, $candidateMsiBegin) `
        -Readback (New-ContractReadback) `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual $stalePlusBegin.first_msi_commits 0 (
        'A historical successful MSI transaction completed the candidate install.'
    )
    Require-ContractEqual $stalePlusBegin.msi_product_version '70.1.1' (
        'A candidate-begin log was not selected as the candidate MSI version.'
    )

    $candidate3010Evidence = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade `
        -BurnLogs @($firstBurn) `
        -MsiLogs @($candidateMsi3010) `
        -Readback (New-ContractReadback) `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual $candidate3010Evidence.first_msi_commits 1 (
        'A successful candidate InstallFinalize with MSI status 3010 was not a commit.'
    )

    $candidateAndStale = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade `
        -BurnLogs @($firstBurn) `
        -MsiLogs @($candidateMsi3010, $historicalBaselineMsi) `
        -Readback (New-ContractReadback) `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual $candidateAndStale.first_msi_commits 1 (
        'A historical MSI log changed the candidate commit count.'
    )
    Require-ContractEqual $candidateAndStale.msi_product_version '70.1.1' (
        'A trailing historical MSI log replaced the candidate ProductVersion.'
    )

    $removalEvidence = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade `
        -BurnLogs @($firstBurn) `
        -MsiLogs @($candidateRemoval) `
        -Readback (New-ContractReadback) `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual $removalEvidence.first_msi_commits 0 (
        'MSI removal success was mislabeled as an install commit.'
    )

    $zhTwSuccess = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade `
        -BurnLogs @($firstBurn) `
        -MsiLogs @($candidateMsiZhTw) `
        -Readback (New-ContractReadback) `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual $zhTwSuccess.first_msi_transactions 1 (
        'A zh-TW candidate success lost its MSI transaction.'
    )
    Require-ContractEqual $zhTwSuccess.first_msi_commits 1 (
        'A zh-TW candidate InstallFinalize success was not committed.'
    )
    Require-ContractEqual $zhTwSuccess.msi_product_version '70.1.1' (
        'A zh-TW candidate summary did not preserve ProductVersion 70.1.1.'
    )
    Require-SyntheticContractVerdict Upgrade (
        Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $zhTwSuccess
    ) PASS -Context 'zh-TW MSI install success'

    $zhTw3010 = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade `
        -BurnLogs @($firstBurn) `
        -MsiLogs @($candidateMsiZhTw3010) `
        -Readback (New-ContractReadback) `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual $zhTw3010.first_msi_transactions 1 (
        'A zh-TW 3010 candidate success lost its MSI transaction.'
    )
    Require-ContractEqual $zhTw3010.first_msi_commits 1 (
        'A zh-TW 3010 InstallFinalize success was not committed.'
    )
    Require-ContractEqual $zhTw3010.restart_required $true (
        'A zh-TW MSI status 3010 did not require restart.'
    )
    Require-ContractEqual $zhTw3010.msi_product_version '70.1.1' (
        'A zh-TW 3010 summary changed the observed candidate version.'
    )
    Require-SyntheticContractVerdict Upgrade (
        Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $zhTw3010
    ) PASS -Context 'zh-TW MSI restart-required commit'

    $zhTwFailure = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade `
        -BurnLogs @($firstBurn) `
        -MsiLogs @($candidateMsiZhTwFailure) `
        -Readback (New-ContractReadback) `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual $zhTwFailure.first_msi_transactions 1 (
        'A zh-TW candidate failure lost its MSI transaction observation.'
    )
    Require-ContractEqual $zhTwFailure.first_msi_commits 0 (
        'A zh-TW InstallFinalize return value 3 plus MSI 1603 was committed.'
    )
    Require-ContractEqual $zhTwFailure.restart_required $false (
        'A zh-TW MSI 1603 was mislabeled as restart-required.'
    )
    Require-SyntheticContractVerdict Upgrade (
        Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $zhTwFailure
    ) NOT_COVERED -Context 'zh-TW MSI failure has no candidate commit'

    $zhTwRemovalAndHistorical = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade `
        -BurnLogs @($firstBurn) `
        -MsiLogs @($candidateMsiZhTwRemoval, $historicalMsiZhTw) `
        -Readback (New-ContractReadback) `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual $zhTwRemovalAndHistorical.first_msi_transactions 1 (
        'A historical zh-TW MSI log completed or added a candidate transaction.'
    )
    Require-ContractEqual $zhTwRemovalAndHistorical.first_msi_commits 0 (
        'A same-version zh-TW removal or historical success became a candidate install commit.'
    )
    Require-ContractEqual $zhTwRemovalAndHistorical.msi_product_version '70.1.1' (
        'A historical zh-TW 70.1.0 success replaced the candidate removal version.'
    )
    Require-SyntheticContractVerdict Upgrade (
        Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $zhTwRemovalAndHistorical
    ) NOT_COVERED -Context 'zh-TW removal and historical MSI are not a commit'

    $sameLogMajorUpgradeMsi = New-ContractLog 'first' @(
        'MSI (s) (AA:AA) [12:00:07]: Property(S): ProductVersion = 70.1.0',
        'MSI (s) (AA:AA) [12:00:08]: Product: Synthetic LiuKai -- Removal completed successfully.',
        'Action ended 12:00:09: InstallFinalize. Return value 1.',
        'MSI (s) (AA:AA) [12:00:10]: Property(S): ProductVersion = 70.1.1',
        'MSI (s) (AA:AA) [12:00:11]: Product: Synthetic LiuKai -- Installation completed successfully.',
        'MSI (s) (AA:AA) [12:00:11]: Windows Installer installed the product. Product Name: Synthetic LiuKai. Product Version: 70.1.1. Product Language: 1033. Manufacturer: LiuKai. Installation success or error status: 3010.',
        'Action ended 12:00:12: InstallFinalize. Return value 1.'
    )
    $sameLogMajorUpgrade = ConvertFrom-WixInstallerScenarioLogs -Scenario Upgrade `
        -BurnLogs @($firstBurn) -MsiLogs @($sameLogMajorUpgradeMsi) `
        -Readback (New-ContractReadback) -EvidenceClass synthetic_contract_only
    Require-ContractEqual $sameLogMajorUpgrade.first_msi_commits 1 'The same-log MajorUpgrade candidate install commit was lost.'
    Require-ContractEqual $sameLogMajorUpgrade.msi_product_version '70.1.1' 'The baseline ProductVersion replaced the same-log candidate version.'
    Require-ContractEqual $sameLogMajorUpgrade.restart_required $true 'The same-log MajorUpgrade status 3010 did not require restart.'

    $upgrade = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Upgrade `
        -BurnLogs @($firstBurn) `
        -MsiLogs @($firstMsi) `
        -Readback (New-ContractReadback) `
        -EvidenceClass synthetic_contract_only
    Require-Contract ($upgrade.first_msi_commits -eq 1) 'The raw MSI finalize evidence was not parsed.'
    Require-SyntheticContractVerdict Upgrade (Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $upgrade) PASS

    foreach ($versionField in @(
        'observed_runtime_version',
        'observed_payload_version',
        'observed_com_version'
    )) {
        $wrongVersionReadback = New-ContractReadback
        $wrongVersionReadback.PSObject.Properties[$versionField].Value = '1.0.0'
        $wrongVersion = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Upgrade `
            -BurnLogs @($firstBurn) `
            -MsiLogs @($firstMsi) `
            -Readback $wrongVersionReadback `
            -EvidenceClass synthetic_contract_only
        Require-SyntheticContractVerdict Upgrade (
            Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $wrongVersion
        ) FAIL -Context ("Upgrade $versionField explicitly contradicted")

        $missingVersionReadback = New-ContractReadback
        $missingVersionReadback.PSObject.Properties.Remove($versionField) | Out-Null
        $missingVersion = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Upgrade `
            -BurnLogs @($firstBurn) `
            -MsiLogs @($firstMsi) `
            -Readback $missingVersionReadback `
            -EvidenceClass synthetic_contract_only
        Require-SyntheticContractVerdict Upgrade (
            Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $missingVersion
        ) NOT_COVERED -Context ("Upgrade $versionField is absent")
    }

    foreach ($requiredObservation in @(
        'manifest_integrity_ok',
        'registration_status_ok',
        'new_bundle_registered',
        'settings_preserved',
        'doctor_preserved'
    )) {
        $contradictedObservation = New-ContractReadback
        $contradictedObservation.PSObject.Properties[$requiredObservation].Value = $false
        $contradictedUpgrade = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Upgrade `
            -BurnLogs @($firstBurn) `
            -MsiLogs @($firstMsi) `
            -Readback $contradictedObservation `
            -EvidenceClass synthetic_contract_only
        Require-SyntheticContractVerdict Upgrade (
            Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $contradictedUpgrade
        ) FAIL -Context ("Upgrade $requiredObservation=false")

        $missingObservation = New-ContractReadback
        $missingObservation.PSObject.Properties.Remove($requiredObservation) | Out-Null
        $incompleteUpgrade = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Upgrade `
            -BurnLogs @($firstBurn) `
            -MsiLogs @($firstMsi) `
            -Readback $missingObservation `
            -EvidenceClass synthetic_contract_only
        Require-SyntheticContractVerdict Upgrade (
            Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $incompleteUpgrade
        ) NOT_COVERED -Context ("Upgrade $requiredObservation is absent")
    }

    $reentryReadback = New-ContractReadback
    $reentryReadback.targeted_reentry_retries = 1
    $reentry = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry `
        -BurnLogs @($firstBurn, $secondLockBurn) `
        -MsiLogs @($firstMsi) `
        -Readback $reentryReadback `
        -EvidenceClass synthetic_contract_only
    Require-Contract ($reentry.second_msi_transactions -eq 0) 'The zero-second-MSI fact was lost.'
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $reentry 'second_guard_busy_before_apply'
    ) $true 'The exact early guard rejection was not recorded.'
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $reentry 'second_apply_begin_count'
    ) 0 'The second invocation was recorded as having an Apply begin.'
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $reentry 'second_session_begin_count'
    ) 0 'The second invocation was recorded as having a Session begin.'
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $reentry 'second_log_complete'
    ) $true 'The complete second shutdown was not recorded.'
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $reentry 'second_terminal_exit_code'
    ) '0x80070652' 'The i500 terminal exit code was not retained.'
    Require-SyntheticContractVerdict Reentry (Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $reentry) PASS

    $secondEarlyExitBurn = New-ContractLog 'second' @(
        '[0002:0002][2026-11-27T12:00:04Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0002:0002][2026-11-27T12:00:04Z]i000: LIUKAI_GUARD_BUSY stage=acquire hr=0x80070652',
        '[0002:0002][2026-11-27T12:00:04Z]e000: Error 0x80070652: Failed to create BAFunctions.',
        '[0002:0002][2026-11-27T12:00:04Z]e000: Error 0x80070652: Failed to start bootstrapper application.',
        '[0002:0002][2026-11-27T12:00:04Z]i410: Variable: WixBundleVersion = 70.1.1',
        '[0002:0002][2026-11-27T12:00:04Z]e000: Error 0x80070652: Failed to run normal mode.',
        '[0002:0002][2026-11-27T12:00:04Z]i007: Exit code: 0x80070652, restarting: No'
    )
    $earlyExitReentry = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry -BurnLogs @($firstBurn, $secondEarlyExitBurn) `
        -MsiLogs @($firstMsi) -Readback $reentryReadback `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $earlyExitReentry 'second_log_complete'
    ) $true 'The real i007 early-create-failure terminal was not recognized.'
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $earlyExitReentry 'second_terminal_exit_code'
    ) '0x80070652' 'The i007 terminal exit code was not retained.'
    Require-ContractEqual $earlyExitReentry.second_apply_begin_count 0 (
        'The i007 early rejection was reported after Apply begin.'
    )
    Require-ContractEqual $earlyExitReentry.second_session_begin_count 0 (
        'The i007 early rejection was reported after Session begin.'
    )
    Require-SyntheticContractVerdict Reentry (
        Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $earlyExitReentry
    ) PASS -Context 'Real early BAFunctions failure shape'

    $secondGuardAfterZero = New-ContractLog 'second' @(
        '[0002:0002][2026-11-27T12:00:04Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0002:0002][2026-11-27T12:00:04Z]i000: Pre-guard probe result: 0x00000000',
        '[0002:0002][2026-11-27T12:00:04Z]i410: Variable: WixBundleVersion = 70.1.1',
        '[0002:0002][2026-11-27T12:00:04Z]e000: LIUKAI_GUARD_BUSY stage=acquire hr=0x80070652',
        '[0002:0002][2026-11-27T12:00:05Z]i500: Shutting down, exit code: 0x80070652'
    )
    $guardAfterZero = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry -BurnLogs @($firstBurn, $secondGuardAfterZero) `
        -MsiLogs @($firstMsi) -Readback $reentryReadback `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual $guardAfterZero.second_hresult '0x80070652' (
        'A pre-guard 0x00000000 replaced the precise busy HRESULT.'
    )
    Require-SyntheticContractVerdict Reentry (
        Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $guardAfterZero
    ) PASS -Context 'Pre-guard 0x00000000'

    foreach ($contradiction in @(
        [pscustomobject]@{
            Name = 'Apply begin'
            Line = '[0002:0002][2026-11-27T12:00:05Z]i300: Apply begin'
            CountField = 'second_apply_begin_count'
        },
        [pscustomobject]@{
            Name = 'Session begin'
            Line = '[0002:0002][2026-11-27T12:00:05Z]i370: Session begin, registration key: SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{B}, scope: PerMachine'
            CountField = 'second_session_begin_count'
        }
    )) {
        $contradictingSecond = New-ContractLog 'second' @(
            '[0002:0002][2026-11-27T12:00:04Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
            '[0002:0002][2026-11-27T12:00:04Z]i410: Variable: WixBundleVersion = 70.1.1',
            '[0002:0002][2026-11-27T12:00:04Z]e000: LIUKAI_GUARD_BUSY stage=acquire hr=0x80070652',
            $contradiction.Line,
            '[0002:0002][2026-11-27T12:00:06Z]i500: Shutting down, exit code: 0x80070652'
        )
        $lateProgress = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Reentry -BurnLogs @($firstBurn, $contradictingSecond) `
            -MsiLogs @($firstMsi) -Readback $reentryReadback `
            -EvidenceClass synthetic_contract_only
        Require-ContractEqual (
            Get-WixInstallerScenarioProperty $lateProgress $contradiction.CountField
        ) 1 "$($contradiction.Name) was omitted from the observed second-run count."
        Require-SyntheticContractVerdict Reentry (
            Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $lateProgress
        ) FAIL -Context "Guard marker followed by $($contradiction.Name)"
    }

    foreach ($genericBusyLine in @(
        '[0002:0002][2026-11-27T12:00:04Z]e000: another installer is running: 1618',
        '[0002:0002][2026-11-27T12:00:04Z]e000: ERROR_INSTALL_ALREADY_RUNNING',
        '[0002:0002][2026-11-27T12:00:04Z]e000: result 0x80070652'
    )) {
        $genericBusySecond = New-ContractLog 'second' @(
            '[0002:0002][2026-11-27T12:00:04Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
            '[0002:0002][2026-11-27T12:00:04Z]i410: Variable: WixBundleVersion = 70.1.1',
            $genericBusyLine,
            '[0002:0002][2026-11-27T12:00:05Z]i500: Shutting down, exit code: 0x80070652'
        )
        $genericBusy = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Reentry -BurnLogs @($firstBurn, $genericBusySecond) `
            -MsiLogs @($firstMsi) -Readback $reentryReadback `
            -EvidenceClass synthetic_contract_only
        Require-ContractEqual (
            Get-WixInstallerScenarioProperty $genericBusy 'second_guard_busy_before_apply'
        ) $false 'Generic busy text was relabeled as the LiuKai product guard.'
        Require-SyntheticContractVerdict Reentry (
            Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $genericBusy
        ) NOT_COVERED -Context "No product guard marker: $genericBusyLine"
    }

    $missingSecondLog = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry -BurnLogs @($firstBurn) -MsiLogs @($firstMsi) `
        -Readback $reentryReadback -EvidenceClass synthetic_contract_only
    Require-Contract (
        (-not (Test-WixInstallerScenarioProperty $missingSecondLog 'second_apply_begin_count') -or
            $null -eq (Get-WixInstallerScenarioProperty $missingSecondLog 'second_apply_begin_count')) -and
        (-not (Test-WixInstallerScenarioProperty $missingSecondLog 'second_session_begin_count') -or
            $null -eq (Get-WixInstallerScenarioProperty $missingSecondLog 'second_session_begin_count')) -and
        (-not (Test-WixInstallerScenarioProperty $missingSecondLog 'second_log_complete') -or
            $null -eq (Get-WixInstallerScenarioProperty $missingSecondLog 'second_log_complete'))
    ) 'An absent second log was converted into proven zero Apply/Session begins or completion.'
    Require-SyntheticContractVerdict Reentry (
        Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $missingSecondLog
    ) NOT_COVERED -Context 'Missing second log'

    $unfinishedSecondBurn = New-ContractLog 'second' @(
        '[0002:0002][2026-11-27T12:00:04Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0002:0002][2026-11-27T12:00:04Z]i410: Variable: WixBundleVersion = 70.1.1',
        '[0002:0002][2026-11-27T12:00:04Z]e000: LIUKAI_GUARD_BUSY stage=acquire hr=0x80070652'
    )
    $unfinishedSecond = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry -BurnLogs @($firstBurn, $unfinishedSecondBurn) `
        -MsiLogs @($firstMsi) -Readback $reentryReadback `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual (
        Get-WixInstallerScenarioProperty $unfinishedSecond 'second_log_complete'
    ) $false 'A second Burn log without its terminal shutdown was marked complete.'
    Require-Contract (
        $null -eq (Get-WixInstallerScenarioProperty $unfinishedSecond 'second_terminal_exit_code')
    ) 'A non-terminal second log acquired an exit code.'
    Require-SyntheticContractVerdict Reentry (
        Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $unfinishedSecond
    ) NOT_COVERED -Context 'Missing second terminal log'

    foreach ($terminalSuccessCase in @(
        [pscustomobject]@{
            Name = 'i500'
            Terminal = '[0002:0002][2026-11-27T12:00:05Z]i500: Shutting down, exit code: 0x0'
        },
        [pscustomobject]@{
            Name = 'i007'
            Terminal = '[0002:0002][2026-11-27T12:00:05Z]i007: Exit code: 0x0, restarting: No'
        }
    )) {
        $busyWithSuccessfulTerminalLog = New-ContractLog 'second' @(
            '[0002:0002][2026-11-27T12:00:04Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
            '[0002:0002][2026-11-27T12:00:04Z]i410: Variable: WixBundleVersion = 70.1.1',
            '[0002:0002][2026-11-27T12:00:04Z]e000: LIUKAI_GUARD_BUSY stage=acquire hr=0x80070652',
            $terminalSuccessCase.Terminal
        )
        $busyWithSuccessfulTerminal = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Reentry -BurnLogs @($firstBurn, $busyWithSuccessfulTerminalLog) `
            -MsiLogs @($firstMsi) -Readback $reentryReadback `
            -EvidenceClass synthetic_contract_only
        Require-ContractEqual $busyWithSuccessfulTerminal.second_log_complete $true (
            "The complete $($terminalSuccessCase.Name) terminal was not recognized."
        )
        Require-ContractEqual (
            Get-WixInstallerScenarioProperty $busyWithSuccessfulTerminal 'second_terminal_exit_code'
        ) '0x0' (
            "The $($terminalSuccessCase.Name) terminal success code was not retained."
        )
        Require-SyntheticContractVerdict Reentry (
            Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $busyWithSuccessfulTerminal
        ) FAIL -Context "Guard marker contradicted by $($terminalSuccessCase.Name) exit 0x0"
    }

    $runtimeWithoutSecondProcess = Copy-ContractObject $reentry
    $runtimeWithoutSecondProcess.evidence_class = 'runtime_observed'
    $runtimeWithoutSecondProcessVerdict = Get-WixInstallerScenarioVerdict `
        -Scenario Reentry -Evidence $runtimeWithoutSecondProcess
    Require-ContractEqual $runtimeWithoutSecondProcessVerdict.verdict 'NOT_COVERED' (
        'Runtime evidence passed without a second_process observation.'
    )

    $runtimeMissingExited = Copy-ContractObject $reentry
    $runtimeMissingExited.evidence_class = 'runtime_observed'
    $runtimeMissingExited | Add-Member -NotePropertyName second_process -NotePropertyValue (
        [pscustomobject]@{ pid = 2222 }
    )
    $runtimeMissingExitedVerdict = Get-WixInstallerScenarioVerdict `
        -Scenario Reentry -Evidence $runtimeMissingExited
    Require-ContractEqual $runtimeMissingExitedVerdict.verdict 'NOT_COVERED' (
        'Runtime evidence passed without second_process.exited.'
    )

    $runtimeStillRunning = Copy-ContractObject $reentry
    $runtimeStillRunning.evidence_class = 'runtime_observed'
    $runtimeStillRunning | Add-Member -NotePropertyName second_process -NotePropertyValue (
        [pscustomobject]@{ pid = 2222; exited = $false }
    )
    $runtimeStillRunningVerdict = Get-WixInstallerScenarioVerdict `
        -Scenario Reentry -Evidence $runtimeStillRunning
    Require-ContractEqual $runtimeStillRunningVerdict.verdict 'NOT_COVERED' (
        'Runtime evidence passed while the second process was still running.'
    )

    $runtimeMissingExitCode = Copy-ContractObject $reentry
    $runtimeMissingExitCode.evidence_class = 'runtime_observed'
    $runtimeMissingExitCode | Add-Member -NotePropertyName second_process -NotePropertyValue (
        [pscustomobject]@{ pid = 2222; exited = $true }
    )
    $runtimeMissingExitCodeVerdict = Get-WixInstallerScenarioVerdict `
        -Scenario Reentry -Evidence $runtimeMissingExitCode
    Require-ContractEqual $runtimeMissingExitCodeVerdict.verdict 'NOT_COVERED' (
        'Runtime evidence passed without second_process.exit_code.'
    )

    $runtimeZeroExitCode = Copy-ContractObject $reentry
    $runtimeZeroExitCode.evidence_class = 'runtime_observed'
    $runtimeZeroExitCode | Add-Member -NotePropertyName second_process -NotePropertyValue (
        [pscustomobject]@{ pid = 2222; exited = $true; exit_code = 0 }
    )
    $runtimeZeroExitCodeVerdict = Get-WixInstallerScenarioVerdict `
        -Scenario Reentry -Evidence $runtimeZeroExitCode
    Require-ContractEqual $runtimeZeroExitCodeVerdict.verdict 'FAIL' (
        'Runtime evidence accepted process exit code 0 after the guard busy marker.'
    )

    $runtimeCompleted = Copy-ContractObject $reentry
    $runtimeCompleted.evidence_class = 'runtime_observed'
    $runtimeCompleted | Add-Member -NotePropertyName second_process -NotePropertyValue (
        [pscustomobject]@{ pid = 2222; exited = $true; exit_code = 2147944018 }
    )
    $runtimeCompletedVerdict = Get-WixInstallerScenarioVerdict `
        -Scenario Reentry -Evidence $runtimeCompleted
    Require-ContractEqual $runtimeCompletedVerdict.verdict 'PASS' (
        'Complete runtime process and log evidence did not retain the positive reentry verdict.'
    )

    $runtimeCompletedSigned = Copy-ContractObject $reentry
    $runtimeCompletedSigned.evidence_class = 'runtime_observed'
    $runtimeCompletedSigned | Add-Member -NotePropertyName second_process -NotePropertyValue (
        [pscustomobject]@{ pid = 2222; exited = $true; exit_code = -2147023278 }
    )
    $runtimeCompletedSignedVerdict = Get-WixInstallerScenarioVerdict `
        -Scenario Reentry -Evidence $runtimeCompletedSigned
    Require-ContractEqual $runtimeCompletedSignedVerdict.verdict 'PASS' (
        'Signed process exit code -2147023278 was not accepted as HRESULT 0x80070652.'
    )

    $firstReentry3010Burn = New-ContractLog 'first' @(
        '[0001:0001][2026-11-27T12:00:00Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0001:0001][2026-11-27T12:00:01Z]i410: Variable: WixBundleVersion = 70.1.1',
        '[0001:0001][2026-11-27T12:00:02Z]i300: Apply begin',
        '[0001:0001][2026-11-27T12:00:10Z]i399: Apply complete, result: 3010',
        '[0001:0001][2026-11-27T12:00:11Z]i500: Shutting down, exit code: 3010'
    )
    $reentry3010 = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry `
        -BurnLogs @($firstReentry3010Burn, $secondLockBurn) `
        -MsiLogs @($candidateMsi3010) `
        -Readback $reentryReadback `
        -EvidenceClass synthetic_contract_only
    Require-Contract ($reentry3010.first_msi_commits -eq 1) 'The 3010 MSI transaction did not commit.'
    Require-Contract ($reentry3010.restart_required -eq $true) 'The 3010 MSI transaction did not require restart.'
    Require-Contract ($reentry3010.second_msi_transactions -eq 0) 'The 3010 reentry gained a second MSI transaction.'
    Require-SyntheticContractVerdict Reentry (
        Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $reentry3010
    ) PASS -Context 'Complete-state Reentry 3010'

    $rebootEvidence = Copy-ContractObject $reentry3010
    $rebootEvidence.evidence_class = 'runtime_observed'
    $rebootEvidence | Add-Member -NotePropertyName unattributed_msi_transactions -NotePropertyValue 0
    $rebootEvidence | Add-Member -NotePropertyName first_process -NotePropertyValue (
        @{ exited = $true; exit_code = 3010; pid = 1111 }
    )
    $rebootEvidence | Add-Member -NotePropertyName second_process -NotePropertyValue (
        @{ exited = $true; exit_code = -2147023278; pid = 2222 }
    )
    $preRebootVerdict = Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $rebootEvidence
    Require-ContractEqual $preRebootVerdict.verdict 'PASS' (
        'The complete runtime 3010 reentry evidence did not form the reboot prerequisite.'
    )
    $preRebootVerdict.verdict = 'NOT_COVERED'
    $preRebootVerdict.receipt_status = 'RUNTIME_OBSERVED_NOT_COVERED'
    $preRebootVerdict.actual_scenario_pass = $false
    $preRebootVerdict.reason = 'Root-owned guest reboot required after 3010.'
    $rebootReceipt = [pscustomobject]@{
        format = 'liukai-installer-scenario-receipt-v1'
        scenario = 'Reentry'
        verdict = $preRebootVerdict
        evidence = $rebootEvidence
        raw_log_copies = @()
        root_guest_reboot_required = $true
        unexpected_process_live = $false
    }
    $postRebootReadback = Copy-ContractObject $reentryReadback
    $originalReceiptJson = $rebootReceipt | ConvertTo-Json -Depth 20

    $continued = Test-WixInstallerScenarioRebootContinuation `
        -OriginalReceipt $rebootReceipt `
        -PostRebootReadback $postRebootReadback `
        -GuestBootUtc '2026-11-27T12:05:00Z' `
        -OwnedPendingDeletes @()
    Require-ContractEqual $continued.scenario 'Reentry' (
        'A reboot continuation changed the original scenario.'
    )
    Require-ContractEqual $continued.verdict 'PASS' (
        'A complete post-reboot Reentry continuation did not converge.'
    )
    Require-ContractEqual $continued.evidence_class 'runtime_observed' (
        'A runtime reboot continuation was relabeled.'
    )
    Require-Contract $continued.actual_scenario_pass (
        'A complete runtime reboot continuation was not an actual scenario PASS.'
    )
    Require-ContractEqual (
        $rebootReceipt | ConvertTo-Json -Depth 20
    ) $originalReceiptJson 'The reboot continuation mutated the original receipt.'

    $staleBoot = $null
    $staleBootError = $null
    try {
        $staleBoot = Test-WixInstallerScenarioRebootContinuation `
            -OriginalReceipt $rebootReceipt `
            -PostRebootReadback $postRebootReadback `
            -GuestBootUtc '2026-11-27T12:00:10Z' `
            -OwnedPendingDeletes @()
    }
    catch {
        $staleBootError = $_.Exception.Message
    }
    Require-Contract (
        ($null -ne $staleBoot -and $staleBoot.verdict -ceq 'NOT_COVERED') -or
        (-not [string]::IsNullOrEmpty($staleBootError) -and
            $staleBootError.StartsWith('[SC', [StringComparison]::Ordinal))
    ) "A guest boot no newer than Apply was not safely rejected: $staleBootError"

    $contradictedReadback = Copy-ContractObject $postRebootReadback
    $contradictedReadback.doctor_preserved = $false
    $contradictedContinuation = Test-WixInstallerScenarioRebootContinuation `
        -OriginalReceipt $rebootReceipt `
        -PostRebootReadback $contradictedReadback `
        -GuestBootUtc '2026-11-27T12:05:00Z' `
        -OwnedPendingDeletes @()
    Require-ContractEqual $contradictedContinuation.verdict 'FAIL' (
        'A contradictory post-reboot final state was not failed.'
    )

    foreach ($processName in @('first_process', 'second_process')) {
        $missingExitReceipt = Copy-ContractObject $rebootReceipt
        $missingExitEvidence = Copy-ContractObject $rebootEvidence
        $missingExitEvidence.PSObject.Properties[$processName].Value =
            @{ exited = $true; pid = 4242 }
        $missingExitReceipt.evidence = $missingExitEvidence
        $missingExit = Test-WixInstallerScenarioRebootContinuation `
            -OriginalReceipt $missingExitReceipt `
            -PostRebootReadback $postRebootReadback `
            -GuestBootUtc '2026-11-27T12:05:00Z' `
            -OwnedPendingDeletes @()
        Require-ContractEqual $missingExit.verdict 'NOT_COVERED' (
            "A missing $processName native exit code was accepted."
        )
    }

    $secondMsiReceipt = Copy-ContractObject $rebootReceipt
    $secondMsiEvidence = Copy-ContractObject $rebootEvidence
    $secondMsiEvidence.second_msi_transactions = 1
    $secondMsiEvidence.second_msi_commits = 1
    $secondMsiReceipt.evidence = $secondMsiEvidence
    $secondMsiContinuation = Test-WixInstallerScenarioRebootContinuation `
        -OriginalReceipt $secondMsiReceipt `
        -PostRebootReadback $postRebootReadback `
        -GuestBootUtc '2026-11-27T12:05:00Z' `
        -OwnedPendingDeletes @()
    Require-ContractEqual $secondMsiContinuation.verdict 'FAIL' (
        'A post-reboot receipt with a second MSI transaction was accepted.'
    )

    $pendingDeleteContinuation = Test-WixInstallerScenarioRebootContinuation `
        -OriginalReceipt $rebootReceipt `
        -PostRebootReadback $postRebootReadback `
        -GuestBootUtc '2026-11-27T12:05:00Z' `
        -OwnedPendingDeletes @(
            [pscustomobject]@{ path = 'C:\Owned Fixture\pending-delete.bin' }
        )
    Require-Contract (
        $pendingDeleteContinuation.verdict -cne 'PASS'
    ) 'An owned pending delete produced an actual Reentry PASS.'
    Require-Contract (-not $pendingDeleteContinuation.actual_scenario_pass) (
        'An owned pending delete was labeled as an actual scenario PASS.'
    )

    foreach ($ineligibleReceiptState in @(
        @{ property = 'root_guest_reboot_required'; value = $false },
        @{ property = 'unexpected_process_live'; value = $true }
    )) {
        $ineligibleReceipt = Copy-ContractObject $rebootReceipt
        $receiptStateProperty = $ineligibleReceipt.PSObject.Properties[
            $ineligibleReceiptState.property]
        $receiptStateProperty.Value = $ineligibleReceiptState.value
        $ineligibleContinuation = Test-WixInstallerScenarioRebootContinuation `
            -OriginalReceipt $ineligibleReceipt `
            -PostRebootReadback $postRebootReadback `
            -GuestBootUtc '2026-11-27T12:05:00Z' `
            -OwnedPendingDeletes @()
        Require-ContractEqual $ineligibleContinuation.verdict 'NOT_COVERED' (
            "Ineligible receipt state $($ineligibleReceiptState.property) was continued."
        )
    }

    foreach ($requiredObservation in @(
        'new_bundle_registered',
        'manifest_integrity_ok',
        'registration_status_ok',
        'settings_preserved',
        'doctor_preserved',
        'old_bundle_and_msi_registrations_absent'
    )) {
        $contradictedReentryReadback = Copy-ContractObject $reentryReadback
        $contradictedReentryReadback.PSObject.Properties[$requiredObservation].Value = $false
        $contradictedReentry = Copy-ContractObject $reentry3010
        $contradictedReentry.readback = $contradictedReentryReadback
        Require-SyntheticContractVerdict Reentry (
            Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $contradictedReentry
        ) FAIL -Context ("Reentry 3010 $requiredObservation=false")

        foreach ($unobservedState in @('missing', 'null')) {
            $unobservedReentryReadback = Copy-ContractObject $reentryReadback
            if ($unobservedState -ceq 'missing') {
                $unobservedReentryReadback.PSObject.Properties.Remove($requiredObservation) | Out-Null
            }
            else {
                $unobservedReentryReadback.PSObject.Properties[$requiredObservation].Value = $null
            }
            $unobservedReentry = Copy-ContractObject $reentry3010
            $unobservedReentry.readback = $unobservedReentryReadback
            Require-SyntheticContractVerdict Reentry (
                Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $unobservedReentry
            ) NOT_COVERED -Context ("Reentry 3010 $requiredObservation is $unobservedState")
        }
    }

    foreach ($runtimeVersionField in @(
        'observed_runtime_version',
        'observed_payload_version',
        'observed_com_version'
    )) {
        $contradictedRuntimeReadback = Copy-ContractObject $reentryReadback
        $contradictedRuntimeReadback.PSObject.Properties[$runtimeVersionField].Value = '1.0.0'
        $contradictedRuntimeVersion = Copy-ContractObject $reentry3010
        $contradictedRuntimeVersion.readback = $contradictedRuntimeReadback
        Require-SyntheticContractVerdict Reentry (
            Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $contradictedRuntimeVersion
        ) FAIL -Context ("Reentry 3010 $runtimeVersionField explicitly contradicted")

        foreach ($unobservedState in @('missing', 'null')) {
            $unobservedRuntimeReadback = Copy-ContractObject $reentryReadback
            if ($unobservedState -ceq 'missing') {
                $unobservedRuntimeReadback.PSObject.Properties.Remove($runtimeVersionField) | Out-Null
            }
            else {
                $unobservedRuntimeReadback.PSObject.Properties[$runtimeVersionField].Value = $null
            }
            $unobservedRuntimeVersion = Copy-ContractObject $reentry3010
            $unobservedRuntimeVersion.readback = $unobservedRuntimeReadback
            Require-SyntheticContractVerdict Reentry (
                Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $unobservedRuntimeVersion
            ) NOT_COVERED -Context ("Reentry 3010 $runtimeVersionField is $unobservedState")
        }
    }

    foreach ($msiVersionField in @('burn_version', 'msi_product_version')) {
        $contradictedMsiVersion = Copy-ContractObject $reentry3010
        $contradictedMsiVersion.PSObject.Properties[$msiVersionField].Value = '70.1.0'
        Require-SyntheticContractVerdict Reentry (
            Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $contradictedMsiVersion
        ) FAIL -Context ("Reentry 3010 $msiVersionField explicitly contradicted")

        foreach ($unobservedState in @('missing', 'null')) {
            $unobservedMsiVersion = Copy-ContractObject $reentry3010
            if ($unobservedState -ceq 'missing') {
                $unobservedMsiVersion.PSObject.Properties.Remove($msiVersionField) | Out-Null
            }
            else {
                $unobservedMsiVersion.PSObject.Properties[$msiVersionField].Value = $null
            }
            Require-SyntheticContractVerdict Reentry (
                Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $unobservedMsiVersion
            ) NOT_COVERED -Context ("Reentry 3010 $msiVersionField is $unobservedState")
        }
    }

    foreach ($expectedVersionField in @('expected_runtime_version', 'expected_msi_version')) {
        foreach ($unobservedState in @('missing', 'null')) {
            $missingExpectedReadback = Copy-ContractObject $reentryReadback
            if ($unobservedState -ceq 'missing') {
                $missingExpectedReadback.PSObject.Properties.Remove($expectedVersionField) | Out-Null
            }
            else {
                $missingExpectedReadback.PSObject.Properties[$expectedVersionField].Value = $null
            }
            $missingExpectedVersion = Copy-ContractObject $reentry3010
            $missingExpectedVersion.readback = $missingExpectedReadback
            Require-SyntheticContractVerdict Reentry (
                Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $missingExpectedVersion
            ) NOT_COVERED -Context ("Reentry 3010 $expectedVersionField is $unobservedState")
        }
    }

    $secondWithoutLock = New-ContractLog 'second' @(
        '[0002:0002][2026-11-27T12:00:04Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0002:0002][2026-11-27T12:00:04Z]i410: Variable: WixBundleVersion = 70.1.1',
        '[0002:0002][2026-11-27T12:00:05Z]i500: Shutting down, exit code: 0x0'
    )
    $noLock = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry `
        -BurnLogs @($firstBurn, $secondWithoutLock) `
        -MsiLogs @($firstMsi) `
        -Readback $reentryReadback `
        -EvidenceClass synthetic_contract_only
    Require-SyntheticContractVerdict Reentry (Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $noLock) NOT_COVERED

    $lateSecond = New-ContractLog 'second' @(
        '[0002:0002][2026-11-27T12:00:12Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0002:0002][2026-11-27T12:00:12Z]i410: Variable: WixBundleVersion = 70.1.1',
        '[0002:0002][2026-11-27T12:00:12Z]e000: LIUKAI_GUARD_BUSY stage=acquire hr=0x80070652',
        '[0002:0002][2026-11-27T12:00:13Z]i500: Shutting down, exit code: 0x80070652'
    )
    $nonOverlap = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry `
        -BurnLogs @($firstBurn, $lateSecond) `
        -MsiLogs @($firstMsi) `
        -Readback $reentryReadback `
        -EvidenceClass synthetic_contract_only
    Require-SyntheticContractVerdict Reentry (Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $nonOverlap) NOT_COVERED

    $firstLocalBurn = New-ContractLog 'first' @(
        '[0001:0001][2026-11-27T12:00:00]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0001:0001][2026-11-27T12:00:01]i410: Variable: WixBundleVersion = 70.1.1',
        '[0001:0001][2026-11-27T12:00:02]i300: Apply begin',
        '[0001:0001][2026-11-27T12:00:10]i399: Apply complete, result: 0x0',
        '[0001:0001][2026-11-27T12:00:11]i500: Shutting down, exit code: 0x0'
    )
    $secondLocalBurn = New-ContractLog 'second' @(
        '[0002:0002][2026-11-27T12:00:04]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0002:0002][2026-11-27T12:00:04]i410: Variable: WixBundleVersion = 70.1.1',
        '[0002:0002][2026-11-27T12:00:05]e000: LIUKAI_GUARD_BUSY stage=acquire hr=0x80070652',
        '[0002:0002][2026-11-27T12:00:06]i500: Shutting down, exit code: 0x80070652'
    )
    $localTimeOverlap = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry `
        -BurnLogs @($firstLocalBurn, $secondLocalBurn) `
        -MsiLogs @($firstMsi) `
        -Readback $reentryReadback `
        -EvidenceClass synthetic_contract_only
    Require-ContractEqual $localTimeOverlap.first_apply_start_utc (
        Convert-ContractLocalTimestampToUtc '2026-11-27T12:00:02'
    ) 'Burn timestamps without a suffix were not interpreted as local time.'
    Require-ContractEqual $localTimeOverlap.second_invocation_start_utc (
        Convert-ContractLocalTimestampToUtc '2026-11-27T12:00:04'
    ) 'The second local-time Burn timestamp was not converted to UTC.'
    Require-SyntheticContractVerdict Reentry (
        Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $localTimeOverlap
    ) PASS

    $secondMsi = New-ContractLog 'second' $firstMsi.text.Split("`n")
    $secondTransaction = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry `
        -BurnLogs @($firstBurn, $secondLockBurn) `
        -MsiLogs @($firstMsi, $secondMsi) `
        -Readback $reentryReadback `
        -EvidenceClass synthetic_contract_only
    Require-SyntheticContractVerdict Reentry (Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $secondTransaction) FAIL

    $failedFirstBurn = New-ContractLog 'first' @(
        '[0001:0001][2026-11-27T12:00:00Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0001:0001][2026-11-27T12:00:01Z]i410: Variable: WixBundleVersion = 70.1.1',
        '[0001:0001][2026-11-27T12:00:02Z]i300: Apply begin',
        '[0001:0001][2026-11-27T12:00:10Z]i399: Apply complete, result: 0x80004005',
        '[0001:0001][2026-11-27T12:00:11Z]i500: Shutting down, exit code: 0x80004005'
    )
    $failedFirst = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry `
        -BurnLogs @($failedFirstBurn, $secondLockBurn) `
        -MsiLogs @($firstMsi) `
        -Readback $reentryReadback `
        -EvidenceClass synthetic_contract_only
    Require-SyntheticContractVerdict Reentry (Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $failedFirst) FAIL

    $overRetriedReadback = Copy-ContractObject $reentryReadback
    $overRetriedReadback.targeted_reentry_retries = 2
    $overRetried = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Reentry `
        -BurnLogs @($firstBurn, $secondWithoutLock) `
        -MsiLogs @($firstMsi) `
        -Readback $overRetriedReadback `
        -EvidenceClass synthetic_contract_only
    Require-SyntheticContractVerdict Reentry (Get-WixInstallerScenarioVerdict -Scenario Reentry -Evidence $overRetried) FAIL

    $interruptedBurn = New-ContractLog 'first' @(
        '[0003:0003][2026-11-27T13:00:00Z]i001: Burn x64 v6.0.2, Windows v10.0 x64',
        '[0003:0003][2026-11-27T13:00:01Z]i410: Variable: WixBundleVersion = 70.1.1',
        '[0003:0003][2026-11-27T13:00:02Z]i300: Apply begin',
        '[0003:0003][2026-11-27T13:00:08Z]i410: Applying package; MSI commit confirmed by the MSI log'
    )
    $recoveryReadback = New-ContractReadback
    $recoveryReadback | Add-Member -NotePropertyName injection_origin -NotePropertyValue 'candidate_ba_ui'
    $recoveryReadback | Add-Member -NotePropertyName injection_attempts -NotePropertyValue 2
    $recoveryReadback | Add-Member -NotePropertyName bundle_consistent_at_interrupt -NotePropertyValue $false
    $recoveryReadback | Add-Member -NotePropertyName normal_ui_rerun_mode -NotePropertyValue 'normal_double_click'
    $recoveryReadback | Add-Member -NotePropertyName rerun_arguments -NotePropertyValue @()
    $recoveryReadback | Add-Member -NotePropertyName rerun_converged -NotePropertyValue $true
    $recoveryReadback | Add-Member -NotePropertyName uninstall_succeeded -NotePropertyValue $true
    $recoveryReadback | Add-Member -NotePropertyName normal_rerun_process -NotePropertyValue (
        [pscustomobject]@{ exited = $true; exit_code = 0 }
    )
    $recoveryReadback | Add-Member -NotePropertyName normal_uninstall_process -NotePropertyValue (
        [pscustomobject]@{ exited = $true; exit_code = 0 }
    )

    $recovery = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Recovery `
        -BurnLogs @($interruptedBurn) `
        -MsiLogs @($firstMsi) `
        -Readback $recoveryReadback `
        -EvidenceClass synthetic_contract_only
    Require-SyntheticContractVerdict Recovery (Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $recovery) PASS

    foreach ($processField in @('normal_rerun_process', 'normal_uninstall_process')) {
        foreach ($missingShape in @(
            @{ name = 'missing process'; value = $null; remove = $true },
            @{ name = 'missing exit'; value = [pscustomobject]@{ exited = $true }; remove = $false },
            @{ name = 'not exited'; value = [pscustomobject]@{ exited = $false; exit_code = 0 }; remove = $false }
        )) {
            $missingProcessReadback = Copy-ContractObject $recoveryReadback
            if ($missingShape.remove) {
                $missingProcessReadback.PSObject.Properties.Remove($processField) | Out-Null
            }
            else {
                $missingProcessReadback.PSObject.Properties[$processField].Value = $missingShape.value
            }
            $missingProcessRecovery = ConvertFrom-WixInstallerScenarioLogs `
                -Scenario Recovery -BurnLogs @($interruptedBurn) -MsiLogs @($firstMsi) `
                -Readback $missingProcessReadback -EvidenceClass synthetic_contract_only
            Require-SyntheticContractVerdict Recovery (
                Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $missingProcessRecovery
            ) NOT_COVERED -Context "$processField $($missingShape.name)"
        }

        foreach ($badExit in @(1603, -2147023278)) {
            $badProcessReadback = Copy-ContractObject $recoveryReadback
            $badProcessReadback.PSObject.Properties[$processField].Value =
                [pscustomobject]@{ exited = $true; exit_code = $badExit }
            $badProcessRecovery = ConvertFrom-WixInstallerScenarioLogs `
                -Scenario Recovery -BurnLogs @($interruptedBurn) -MsiLogs @($firstMsi) `
                -Readback $badProcessReadback -EvidenceClass synthetic_contract_only
            Require-SyntheticContractVerdict Recovery (
                Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $badProcessRecovery
            ) FAIL -Context "$processField exit $badExit"
        }

        $rebootProcessReadback = Copy-ContractObject $recoveryReadback
        $rebootProcessReadback.PSObject.Properties[$processField].Value =
            [pscustomobject]@{ exited = $true; exit_code = 3010 }
        $rebootProcessRecovery = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Recovery -BurnLogs @($interruptedBurn) -MsiLogs @($firstMsi) `
            -Readback $rebootProcessReadback -EvidenceClass synthetic_contract_only
        Require-SyntheticContractVerdict Recovery (
            Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $rebootProcessRecovery
        ) NOT_COVERED -Context "$processField exit 3010"
    }

    foreach ($versionField in @(
        'observed_runtime_version',
        'observed_payload_version',
        'observed_com_version'
    )) {
        $wrongRecoveryVersion = Copy-ContractObject $recoveryReadback
        $wrongRecoveryVersion.PSObject.Properties[$versionField].Value = '1.0.0'
        $wrongRecovery = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Recovery `
            -BurnLogs @($interruptedBurn) `
            -MsiLogs @($firstMsi) `
            -Readback $wrongRecoveryVersion `
            -EvidenceClass synthetic_contract_only
        Require-SyntheticContractVerdict Recovery (
            Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $wrongRecovery
        ) FAIL -Context ("Recovery $versionField explicitly contradicted")

        $missingRecoveryVersion = Copy-ContractObject $recoveryReadback
        $missingRecoveryVersion.PSObject.Properties.Remove($versionField) | Out-Null
        $incompleteRecoveryVersion = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Recovery `
            -BurnLogs @($interruptedBurn) `
            -MsiLogs @($firstMsi) `
            -Readback $missingRecoveryVersion `
            -EvidenceClass synthetic_contract_only
        Require-SyntheticContractVerdict Recovery (
            Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $incompleteRecoveryVersion
        ) NOT_COVERED -Context ("Recovery $versionField is absent")
    }

    $noCommitReadback = Copy-ContractObject $recoveryReadback
    $noCommitReadback.bundle_consistent_at_interrupt = $false
    $noCommit = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Recovery `
        -BurnLogs @($interruptedBurn) `
        -MsiLogs @() `
        -Readback $noCommitReadback `
        -EvidenceClass synthetic_contract_only
    Require-SyntheticContractVerdict Recovery (Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $noCommit) NOT_COVERED

    $unprovenMsi = New-ContractLog 'first' @(
        'MSI (s) (AA:AA) [13:00:09]: Product: Synthetic LiuKai -- Installation completed successfully.',
        'MSI (s) (AA:AA) [13:00:09]: Property(S): ProductVersion = 70.1.1'
    )
    $unprovenCommit = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Recovery `
        -BurnLogs @($interruptedBurn) `
        -MsiLogs @($unprovenMsi) `
        -Readback $noCommitReadback `
        -EvidenceClass synthetic_contract_only
    Require-Contract ($unprovenCommit.first_msi_commits -eq 0) 'MSI completion text without InstallFinalize success was treated as a commit.'
    Require-SyntheticContractVerdict Recovery (Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $unprovenCommit) NOT_COVERED

    $consistentReadback = Copy-ContractObject $recoveryReadback
    $consistentReadback.bundle_consistent_at_interrupt = $true
    $consistentState = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Recovery `
        -BurnLogs @($interruptedBurn) `
        -MsiLogs @($firstMsi) `
        -Readback $consistentReadback `
        -EvidenceClass synthetic_contract_only
    Require-SyntheticContractVerdict Recovery (Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $consistentState) NOT_COVERED

    $directMsiReadback = Copy-ContractObject $recoveryReadback
    $directMsiReadback.injection_origin = 'direct_msi'
    $directMsi = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Recovery `
        -BurnLogs @($interruptedBurn) `
        -MsiLogs @($firstMsi) `
        -Readback $directMsiReadback `
        -EvidenceClass synthetic_contract_only
    Require-SyntheticContractVerdict Recovery (Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $directMsi) NOT_COVERED

    $tooManyAttempts = Copy-ContractObject $recoveryReadback
    $tooManyAttempts.injection_attempts = 3
    $unbounded = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario Recovery `
        -BurnLogs @($interruptedBurn) `
        -MsiLogs @() `
        -Readback $tooManyAttempts `
        -EvidenceClass synthetic_contract_only
    Require-SyntheticContractVerdict Recovery (Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $unbounded) FAIL

    foreach ($broken in @(
        @{ name = 'normal_ui_rerun_mode'; value = 'passive' },
        @{ name = 'rerun_arguments'; value = @('/passive') },
        @{ name = 'rerun_converged'; value = $false },
        @{ name = 'old_bundle_and_msi_registrations_absent'; value = $false },
        @{ name = 'manifest_integrity_ok'; value = $false },
        @{ name = 'registration_status_ok'; value = $false },
        @{ name = 'new_bundle_registered'; value = $false },
        @{ name = 'settings_preserved'; value = $false },
        @{ name = 'doctor_preserved'; value = $false },
        @{ name = 'uninstall_succeeded'; value = $false }
    )) {
        $brokenReadback = Copy-ContractObject $recoveryReadback
        $brokenReadback.PSObject.Properties[$broken.name].Value = $broken.value
        $brokenRecovery = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Recovery `
            -BurnLogs @($interruptedBurn) `
            -MsiLogs @($firstMsi) `
            -Readback $brokenReadback `
            -EvidenceClass synthetic_contract_only
        Require-SyntheticContractVerdict Recovery (
            Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $brokenRecovery
        ) FAIL -Context "Recovery $($broken.name)=$($broken.value)"
    }

    foreach ($requiredObservation in @(
        'manifest_integrity_ok',
        'registration_status_ok',
        'new_bundle_registered',
        'settings_preserved',
        'doctor_preserved'
    )) {
        $missingRecoveryObservation = Copy-ContractObject $recoveryReadback
        $missingRecoveryObservation.PSObject.Properties.Remove($requiredObservation) | Out-Null
        $incompleteRecovery = ConvertFrom-WixInstallerScenarioLogs `
            -Scenario Recovery `
            -BurnLogs @($interruptedBurn) `
            -MsiLogs @($firstMsi) `
            -Readback $missingRecoveryObservation `
            -EvidenceClass synthetic_contract_only
        Require-SyntheticContractVerdict Recovery (
            Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $incompleteRecovery
        ) NOT_COVERED -Context ("Recovery $requiredObservation is absent")
    }

    $expectedBa = [pscustomobject]@{
        path = Join-Path $testRoot 'candidate-ba.exe'
        sha256 = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
        pid = 4242
        creation_time_utc = '2026-11-27T12:00:00Z'
    }
    $candidateBa = [pscustomobject]@{
        process_kind = 'candidate_ba_ui'
        path = $expectedBa.path
        sha256 = $expectedBa.sha256
        pid = $expectedBa.pid
        creation_time_utc = $expectedBa.creation_time_utc
        elevated = $false
    }
    $eligible = Test-WixInstallerScenarioKillEligible -ExpectedTarget $expectedBa -ObservedProcess $candidateBa
    Require-Contract $eligible.eligible 'An exact, verified, non-elevated candidate BA was rejected.'

    foreach ($identity in @(
        @{ name = 'path'; value = Join-Path $testRoot 'other.exe' },
        @{ name = 'sha256'; value = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb' },
        @{ name = 'pid'; value = 4243 },
        @{ name = 'creation_time_utc'; value = '2026-11-27T12:00:01Z' },
        @{ name = 'elevated'; value = $true },
        @{ name = 'process_kind'; value = 'msiexec' },
        @{ name = 'process_kind'; value = 'windows_installer_service' },
        @{ name = 'process_kind'; value = 'burn_engine' },
        @{ name = 'process_kind'; value = 'elevated_burn_engine' },
        @{ name = 'process_kind'; value = 'host_process' }
    )) {
        $changed = Copy-ContractObject $candidateBa
        $changed.PSObject.Properties[$identity.name].Value = $identity.value
        $decision = Test-WixInstallerScenarioKillEligible -ExpectedTarget $expectedBa -ObservedProcess $changed
        Require-Contract (-not $decision.eligible) "Unsafe kill target was accepted for $($identity.name)=$($identity.value)."
        Require-Contract (-not [string]::IsNullOrWhiteSpace([string]$decision.reason)) 'A rejected kill target did not explain why.'
    }

    $nameOnly = [pscustomobject]@{
        process_kind = 'candidate_ba_ui'
        process_name = 'LiuKaiInstaller.exe'
    }
    Require-Contract (
        -not (Test-WixInstallerScenarioKillEligible -ExpectedTarget $expectedBa -ObservedProcess $nameOnly).eligible
    ) 'A name-only process observation became kill eligible.'

    $bundleIdentity = [pscustomobject]@{ msi_version = '70.1.1' }
    $bundleSnapshot = [pscustomobject]@{
        uninstall_registry_issues = @()
        uninstall_entries = @([pscustomobject]@{
            kind = 'burn_bundle'
            display_version = '70.1.1'
            values = [pscustomobject]@{ Installed = 1 }
        })
    }
    Require-Contract (Test-WixInstallerScenarioBundleMatches -Snapshot $bundleSnapshot -Identity $bundleIdentity) 'An installed matching bundle was rejected.'
    $bundleSnapshot.uninstall_entries[0].values.Installed = 0
    Require-Contract (-not (Test-WixInstallerScenarioBundleMatches -Snapshot $bundleSnapshot -Identity $bundleIdentity)) 'A matching bundle with Installed=0 was accepted.'
    $bundleSnapshot.uninstall_entries[0].values = [pscustomobject]@{}
    Require-Contract (-not (Test-WixInstallerScenarioBundleMatches -Snapshot $bundleSnapshot -Identity $bundleIdentity)) 'A matching bundle without Installed was accepted.'

    $statePath = Join-Path $testRoot 'progress-state.json'
    $atomicWriteError = $null
    try {
        Write-WixInstallerScenarioJson $statePath ([ordered]@{ phase = 'before'; value = 1 })
        Write-WixInstallerScenarioJson $statePath ([ordered]@{ phase = 'after'; value = 2 })
    }
    catch { $atomicWriteError = $_.Exception.Message }
    Require-Contract ($null -eq $atomicWriteError) "Repeated JSON receipt update failed: $atomicWriteError"
    if ($null -eq $atomicWriteError) {
        $state = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
        Require-Contract ($state.phase -ceq 'after' -and $state.value -eq 2) 'JSON receipt did not contain the complete replacement.'
        Require-Contract (@(Get-ChildItem -LiteralPath $testRoot -Filter '.progress-state.json.*.tmp').Count -eq 0) 'JSON receipt left an incomplete temporary replacement.'
    }

    if ($script:ContractFailures.Count -gt 0) {
        $summary = @(
            $script:ContractFailures |
                Select-Object -First 16 |
                ForEach-Object { '- ' + $_ }
        ) -join [Environment]::NewLine
        $omitted = [Math]::Max(0, $script:ContractFailures.Count - 16)
        if ($omitted -gt 0) {
            $summary += [Environment]::NewLine + "- ... $omitted additional contract failure(s)"
        }
        throw "[SC900] Contract regressions=$($script:ContractFailures.Count).$([Environment]::NewLine)$summary"
    }
    Write-Output (
        'wix_installer_scenario_contracts_passed=' + $script:ContractAssertions +
        '; evidence=synthetic_contract_only; installer_runs=0; process_actions=0; registry_actions=0'
    )
}
finally {
    if ((Test-Path -LiteralPath $testRoot) -and
        [IO.Path]::GetFullPath($testRoot).StartsWith($tempBase + '\', [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $testRoot) -like 'liu-kai-installer-contract-*') {
        [IO.Directory]::Delete($testRoot, $true)
    }
}
