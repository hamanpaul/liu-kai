#Requires -Version 5.1

[CmdletBinding(DefaultParameterSetName = 'Runtime')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Runtime')]
    [string]$BaselineSetup,

    [Parameter(Mandatory = $true, ParameterSetName = 'Runtime')]
    [string]$CandidateSetup,

    [Parameter(Mandatory = $true, ParameterSetName = 'Runtime')]
    [string]$InputManifest,

    [Parameter(Mandatory = $true, ParameterSetName = 'Runtime')]
    [string]$OutputDir,

    [Parameter(Mandatory = $true, ParameterSetName = 'Runtime')]
    [ValidateSet('Upgrade', 'Reentry', 'Recovery')]
    [string]$Scenario,

    [string]$ToolchainBin = '',

    [ValidateRange(60, 1800)]
    [int]$ObservationTimeoutSeconds = 900,

    [ValidateRange(1, 600)]
    [int]$MappedModuleHoldSeconds = 600,

    [Parameter(ParameterSetName = 'Runtime')]
    [switch]$AllowElevatedBaUi,

    [Parameter(ParameterSetName = 'Runtime')]
    [ValidateRange(1, 2)]
    [int]$RecoveryAttempts = 2,

    [Parameter(ParameterSetName = 'Runtime')]
    [switch]$AcceptRetainedBundle,

    [Parameter(Mandatory = $true, ParameterSetName = 'Contract')]
    [switch]$ContractOnly
)

function Get-WixInstallerScenarioProperty {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        $Default = $null
    )

    if ($null -eq $Object) {
        return $Default
    }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) {
            return $Object[$Name]
        }
        return $Default
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $Default
    }
    return $property.Value
}

function Test-WixInstallerScenarioProperty {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -eq $Object) {
        return $false
    }
    if ($Object -is [System.Collections.IDictionary]) {
        return $Object.Contains($Name)
    }
    return $null -ne $Object.PSObject.Properties[$Name]
}

function Get-WixInstallerScenarioVersion {
    param([AllowNull()][string]$Text)

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return $null
    }
    try {
        return [Version]$Text
    }
    catch [FormatException] {
        return $null
    }
    catch [ArgumentException] {
        return $null
    }
}

function Test-WixInstallerScenarioSuccessfulExit {
    param([AllowNull()][string]$Code)

    if ([string]::IsNullOrWhiteSpace($Code)) {
        return $false
    }
    try {
        $text = $Code.Trim()
        $value = if ($text.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
            [Convert]::ToInt64($text.Substring(2), 16)
        } else {
            [Convert]::ToInt64($text, 10)
        }
        return $value -eq 0 -or $value -eq 3010
    }
    catch [FormatException] {
        return $false
    }
    catch [OverflowException] {
        return $false
    }
}

function Assert-WixInstallerScenarioPreflight {
    param(
        [Parameter(Mandatory = $true, Position = 0)][string]$BaselineSetup,
        [Parameter(Mandatory = $true, Position = 1)][string]$CandidateSetup,
        [Parameter(Mandatory = $true, Position = 2)][string]$OutputDir,
        [Parameter(Mandatory = $true, Position = 3)]
        [ValidateSet('Upgrade', 'Reentry', 'Recovery')]
        [string]$Scenario,
        [Parameter(Mandatory = $true, Position = 4)][string]$InputManifest,
        [Parameter(Mandatory = $true, Position = 5)]$GuestObservation
    )

    $manifest = $null
    try {
        if (-not (Test-Path -LiteralPath $InputManifest -PathType Leaf)) {
            throw 'The input manifest is missing.'
        }
        $manifest = Get-Content -LiteralPath $InputManifest -Raw -Encoding UTF8 -ErrorAction Stop |
            ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "[SC011] Cannot read the supplied input manifest: $($_.Exception.Message)"
    }
    if ((Get-WixInstallerScenarioProperty $manifest 'schema_version') -ne 1) {
        throw '[SC011] The supplied input manifest schema_version must be 1.'
    }

    $authorization = Get-WixInstallerScenarioProperty $manifest 'owned_instance'
    $manifestSandboxId = [string](Get-WixInstallerScenarioProperty $authorization 'sandbox_id')
    $manifestSentinel = [string](Get-WixInstallerScenarioProperty $authorization 'sentinel_path')
    $manifestSentinelHash = [string](Get-WixInstallerScenarioProperty $authorization 'sentinel_sha256')
    $authorizationKind = [string](Get-WixInstallerScenarioProperty $authorization 'authorization_kind')
    $guestKind = [string](Get-WixInstallerScenarioProperty $GuestObservation 'authorization_kind')
    $guestSandboxId = [string](Get-WixInstallerScenarioProperty $GuestObservation 'sandbox_id')
    $guestSentinel = [string](Get-WixInstallerScenarioProperty $GuestObservation 'sentinel_path')
    $guestSentinelHash = [string](Get-WixInstallerScenarioProperty $GuestObservation 'sentinel_sha256')
    $guestPresent = Get-WixInstallerScenarioProperty $GuestObservation 'sentinel_present' $false
    $isSandbox = Get-WixInstallerScenarioProperty $GuestObservation 'is_windows_sandbox_guest' $false
    if ($authorizationKind -cne 'owned-windows-sandbox-v1' -or
        $guestKind -cne $authorizationKind -or
        -not $isSandbox -or
        [string]::IsNullOrWhiteSpace($manifestSandboxId) -or
        $guestSandboxId -cne $manifestSandboxId -or
        -not $guestPresent -or
        [string]::IsNullOrWhiteSpace($manifestSentinel) -or
        $guestSentinel -cne $manifestSentinel -or
        $manifestSentinelHash -cnotmatch '^[0-9a-f]{64}$' -or
        $guestSentinelHash -cne $manifestSentinelHash) {
        throw '[SC010] The explicitly owned Windows Sandbox identity or guest-local sentinel was not verified.'
    }
    try {
        $sentinelItem = Get-Item -LiteralPath $manifestSentinel -ErrorAction Stop
        if (($sentinelItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            (Get-FileHash -LiteralPath $manifestSentinel -Algorithm SHA256).Hash.ToLowerInvariant() -cne $manifestSentinelHash) {
            throw 'Sentinel mismatch.'
        }
    }
    catch {
        throw "[SC010] The owned guest sentinel cannot be verified: $($_.Exception.Message)"
    }

    $resolved = @{}
    $baselineIdentity = Get-WixInstallerScenarioProperty $manifest 'baseline'
    $candidateIdentity = Get-WixInstallerScenarioProperty $manifest 'candidate'
    foreach ($entry in @(
        @{ name = 'baseline'; path = $BaselineSetup; identity = $baselineIdentity },
        @{ name = 'candidate'; path = $CandidateSetup; identity = $candidateIdentity }
    )) {
        if ($null -eq $entry.identity -or -not (Test-Path -LiteralPath $entry.path -PathType Leaf)) {
            throw "[SC011] The $($entry.name) setup or manifest identity is missing."
        }
        try {
            $path = (Resolve-Path -LiteralPath $entry.path -ErrorAction Stop).Path
            $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
        }
        catch {
            throw "[SC011] Cannot hash the $($entry.name) setup: $($_.Exception.Message)"
        }
        $expectedHash = [string](Get-WixInstallerScenarioProperty $entry.identity 'setup_sha256')
        if ($expectedHash -cnotmatch '^[0-9a-f]{64}$' -or $hash -cne $expectedHash) {
            throw "[SC011] The $($entry.name) setup hash does not match the supplied manifest."
        }
        $runtimeVersion = [string](Get-WixInstallerScenarioProperty $entry.identity 'runtime_version')
        $burnVersion = [string](Get-WixInstallerScenarioProperty $entry.identity 'burn_version')
        $msiVersion = [string](Get-WixInstallerScenarioProperty $entry.identity 'msi_version')
        $runtimeParsed = Get-WixInstallerScenarioVersion $runtimeVersion
        $burnParsed = Get-WixInstallerScenarioVersion $burnVersion
        $msiParsed = Get-WixInstallerScenarioVersion $msiVersion
        if ($null -eq $runtimeParsed -or $null -eq $burnParsed -or $null -eq $msiParsed -or
            $burnVersion -cne $msiVersion) {
            throw "[SC012] The $($entry.name) runtime/Burn/MSI versions are invalid or inconsistent."
        }
        $resolved[$entry.name] = [pscustomobject]@{
            setup_path = $path
            setup_sha256 = $hash
            runtime_version = $runtimeVersion
            burn_version = $burnVersion
            msi_version = $msiVersion
            ba_payloads = Get-WixInstallerScenarioProperty $entry.identity 'ba_payloads' @()
        }
    }
    if ((Get-WixInstallerScenarioVersion $resolved.candidate.runtime_version) -le
        (Get-WixInstallerScenarioVersion $resolved.baseline.runtime_version) -or
        (Get-WixInstallerScenarioVersion $resolved.candidate.burn_version) -le
        (Get-WixInstallerScenarioVersion $resolved.baseline.burn_version) -or
        (Get-WixInstallerScenarioVersion $resolved.candidate.msi_version) -le
        (Get-WixInstallerScenarioVersion $resolved.baseline.msi_version)) {
        throw '[SC012] Candidate runtime, Burn, and MSI versions must all increase over baseline.'
    }

    $outputPath = [IO.Path]::GetFullPath($OutputDir)
    if (Test-Path -LiteralPath $outputPath) {
        throw '[SC013] The scenario output directory must not already exist.'
    }
    if (-not (Test-Path -LiteralPath (Split-Path -Parent $outputPath) -PathType Container)) {
        throw '[SC013] The scenario output parent must already exist.'
    }
    return [pscustomobject]@{
        scenario = $Scenario
        baseline = $resolved.baseline
        candidate = $resolved.candidate
        manifest_path = (Resolve-Path -LiteralPath $InputManifest).Path
        output_path = $outputPath
        owned_instance = [pscustomobject]@{
            authorization_kind = $authorizationKind
            sandbox_id = $manifestSandboxId
            sentinel_path = $manifestSentinel
            sentinel_sha256 = $manifestSentinelHash
        }
        manifest = $manifest
    }
}

function ConvertFrom-WixInstallerScenarioTimestamp {
    param([Parameter(Mandatory = $true)][string]$Value)

    try {
        $zone = [string]([regex]::Match(
            $Value,
            '(?<zone>Z|[+-]\d{2}:\d{2})$',
            [System.Text.RegularExpressions.RegexOptions]::None
        ).Groups['zone'].Value)
        if (-not [string]::IsNullOrWhiteSpace($zone)) {
            return [DateTimeOffset]::Parse(
                $Value,
                [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::RoundtripKind
            ).UtcDateTime.ToString('o')
        }
        $local = [DateTime]::Parse(
            $Value,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::None)
        $unspecified = [DateTime]::SpecifyKind($local, [DateTimeKind]::Unspecified)
        return [TimeZoneInfo]::ConvertTimeToUtc(
            $unspecified,
            [TimeZoneInfo]::Local).ToString('o')
    }
    catch [FormatException] {
        return $null
    }
    catch [ArgumentException] {
        return $null
    }
}

function Get-WixInstallerScenarioTimestamp {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text,
        [switch]$Last
    )

    $matches = [regex]::Matches(
        $Text,
        '\[(?<time>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})?)\]',
        [System.Text.RegularExpressions.RegexOptions]::None)
    if ($matches.Count -eq 0) {
        return $null
    }
    $match = if ($Last) { $matches[$matches.Count - 1] } else { $matches[0] }
    return ConvertFrom-WixInstallerScenarioTimestamp $match.Groups['time'].Value
}

function Get-WixInstallerScenarioFirstTimestamp {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)

    return Get-WixInstallerScenarioTimestamp -Text $Text
}

function Get-WixInstallerScenarioLastTimestamp {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)

    return Get-WixInstallerScenarioTimestamp -Text $Text -Last
}

function Get-WixInstallerScenarioLogValue {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [int]$Group = 1
    )

    $match = [regex]::Match(
        $Text,
        $Pattern,
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase -bor
        [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if (-not $match.Success) {
        return $null
    }
    return $match.Groups[$Group].Value
}

function ConvertFrom-WixInstallerScenarioLogs {
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [ValidateSet('Upgrade', 'Reentry', 'Recovery')]
        [string]$Scenario,
        [Parameter(Mandatory = $true, Position = 1)][AllowEmptyCollection()][object[]]$BurnLogs,
        [Parameter(Mandatory = $true, Position = 2)][AllowEmptyCollection()][object[]]$MsiLogs,
        [Parameter(Mandatory = $true, Position = 3)]$Readback,
        [Parameter(Mandatory = $true, Position = 4)]
        [ValidateSet('runtime_observed', 'synthetic_contract_only')]
        [string]$EvidenceClass
    )

    $firstBurnText = @(
        $BurnLogs | Where-Object { (Get-WixInstallerScenarioProperty $_ 'run') -ceq 'first' } |
            ForEach-Object { [string](Get-WixInstallerScenarioProperty $_ 'text') }
    ) -join "`r`n"
    $secondBurnText = @(
        $BurnLogs | Where-Object { (Get-WixInstallerScenarioProperty $_ 'run') -ceq 'second' } |
            ForEach-Object { [string](Get-WixInstallerScenarioProperty $_ 'text') }
    ) -join "`r`n"
    $expectedMsiVersion = [string](Get-WixInstallerScenarioProperty $Readback 'expected_msi_version')
    $firstMsiTransactionCount = 0
    $secondMsiTransactionCount = 0
    $firstCommitCount = 0
    $secondCommitCount = 0
    $firstMsiNativeExit = $null
    $msiVersion = $null
    $restartRequired = $false
    foreach ($entry in @($MsiLogs)) {
        $text = [string](Get-WixInstallerScenarioProperty $entry 'text')
        $summaryMatches = [regex]::Matches(
            $text,
            '(?:Windows\s+Installer\s+installed\s+the\s+product\.[^\r\n]*Product\s+Version:\s*(?<version>[0-9]+(?:\.[0-9]+){1,3})\.[^\r\n]*Installation\s+success\s+or\s+error\s+status:\s*(?<status>[0-9]+)\.|Windows Installer 已安裝該產品。[^\r\n]*產品版本:\s*(?<version>[0-9]+(?:\.[0-9]+){1,3})。[^\r\n]*安裝成功或錯誤狀態:\s*(?<status>[0-9]+)。\s*)',
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        $candidateSummary = $null
        foreach ($summaryMatch in @($summaryMatches)) {
            if ($summaryMatch.Groups['version'].Value -ceq $expectedMsiVersion) {
                $candidateSummary = $summaryMatch
                break
            }
        }
        $version = if ($summaryMatches.Count -gt 0) {
            if ($null -ne $candidateSummary) {
                $candidateSummary.Groups['version'].Value
            }
            else {
                $null
            }
        }
        else {
            $versionMatches = [regex]::Matches(
                $text,
                'ProductVersion\s*=\s*([0-9]+(?:\.[0-9]+){1,3})',
                [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
            if ($versionMatches.Count -gt 0) {
                $versionMatches[$versionMatches.Count - 1].Groups[1].Value
            }
            else {
                $null
            }
        }
        if ([string]::IsNullOrWhiteSpace($expectedMsiVersion) -or
            [string]::IsNullOrWhiteSpace($version) -or
            $version -cne $expectedMsiVersion) {
            continue
        }
        $msiVersion = $version
        $finalize = [regex]::IsMatch(
            $text,
            '(?:Action\s+ended[^\r\n]*InstallFinalize\.[^\r\n]*Return\s+value\s+1\.|動作結束[^\r\n]*InstallFinalize。傳回值 1。)',
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        $status = if ($null -ne $candidateSummary) {
            $candidateSummary.Groups['status'].Value
        }
        else {
            Get-WixInstallerScenarioLogValue $text 'Installation success or error status:\s*(0|3010)\.'
        }
        $nativeExits = [regex]::Matches($text,
            '(?im)^MSI [^\r\n]*MainEngineThread is returning (?<exit>[0-9]+)\s*$')
        $nativeRestart = $nativeExits.Count -gt 0 -and
            $nativeExits[$nativeExits.Count - 1].Groups['exit'].Value -ceq '3010'
        if ($status -ceq '3010' -or $nativeRestart) {
            $restartRequired = $true
        }
        $completed = if ($null -ne $candidateSummary) {
            $status -ceq '0' -or $status -ceq '3010'
        }
        else {
            -not [string]::IsNullOrWhiteSpace($status) -or
                [regex]::IsMatch(
                    $text,
                    '(?:Installation\s+completed\s+successfully|成功完成安裝)',
                    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        }
        $run = [string](Get-WixInstallerScenarioProperty $entry 'run')
        if ($run -ceq 'first') {
            $firstMsiTransactionCount++
            $nativeStart = 0
            foreach ($summary in @($summaryMatches)) {
                if ($summary.Groups['version'].Value -ceq $expectedMsiVersion) {
                    $nativeStart = $summary.Index + $summary.Length
                }
            }
            $productVersions = [regex]::Matches($text,
                '(?i)ProductVersion\s*=\s*(?<version>[0-9]+(?:\.[0-9]+){1,3})')
            if ($null -eq $candidateSummary -and $productVersions.Count -gt 0) {
                $lastVersion = $productVersions[$productVersions.Count - 1]
                $nativeStart = $lastVersion.Index + $lastVersion.Length
            }
            $nativeEnd = $text.Length
            foreach ($marker in @($summaryMatches) + @($productVersions)) {
                if ($marker.Index -ge $nativeStart -and
                    $marker.Groups['version'].Value -cne $expectedMsiVersion) {
                    $nativeEnd = [Math]::Min($nativeEnd, $marker.Index)
                }
            }
            $candidateNativeExits = [regex]::Matches($text.Substring($nativeStart, $nativeEnd - $nativeStart),
                '(?im)^MSI [^\r\n]*MainEngineThread is returning (?<exit>[0-9]+)\s*$')
            $firstMsiNativeExit = if ($candidateNativeExits.Count -gt 0) {
                $candidateNativeExits[$candidateNativeExits.Count - 1].Groups['exit'].Value
            } else { $null }
        }
        elseif ($run -ceq 'second') {
            $secondMsiTransactionCount++
        }
        if ($finalize -and $completed) {
            if ($run -ceq 'first') {
                $firstCommitCount++
            }
            elseif ($run -ceq 'second') {
                $secondCommitCount++
            }
        }
    }

    $secondLogPresent = -not [string]::IsNullOrWhiteSpace($secondBurnText)
    $secondApplyCount = if ($secondLogPresent) {
        ([regex]::Matches(
            $secondBurnText,
            '(?im)^[^\r\n]*Apply begin\b')).Count
    }
    else {
        $null
    }
    $secondSessionCount = if ($secondLogPresent) {
        ([regex]::Matches(
            $secondBurnText,
            '(?im)^[^\r\n]*Session begin,')).Count
    }
    else {
        $null
    }
    $guardMatch = [regex]::Match(
        $secondBurnText,
        '(?m)^[^\r\n]*LIUKAI_GUARD_BUSY[ \t]+stage=acquire[ \t]+hr=(?<hr>0x80070652)(?=[ \t]*\r?$)')
    $lockEvidence = $guardMatch.Success -and -not [regex]::IsMatch(
        $secondBurnText.Substring(0, $guardMatch.Index),
        '(?im)Apply begin\b|Session begin,')
    $secondTerminalMatches = [regex]::Matches(
        $secondBurnText,
        '(?im)^[^\r\n]*(?:Shutting down,\s*exit code:\s*|Exit code:\s*)(0x[0-9A-F]+|-?\d+)(?:,\s*restarting:\s*(?:Yes|No))?[ \t]*\r?$')
    $secondTerminalMatch = if ($secondTerminalMatches.Count -gt 0) {
        $secondTerminalMatches[$secondTerminalMatches.Count - 1]
    }
    else {
        $null
    }
    $secondTerminalExitCode = if ($null -ne $secondTerminalMatch) {
        $secondTerminalMatch.Groups[1].Value
    }
    else {
        $null
    }
    $secondHResult = if ($guardMatch.Success) {
        $guardMatch.Groups['hr'].Value
    }
    else {
        $null
    }

    return [pscustomobject]@{
        scenario = $Scenario
        evidence_class = $EvidenceClass
        first_apply_start_utc = if ($firstBurnText -match '(?im)^.*Apply begin') {
            Get-WixInstallerScenarioFirstTimestamp (
                Get-WixInstallerScenarioLogValue $firstBurnText '^(.*Apply begin.*)$')
        }
        else {
            $null
        }
        first_apply_end_utc = if ($firstBurnText -match '(?im)^.*Apply complete,\s*result:') {
            Get-WixInstallerScenarioFirstTimestamp (
                Get-WixInstallerScenarioLogValue $firstBurnText '^(.*Apply complete,\s*result:.*)$')
        }
        else {
            $null
        }
        first_apply_result = Get-WixInstallerScenarioLogValue $firstBurnText 'Apply complete,\s*result:\s*(0x[0-9A-F]+|\d+)'
        first_run_exit_code = Get-WixInstallerScenarioLogValue $firstBurnText 'exit code:\s*(0x[0-9A-F]+|\d+)'
        second_invocation_start_utc = Get-WixInstallerScenarioFirstTimestamp $secondBurnText
        second_invocation_end_utc = Get-WixInstallerScenarioLastTimestamp $secondBurnText
        second_native_lock_evidence = [bool]$lockEvidence
        second_guard_busy_before_apply = [bool]$lockEvidence
        second_apply_begin_count = $secondApplyCount
        second_session_begin_count = $secondSessionCount
        second_log_complete = if ($secondLogPresent) {
            $null -ne $secondTerminalMatch
        }
        else {
            $null
        }
        second_terminal_exit_code = $secondTerminalExitCode
        second_hresult = $secondHResult
        first_msi_transactions = $firstMsiTransactionCount
        second_msi_transactions = $secondMsiTransactionCount
        first_msi_commits = $firstCommitCount
        second_msi_commits = $secondCommitCount
        first_msi_native_exit = $firstMsiNativeExit
        restart_required = [bool]$restartRequired
        burn_version = Get-WixInstallerScenarioLogValue $firstBurnText `
            'Variable:\s*WixBundleVersion\s*=\s*([0-9]+(?:\.[0-9]+){1,3})'
        burn_engine_version = Get-WixInstallerScenarioLogValue $firstBurnText `
            'Burn(?:\s+(?:x86|x64|arm64))?\s+v([0-9]+(?:\.[0-9]+){1,3})'
        msi_product_version = $msiVersion
        raw_burn_logs = @($BurnLogs)
        raw_msi_logs = @($MsiLogs)
        readback = $Readback
    }
}

function Get-WixInstallerScenarioRecoveryPostExitState {
    param([AllowNull()]$Evidence)

    $nativeExit = Get-WixInstallerScenarioProperty $Evidence 'first_msi_native_exit' $null
    $known = (Get-WixInstallerScenarioProperty $Evidence 'first_msi_commits' 0) -eq 1 -and
        [string]$nativeExit -cin @('0', '3010')
    return [pscustomobject]@{
        known = [bool]$known
        native_exit = $nativeExit
        restart_required = if ($known) { [string]$nativeExit -ceq '3010' } else { $null }
    }
}

function Test-WixInstallerScenarioRecoveryRebootProof {
    param(
        [AllowNull()]$Readback,
        [AllowNull()]$Process,
        [AllowNull()]$Reboot,
        [AllowNull()]$NextProcess = $null
    )

    foreach ($name in @('owned_sandbox_id', 'sentinel_path', 'sentinel_sha256')) {
        $expected = [string](Get-WixInstallerScenarioProperty $Readback $name)
        $observed = [string](Get-WixInstallerScenarioProperty $Reboot $name)
        if ([string]::IsNullOrWhiteSpace($expected) -or $observed -cne $expected) {
            return $false
        }
    }
    if ([string](Get-WixInstallerScenarioProperty $Readback 'sentinel_sha256') -cnotmatch '^[0-9a-f]{64}$') {
        return $false
    }
    try {
        $sentinel = [IO.Path]::GetFullPath([string](Get-WixInstallerScenarioProperty $Readback 'sentinel_path'))
        if (-not $sentinel.StartsWith('C:\', [StringComparison]::OrdinalIgnoreCase) -or
            $sentinel -match '^C:\\(?:PolicyWork|PolicyOutput)(?:\\|$)') {
            return $false
        }
    }
    catch [ArgumentException] { return $false }
    catch [NotSupportedException] { return $false }
    catch [IO.IOException] { return $false }

    $exited = Get-WixInstallerScenarioProperty $Process 'exited' $null
    $exitText = [string](Get-WixInstallerScenarioProperty $Process 'exit_code')
    if ($exited -isnot [bool] -or -not $exited -or [string]::IsNullOrWhiteSpace($exitText)) {
        return $false
    }
    try {
        if ($exitText.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
            [void][Convert]::ToInt64($exitText.Substring(2), 16)
        }
        else {
            [void][Convert]::ToInt64($exitText, 10)
        }
    }
    catch [FormatException] { return $false }
    catch [OverflowException] { return $false }
    catch [ArgumentException] { return $false }

    $fields = @(
        @{ key = 'ended'; object = $Process; name = 'ended_utc' },
        @{ key = 'proof_ended'; object = $Reboot; name = 'process_ended_utc' },
        @{ key = 'before'; object = $Reboot; name = 'before_boot_utc' },
        @{ key = 'after'; object = $Reboot; name = 'after_boot_utc' },
        @{ key = 'observed'; object = $Reboot; name = 'observed_utc' }
    )
    if ($null -ne $NextProcess) {
        $fields += @{ key = 'next_started'; object = $NextProcess; name = 'started_utc' }
        $fields += @{ key = 'next_ended'; object = $NextProcess; name = 'ended_utc' }
    }
    $dates = @{}
    foreach ($field in $fields) {
        $value = [string](Get-WixInstallerScenarioProperty $field.object $field.name)
        $parsed = [DateTimeOffset]::MinValue
        if ($value -cnotmatch '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\.[0-9]{1,7})?(?:Z|[+-][0-9]{2}:[0-9]{2})$' -or
            -not [DateTimeOffset]::TryParse($value, [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::None, [ref]$parsed)) {
            return $false
        }
        $dates[$field.key] = $parsed.UtcDateTime
    }
    if ($dates.ended -ne $dates.proof_ended -or $dates.before -gt $dates.ended -or
        $dates.after -le $dates.ended -or $dates.observed -lt $dates.after) {
        return $false
    }
    if ($null -ne $NextProcess -and
        ($dates.next_started -lt $dates.after -or $dates.next_ended -lt $dates.next_started)) {
        return $false
    }
    return $true
}

function Get-WixInstallerScenarioVerdict {
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [ValidateSet('Upgrade', 'Reentry', 'Recovery')]
        [string]$Scenario,
        [Parameter(Mandatory = $true, Position = 1)]$Evidence,
        [switch]$AcceptRetainedBundle
    )

    $readback = Get-WixInstallerScenarioProperty $Evidence 'readback'
    $result = 'NOT_COVERED'
    $reason = 'Insufficient evidence.'
    $firstResultKnown = Test-WixInstallerScenarioProperty $Evidence 'first_apply_result'
    $firstExitKnown = Test-WixInstallerScenarioProperty $Evidence 'first_run_exit_code'
    $firstSucceeded = $firstResultKnown -and $firstExitKnown -and
        -not [string]::IsNullOrWhiteSpace([string](Get-WixInstallerScenarioProperty $Evidence 'first_apply_result')) -and
        -not [string]::IsNullOrWhiteSpace([string](Get-WixInstallerScenarioProperty $Evidence 'first_run_exit_code')) -and
        (Test-WixInstallerScenarioSuccessfulExit (Get-WixInstallerScenarioProperty $Evidence 'first_apply_result')) -and
        (Test-WixInstallerScenarioSuccessfulExit (Get-WixInstallerScenarioProperty $Evidence 'first_run_exit_code')) -and
        (Get-WixInstallerScenarioProperty $Evidence 'first_msi_commits' 0) -gt 0
    $firstProcess = Get-WixInstallerScenarioProperty $Evidence 'first_process'
    $firstProcessComplete = $null -eq $firstProcess -or
        (Get-WixInstallerScenarioProperty $firstProcess 'exited' $false) -eq $true

    if ($Scenario -ceq 'Upgrade') {
        $expectedRuntime = [string](Get-WixInstallerScenarioProperty $readback 'expected_runtime_version')
        $expectedMsi = [string](Get-WixInstallerScenarioProperty $readback 'expected_msi_version')
        $missingProof = $false
        $contradictedProof = $false
        foreach ($field in @(
            'observed_runtime_version',
            'observed_payload_version',
            'observed_com_version'
        )) {
            $actual = [string](Get-WixInstallerScenarioProperty $readback $field)
            if (-not (Test-WixInstallerScenarioProperty $readback $field) -or
                [string]::IsNullOrWhiteSpace($actual)) {
                $missingProof = $true
            }
            elseif ($actual -cne $expectedRuntime) {
                $contradictedProof = $true
            }
        }
        foreach ($entry in @(
            @{ actual = (Get-WixInstallerScenarioProperty $Evidence 'burn_version'); expected = $expectedMsi },
            @{ actual = (Get-WixInstallerScenarioProperty $Evidence 'msi_product_version'); expected = $expectedMsi }
        )) {
            $actual = [string]$entry.actual
            if ([string]::IsNullOrWhiteSpace($actual)) {
                $missingProof = $true
            }
            elseif ($actual -cne [string]$entry.expected) {
                $contradictedProof = $true
            }
        }
        foreach ($field in @(
            'manifest_integrity_ok',
            'registration_status_ok',
            'new_bundle_registered',
            'settings_preserved',
            'doctor_preserved',
            'old_bundle_and_msi_registrations_absent'
        )) {
            if (-not (Test-WixInstallerScenarioProperty $readback $field) -or
                $null -eq (Get-WixInstallerScenarioProperty $readback $field $null)) {
                $missingProof = $true
            }
            elseif ((Get-WixInstallerScenarioProperty $readback $field) -ne $true) {
                $contradictedProof = $true
            }
        }
        $firstFailed =
            ($firstResultKnown -and -not [string]::IsNullOrWhiteSpace([string](Get-WixInstallerScenarioProperty $Evidence 'first_apply_result')) -and
                -not (Test-WixInstallerScenarioSuccessfulExit (Get-WixInstallerScenarioProperty $Evidence 'first_apply_result'))) -or
            ($firstExitKnown -and -not [string]::IsNullOrWhiteSpace([string](Get-WixInstallerScenarioProperty $Evidence 'first_run_exit_code')) -and
                -not (Test-WixInstallerScenarioSuccessfulExit (Get-WixInstallerScenarioProperty $Evidence 'first_run_exit_code')))
        if ($firstFailed -or $contradictedProof) {
            $result = 'FAIL'
            $reason = 'The upgrade contradicted first-run or final-state evidence.'
        }
        elseif ($firstSucceeded -and $firstProcessComplete -and -not $missingProof) {
            $result = 'PASS'
            $reason = 'The first upgrade committed and final runtime, MSI, COM, and registration state agree.'
        }
    }
    elseif ($Scenario -ceq 'Reentry') {
        $retries = [int](Get-WixInstallerScenarioProperty $readback 'targeted_reentry_retries' 0)
        $secondStartText = [string](Get-WixInstallerScenarioProperty $Evidence 'second_invocation_start_utc')
        $secondEndText = [string](Get-WixInstallerScenarioProperty $Evidence 'second_invocation_end_utc')
        $applyStartText = [string](Get-WixInstallerScenarioProperty $Evidence 'first_apply_start_utc')
        $applyEndText = [string](Get-WixInstallerScenarioProperty $Evidence 'first_apply_end_utc')
        $overlap = $false
        if (-not [string]::IsNullOrWhiteSpace($secondStartText) -and
            -not [string]::IsNullOrWhiteSpace($secondEndText) -and
            -not [string]::IsNullOrWhiteSpace($applyStartText) -and
            -not [string]::IsNullOrWhiteSpace($applyEndText)) {
            try {
                $overlap =
                    [DateTimeOffset]::Parse($secondStartText).UtcDateTime -le
                        [DateTimeOffset]::Parse($applyEndText).UtcDateTime -and
                    [DateTimeOffset]::Parse($secondEndText).UtcDateTime -ge
                        [DateTimeOffset]::Parse($applyStartText).UtcDateTime
            }
            catch [FormatException] {
                $overlap = $false
            }
        }
        $secondMsiValue = Get-WixInstallerScenarioProperty $Evidence 'second_msi_transactions' $null
        $secondMsiKnown = (Test-WixInstallerScenarioProperty $Evidence 'second_msi_transactions') -and
            $null -ne $secondMsiValue
        $secondMsi = if ($secondMsiKnown) { [int]$secondMsiValue } else { $null }
        $secondApplyValue = Get-WixInstallerScenarioProperty $Evidence 'second_apply_begin_count' $null
        $secondApplyKnown = (Test-WixInstallerScenarioProperty $Evidence 'second_apply_begin_count') -and
            $null -ne $secondApplyValue
        $secondApplyCount = if ($secondApplyKnown) { [int]$secondApplyValue } else { $null }
        $secondSessionValue = Get-WixInstallerScenarioProperty $Evidence 'second_session_begin_count' $null
        $secondSessionKnown = (Test-WixInstallerScenarioProperty $Evidence 'second_session_begin_count') -and
            $null -ne $secondSessionValue
        $secondSessionCount = if ($secondSessionKnown) { [int]$secondSessionValue } else { $null }
        $secondLogCompleteValue =
            Get-WixInstallerScenarioProperty $Evidence 'second_log_complete' $null
        $secondLogCompleteKnown =
            (Test-WixInstallerScenarioProperty $Evidence 'second_log_complete') -and
            $secondLogCompleteValue -is [bool]
        $secondLogComplete = $secondLogCompleteKnown -and $secondLogCompleteValue
        $guardBusy =
            (Get-WixInstallerScenarioProperty $Evidence 'second_guard_busy_before_apply' $false) -eq $true
        $busyExitCodes = @('0x80070652', '2147944018', '-2147023278')
        $secondTerminalValue =
            Get-WixInstallerScenarioProperty $Evidence 'second_terminal_exit_code' $null
        $secondTerminalKnown =
            (Test-WixInstallerScenarioProperty $Evidence 'second_terminal_exit_code') -and
            -not [string]::IsNullOrWhiteSpace([string]$secondTerminalValue)
        $secondTerminalMatchesBusy = $secondTerminalKnown -and
            ($busyExitCodes -contains ([string]$secondTerminalValue))
        $terminalContradictsGuard = $guardBusy -and
            $secondLogComplete -and $secondTerminalKnown -and
            -not $secondTerminalMatchesBusy
        $unattributedMsi = [int](Get-WixInstallerScenarioProperty $Evidence 'unattributed_msi_transactions' 0)
        $burnMatches = [string](Get-WixInstallerScenarioProperty $readback 'expected_msi_version') -ceq
            [string](Get-WixInstallerScenarioProperty $Evidence 'burn_version')
        $secondProcess = Get-WixInstallerScenarioProperty $Evidence 'second_process'
        $secondProcessExitValue = $null
        $secondProcessExitKnown = $false
        $secondProcessExitMatchesBusy = $false
        $secondProcessExitContradicts = $false
        $secondProcessComplete = if ($null -ne $secondProcess) {
            $secondProcessExited =
                Get-WixInstallerScenarioProperty $secondProcess 'exited' $null
            $secondProcessExitedKnown =
                (Test-WixInstallerScenarioProperty $secondProcess 'exited') -and
                $secondProcessExited -is [bool] -and
                $secondProcessExited
            $secondProcessExitValue =
                Get-WixInstallerScenarioProperty $secondProcess 'exit_code' $null
            $secondProcessExitKnown =
                (Test-WixInstallerScenarioProperty $secondProcess 'exit_code') -and
                -not [string]::IsNullOrWhiteSpace([string]$secondProcessExitValue)
            $secondProcessExitMatchesBusy = $secondProcessExitKnown -and
                ($busyExitCodes -contains ([string]$secondProcessExitValue))
            $secondProcessExitContradicts =
                $secondProcessExitedKnown -and $secondProcessExitKnown -and
                -not $secondProcessExitMatchesBusy
            $secondProcessExitedKnown -and $secondProcessExitMatchesBusy
        }
        else {
            [string](Get-WixInstallerScenarioProperty $Evidence 'evidence_class') -ceq
                'synthetic_contract_only'
        }
        $firstFailed =
            ($firstResultKnown -and -not [string]::IsNullOrWhiteSpace([string](Get-WixInstallerScenarioProperty $Evidence 'first_apply_result')) -and
                -not (Test-WixInstallerScenarioSuccessfulExit (Get-WixInstallerScenarioProperty $Evidence 'first_apply_result'))) -or
            ($firstExitKnown -and -not [string]::IsNullOrWhiteSpace([string](Get-WixInstallerScenarioProperty $Evidence 'first_run_exit_code')) -and
                -not (Test-WixInstallerScenarioSuccessfulExit (Get-WixInstallerScenarioProperty $Evidence 'first_run_exit_code')))
        $secondProgressObserved =
            ($secondApplyKnown -and $secondApplyCount -gt 0) -or
            ($secondSessionKnown -and $secondSessionCount -gt 0)
        if ($retries -gt 1 -or
            ($secondMsiKnown -and $secondMsi -gt 0) -or
            $secondProgressObserved -or
            $terminalContradictsGuard -or
            $secondProcessExitContradicts -or
            $firstFailed) {
            $result = 'FAIL'
            $reason = 'A second Apply/Session/MSI transaction, failed first run, excess retry, or contradictory terminal/process exit was observed.'
        }
        else {
            $expectedRuntime = [string](Get-WixInstallerScenarioProperty $readback 'expected_runtime_version')
            $expectedMsi = [string](Get-WixInstallerScenarioProperty $readback 'expected_msi_version')
            $finalStateProof = $null
            if (-not [string]::IsNullOrWhiteSpace($expectedRuntime) -and
                -not [string]::IsNullOrWhiteSpace($expectedMsi)) {
                $finalStateProof = Get-WixInstallerScenarioVerdict -Scenario Upgrade -Evidence $Evidence
            }

            if ($null -ne $finalStateProof -and $finalStateProof.verdict -ceq 'FAIL') {
                $result = 'FAIL'
                $reason = 'The first upgrade contradicted complete final-state evidence.'
            }
            elseif ($firstSucceeded -and $firstProcessComplete -and $secondProcessComplete -and $overlap -and
                $guardBusy -and $secondLogCompleteKnown -and $secondLogComplete -and
                $secondTerminalKnown -and $secondTerminalMatchesBusy -and
                [string](Get-WixInstallerScenarioProperty $Evidence 'second_hresult') -ceq '0x80070652' -and
                $burnMatches -and
                $secondApplyKnown -and $secondApplyCount -eq 0 -and
                $secondSessionKnown -and $secondSessionCount -eq 0 -and
                $secondMsiKnown -and $secondMsi -eq 0 -and $unattributedMsi -eq 0 -and
                $null -ne $finalStateProof -and $finalStateProof.verdict -ceq 'PASS') {
                $result = 'PASS'
                $reason = 'The second invocation overlapped Apply, hit the exact early guard, completed without Apply/Session/MSI, and the first run has complete final state.'
            }
            else {
                $result = 'NOT_COVERED'
                $reason = 'No unambiguous overlapping early-guard rejection with complete second-log, process, and first-upgrade evidence was observed.'
            }
        }
    }
    elseif ($Scenario -ceq 'Recovery') {
        $attempts = [int](Get-WixInstallerScenarioProperty $readback 'injection_attempts' 0)
        $bundleConsistent = Get-WixInstallerScenarioProperty $readback 'bundle_consistent_at_interrupt' $null
        $verifiedStop = Get-WixInstallerScenarioProperty $readback 'verified_ba_stop' $null
        $engineExited = Get-WixInstallerScenarioProperty $readback 'original_engine_exited' $null
        $retainedAccepted = $AcceptRetainedBundle -and
            $bundleConsistent -is [bool] -and $bundleConsistent -and
            $verifiedStop -is [bool] -and $verifiedStop -and
            $engineExited -is [bool] -and $engineExited
        $formed = (
            (Get-WixInstallerScenarioProperty $Evidence 'first_msi_commits' 0) -gt 0 -and
            (Get-WixInstallerScenarioProperty $readback 'injection_origin') -ceq 'candidate_ba_ui' -and
            ($bundleConsistent -eq $false -or $retainedAccepted)
        )
        if ($attempts -gt 2) {
            $result = 'FAIL'
            $reason = 'Recovery injection exceeded the two-attempt bound.'
        }
        elseif (-not $formed) {
            $result = 'NOT_COVERED'
            $reason = 'A candidate-BA interruption after real MSI commit and bundle inconsistency was not formed.'
        }
        else {
            $missing = $false
            $contradicted = $false
            $processRestartRequired = $false
            if (Get-WixInstallerScenarioProperty $Evidence 'restart_required' $false) {
                $processRestartRequired = -not (Test-WixInstallerScenarioRecoveryRebootProof `
                    -Readback $readback `
                    -Process (Get-WixInstallerScenarioProperty $readback 'recovery_process') `
                    -Reboot (Get-WixInstallerScenarioProperty $readback 'recovery_reboot') `
                    -NextProcess (Get-WixInstallerScenarioProperty $readback 'normal_rerun_process'))
            }
            $expectedRuntime = [string](Get-WixInstallerScenarioProperty $readback 'expected_runtime_version')
            $expectedMsi = [string](Get-WixInstallerScenarioProperty $readback 'expected_msi_version')
            foreach ($field in @(
                'observed_runtime_version',
                'observed_payload_version',
                'observed_com_version'
            )) {
                $actual = [string](Get-WixInstallerScenarioProperty $readback $field)
                if (-not (Test-WixInstallerScenarioProperty $readback $field) -or
                    [string]::IsNullOrWhiteSpace($actual)) {
                    $missing = $true
                }
                elseif ($actual -cne $expectedRuntime) {
                    $contradicted = $true
                }
            }
            foreach ($field in @(
                'rerun_converged',
                'old_bundle_and_msi_registrations_absent',
                'uninstall_succeeded',
                'settings_preserved',
                'doctor_preserved',
                'manifest_integrity_ok',
                'registration_status_ok',
                'new_bundle_registered'
            )) {
                if (-not (Test-WixInstallerScenarioProperty $readback $field) -or
                    $null -eq (Get-WixInstallerScenarioProperty $readback $field $null)) {
                    $missing = $true
                }
                elseif ((Get-WixInstallerScenarioProperty $readback $field) -ne $true) {
                    $contradicted = $true
                }
            }
            foreach ($processField in @('normal_rerun_process', 'normal_uninstall_process')) {
                $process = Get-WixInstallerScenarioProperty $readback $processField $null
                $processExited = Get-WixInstallerScenarioProperty $process 'exited' $null
                $processExitText = [string](Get-WixInstallerScenarioProperty $process 'exit_code' '')
                if ([string]::IsNullOrWhiteSpace($processExitText)) {
                    $missing = $true
                    continue
                }
                $processExitValue = $null
                try {
                    $processExitValue = if ($processExitText.StartsWith(
                            '0x', [StringComparison]::OrdinalIgnoreCase)) {
                        [Convert]::ToInt64($processExitText.Substring(2), 16)
                    }
                    else {
                        [Convert]::ToInt64($processExitText, 10)
                    }
                }
                catch [FormatException] { }
                catch [OverflowException] { }
                catch [ArgumentException] { }
                if ($null -ne $process -and
                    (Test-WixInstallerScenarioProperty $readback $processField)) {
                    if ($processExited -isnot [bool] -or -not $processExited -or
                        $null -eq $processExitValue -or
                        -not (Test-WixInstallerScenarioProperty $process 'exit_code')) {
                        $missing = $true
                    }
                    elseif (-not (Test-WixInstallerScenarioSuccessfulExit $processExitText)) {
                        $contradicted = $true
                    }
                    elseif ($processExitValue -eq 3010) {
                        $rebootField = $processField -replace '_process$', '_reboot'
                        $nextProcess = if ($processField -ceq 'normal_rerun_process') {
                            Get-WixInstallerScenarioProperty $readback 'normal_uninstall_process'
                        } else { $null }
                        if (-not (Test-WixInstallerScenarioRecoveryRebootProof `
                            -Readback $readback -Process $process `
                            -Reboot (Get-WixInstallerScenarioProperty $readback $rebootField) `
                            -NextProcess $nextProcess)) {
                            $processRestartRequired = $true
                        }
                    }
                }
                else {
                    $missing = $true
                }
            }
            if (-not (Test-WixInstallerScenarioProperty $readback 'normal_ui_rerun_mode')) {
                $missing = $true
            }
            else {
                $rerunMode = [string](Get-WixInstallerScenarioProperty $readback 'normal_ui_rerun_mode')
                if ([string]::IsNullOrWhiteSpace($rerunMode)) {
                    $missing = $true
                }
                elseif ($rerunMode -cne 'normal_double_click') {
                    $contradicted = $true
                }
            }
            $argumentsPresent = Test-WixInstallerScenarioProperty $readback 'rerun_arguments'
            $arguments = @(Get-WixInstallerScenarioProperty $readback 'rerun_arguments' @())
            if ($argumentsPresent -and $arguments.Count -ne 0) {
                $contradicted = $true
            }
            elseif (-not $argumentsPresent) {
                $missing = $true
            }
            $burnVersion = [string](Get-WixInstallerScenarioProperty $Evidence 'burn_version')
            $msiVersion = [string](Get-WixInstallerScenarioProperty $Evidence 'msi_product_version')
            if ([string]::IsNullOrWhiteSpace($burnVersion) -or
                [string]::IsNullOrWhiteSpace($msiVersion)) {
                $missing = $true
            }
            elseif ($burnVersion -cne $expectedMsi -or $msiVersion -cne $expectedMsi) {
                $contradicted = $true
            }
            if ($contradicted) {
                $result = 'FAIL'
                $reason = 'The formed recovery state did not recover through one ordinary UI rerun and uninstall.'
            }
            elseif ($missing -or $processRestartRequired) {
                $result = 'NOT_COVERED'
                $reason = 'The formed recovery state lacks complete native-process proof or still requires a guest reboot.'
            }
            else {
                $result = 'PASS'
                $reason = if ($retainedAccepted) {
                    'A verified candidate-BA interruption after MSI commit retained the Full bundle and completed ordinary UI maintenance and uninstall while preserving user state.'
                } else {
                    'A real candidate-BA interruption after MSI commit repaired the inconsistent bundle through ordinary UI and preserved user state.'
                }
            }
        }
    }

    $evidenceClass = [string](Get-WixInstallerScenarioProperty $Evidence 'evidence_class')
    $synthetic = $evidenceClass -ceq 'synthetic_contract_only'
    $actualPass = -not $synthetic -and $evidenceClass -ceq 'runtime_observed' -and $result -ceq 'PASS'
    $verdict = [pscustomobject]@{
        scenario = $Scenario
        verdict = $result
        evidence_class = $evidenceClass
        receipt_status = if ($synthetic) {
            'SYNTHETIC_CONTRACT_ONLY'
        }
        elseif ($actualPass) {
            'ACTUAL_SCENARIO_PASS'
        }
        else {
            'RUNTIME_OBSERVED_' + $result
        }
        actual_scenario_pass = [bool]$actualPass
        reason = $reason
    }
    if ($Scenario -ceq 'Recovery') {
        $verdict | Add-Member -NotePropertyName acceptance_mode -NotePropertyValue $(
            if ($AcceptRetainedBundle) { 'retained_or_repaired' } else { 'repair_required' })
    }
    return $verdict
}

function Test-WixInstallerScenarioRebootContinuation {
    param(
        [Parameter(Mandatory = $true)]$OriginalReceipt,
        [Parameter(Mandatory = $true)]$PostRebootReadback,
        [Parameter(Mandatory = $true)][string]$GuestBootUtc,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$OwnedPendingDeletes
    )

    $scenario = [string](Get-WixInstallerScenarioProperty $OriginalReceipt 'scenario')
    $evidence = Get-WixInstallerScenarioProperty $OriginalReceipt 'evidence'
    $evidenceClass = [string](Get-WixInstallerScenarioProperty $evidence 'evidence_class')
    $result = [pscustomobject]@{
        scenario = $scenario
        verdict = 'NOT_COVERED'
        evidence_class = $evidenceClass
        receipt_status = 'RUNTIME_OBSERVED_NOT_COVERED'
        actual_scenario_pass = $false
        reason = 'Reboot continuation lacks complete, current evidence.'
    }
    if ($scenario -notin @('Upgrade', 'Reentry') -or $evidenceClass -cne 'runtime_observed' -or
        (Get-WixInstallerScenarioProperty $OriginalReceipt 'root_guest_reboot_required' $false) -ne $true -or
        (Get-WixInstallerScenarioProperty $OriginalReceipt 'unexpected_process_live' $null) -ne $false -or
        $null -eq $evidence -or $null -eq $PostRebootReadback) {
        return $result
    }

    $firstProcess = Get-WixInstallerScenarioProperty $evidence 'first_process'
    $firstExit = [string](Get-WixInstallerScenarioProperty $firstProcess 'exit_code' '')
    if ([string]::IsNullOrWhiteSpace($firstExit)) { return $result }
    $firstExited =
        (Get-WixInstallerScenarioProperty $firstProcess 'exited' $null) -eq $true
    $firstExitValue = $null
    try {
        $firstExitValue = if ($firstExit.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
            [Convert]::ToInt64($firstExit.Substring(2), 16)
        }
        else {
            [Convert]::ToInt64($firstExit, 10)
        }
    }
    catch [FormatException] { }
    catch [OverflowException] { }
    catch [ArgumentException] { }
    $firstExitKnown = $null -ne $firstExitValue
    $firstExit3010 = $firstExitKnown -and $firstExitValue -eq 3010 -and
        (Test-WixInstallerScenarioSuccessfulExit $firstExit)
    $applyEndText = [string](Get-WixInstallerScenarioProperty $evidence 'first_apply_end_utc')
    $bootAfterApply = $false
    try {
        $bootAfterApply =
            [DateTimeOffset]::Parse($GuestBootUtc).UtcDateTime -gt
            [DateTimeOffset]::Parse($applyEndText).UtcDateTime
    }
    catch [FormatException] { }
    catch [ArgumentException] { }
    if (-not $firstExited -or -not $firstExitKnown -or -not $bootAfterApply) {
        return $result
    }
    if (-not $firstExit3010) {
        $result.verdict = 'FAIL'
        $result.receipt_status = 'RUNTIME_OBSERVED_FAIL'
        $result.reason = 'The completed first process did not report native exit 3010.'
        return $result
    }

    $continuedEvidence = [ordered]@{}
    foreach ($property in $evidence.PSObject.Properties) {
        $continuedEvidence[$property.Name] = $property.Value
    }
    $continuedEvidence['readback'] = $PostRebootReadback
    $result = Get-WixInstallerScenarioVerdict `
        -Scenario $scenario -Evidence ([pscustomobject]$continuedEvidence)
    if ($result.verdict -ceq 'PASS' -and @($OwnedPendingDeletes).Count -gt 0) {
        $result.verdict = 'NOT_COVERED'
        $result.receipt_status = 'RUNTIME_OBSERVED_NOT_COVERED'
        $result.actual_scenario_pass = $false
        $result.reason = 'Owned pending deletes require another verified continuation.'
    }
    return $result
}

function Test-WixInstallerScenarioKillEligible {
    param(
        [Parameter(Mandatory = $true, Position = 0)]$ExpectedTarget,
        [Parameter(Mandatory = $true, Position = 1)]$ObservedProcess,
        [switch]$AllowElevatedBaUi
    )

    $required = @('path', 'sha256', 'pid', 'creation_time_utc')
    foreach ($name in $required) {
        if (-not (Test-WixInstallerScenarioProperty $ExpectedTarget $name) -or
            -not (Test-WixInstallerScenarioProperty $ObservedProcess $name)) {
            return [pscustomobject]@{ eligible = $false; reason = "Missing verified identity field: $name." }
        }
        $expected = Get-WixInstallerScenarioProperty $ExpectedTarget $name
        $observed = Get-WixInstallerScenarioProperty $ObservedProcess $name
        if ([string]::IsNullOrWhiteSpace([string]$expected) -or [string]::IsNullOrWhiteSpace([string]$observed)) {
            return [pscustomobject]@{ eligible = $false; reason = "Empty verified identity field: $name." }
        }
    }
    $kind = [string](Get-WixInstallerScenarioProperty $ObservedProcess 'process_kind')
    $elevatedBa = $AllowElevatedBaUi -and $kind -ceq 'elevated_burn_ba' -and
        (Get-WixInstallerScenarioProperty $ExpectedTarget 'process_kind') -ceq 'elevated_burn_ba'
    if ($elevatedBa) {
        foreach ($target in @($ExpectedTarget, $ObservedProcess)) {
            foreach ($rule in @(
                @{ name = 'elevated'; expected = $true },
                @{ name = 'alive'; expected = $true },
                @{ name = 'trusted_ba_hash'; expected = $true },
                @{ name = 'setup_hash_match'; expected = $false }
            )) {
                $value = Get-WixInstallerScenarioProperty $target $rule.name $null
                if ($value -isnot [bool] -or $value -ne $rule.expected) {
                    return [pscustomobject]@{ eligible = $false; reason = "Standalone BA proof is absent or contradicted: $($rule.name)." }
                }
            }
        }
        foreach ($name in @('parent_pid', 'root_pid', 'root_creation_time_utc')) {
            $expected = [string](Get-WixInstallerScenarioProperty $ExpectedTarget $name)
            $observed = [string](Get-WixInstallerScenarioProperty $ObservedProcess $name)
            if ([string]::IsNullOrWhiteSpace($expected) -or $observed -cne $expected) {
                return [pscustomobject]@{ eligible = $false; reason = "Standalone BA ancestry changed: $name." }
            }
        }
        $expectedAncestry = @(Get-WixInstallerScenarioProperty $ExpectedTarget 'ancestry_pids' @())
        $observedAncestry = @(Get-WixInstallerScenarioProperty $ObservedProcess 'ancestry_pids' @())
        if ($expectedAncestry.Count -lt 2 -or
            ($expectedAncestry -join ',') -cne ($observedAncestry -join ',') -or
            [string]$expectedAncestry[0] -cne [string]$ExpectedTarget.root_pid -or
            [string]$expectedAncestry[-1] -cne [string]$ExpectedTarget.pid -or
            [string]$expectedAncestry[-2] -cne [string]$ExpectedTarget.parent_pid -or
            [string]$ExpectedTarget.root_pid -ceq [string]$ExpectedTarget.pid) {
            return [pscustomobject]@{ eligible = $false; reason = 'Standalone BA ancestry is incomplete or changed.' }
        }
    }
    elseif ($kind -cne 'candidate_ba_ui') {
        return [pscustomobject]@{
            eligible = $false
            reason = 'Only an explicitly classified candidate BA/UI process is eligible.'
        }
    }
    if (-not $elevatedBa -and (Get-WixInstallerScenarioProperty $ObservedProcess 'elevated' $null) -ne $false) {
        return [pscustomobject]@{ eligible = $false; reason = 'The BA/UI process is elevated or elevation is unknown.' }
    }
    $observedPath = [string](Get-WixInstallerScenarioProperty $ObservedProcess 'path')
    $absolutePath = $false
    try {
        $absolutePath = [IO.Path]::IsPathRooted($observedPath) -and
            ($observedPath -match '^[A-Za-z]:\\' -or $observedPath.StartsWith('\\')) -and
            -not [string]::IsNullOrWhiteSpace([IO.Path]::GetFullPath($observedPath))
    }
    catch [ArgumentException] {
        $absolutePath = $false
    }
    if (-not $absolutePath -or
        $observedPath -cne
            [string](Get-WixInstallerScenarioProperty $ExpectedTarget 'path')) {
        return [pscustomobject]@{ eligible = $false; reason = 'Candidate path changed.' }
    }
    if ([string](Get-WixInstallerScenarioProperty $ObservedProcess 'sha256') -cne
        [string](Get-WixInstallerScenarioProperty $ExpectedTarget 'sha256')) {
        return [pscustomobject]@{ eligible = $false; reason = 'Candidate SHA-256 changed.' }
    }
    if ([int](Get-WixInstallerScenarioProperty $ObservedProcess 'pid') -ne
        [int](Get-WixInstallerScenarioProperty $ExpectedTarget 'pid')) {
        return [pscustomobject]@{ eligible = $false; reason = 'Candidate PID changed.' }
    }
    if ([string](Get-WixInstallerScenarioProperty $ObservedProcess 'creation_time_utc') -cne
        [string](Get-WixInstallerScenarioProperty $ExpectedTarget 'creation_time_utc')) {
        return [pscustomobject]@{ eligible = $false; reason = 'Candidate creation time changed.' }
    }
    return [pscustomobject]@{
        eligible = $true
        reason = if ($elevatedBa) { 'Exact trusted standalone BA/UI identity with explicit opt-in.' } else { 'Exact verified, non-elevated candidate BA/UI identity.' }
    }
}

if ($ContractOnly) {
    return
}

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-WixInstallerScenarioUtf8 {
    return New-Object Text.UTF8Encoding($false)
}

function Read-WixInstallerScenarioSharedLog {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
    try { return $reader.ReadToEnd() }
    finally { $reader.Dispose() }
}

function Get-WixInstallerScenarioJson {
    param([Parameter(Mandatory = $true)]$Value)

    return $Value | ConvertTo-Json -Depth 32
}

function Write-WixInstallerScenarioJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "[SC030] Output parent does not exist for $Path."
    }
    $temporary = Join-Path $parent ('.' + (Split-Path -Leaf $Path) + '.' + [Guid]::NewGuid().ToString('N') + '.tmp')
    [IO.File]::WriteAllText(
        $temporary,
        (Get-WixInstallerScenarioJson $Value),
        (Get-WixInstallerScenarioUtf8)
    )
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        # PS5.1 binds plain $null to an empty string for this string argument.
        [IO.File]::Replace($temporary, $Path, [System.Management.Automation.Language.NullString]::Value)
    }
    else {
        [IO.File]::Move($temporary, $Path)
    }
}

function Write-WixInstallerScenarioProgress {
    param(
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [Parameter(Mandatory = $true)][string]$Phase,
        [Parameter(Mandatory = $true)][string]$Status,
        $Detail = $null
    )

    Write-WixInstallerScenarioJson (Join-Path $OutputDir 'state.json') ([ordered]@{
        format = 'liukai-installer-scenario-state-v1'
        updated_utc = [DateTime]::UtcNow.ToString('o')
        phase = $Phase
        status = $Status
        detail = $Detail
    })
}

function Get-WixInstallerScenarioManifestData {
    param([Parameter(Mandatory = $true)][string]$Path)

    try {
        return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 -ErrorAction Stop |
            ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "[SC011] Cannot parse input manifest: $($_.Exception.Message)"
    }
}

function Get-WixInstallerScenarioGuestObservation {
    param([Parameter(Mandatory = $true)]$Manifest)

    $owned = Get-WixInstallerScenarioProperty $Manifest 'owned_instance'
    $sentinelPath = [string](Get-WixInstallerScenarioProperty $owned 'sentinel_path')
    $expectedSentinelHash = [string](Get-WixInstallerScenarioProperty $owned 'sentinel_sha256')
    $expectedSandboxId = [string](Get-WixInstallerScenarioProperty $owned 'sandbox_id')
    $authorizationKind = [string](Get-WixInstallerScenarioProperty $owned 'authorization_kind')

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $accountName = $identity.Name
    $identity.Dispose()
    $base = $null
    $key = $null
    $registrySandboxId = ''
    try {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::LocalMachine,
            [Microsoft.Win32.RegistryView]::Registry64)
        $key = $base.OpenSubKey('SOFTWARE\LiuKaiPlanTest', $false)
        if ($null -ne $key) {
            $registrySandboxId = [string]$key.GetValue('SandboxId', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        }
    }
    finally {
        if ($null -ne $key) {
            $key.Dispose()
        }
        if ($null -ne $base) {
            $base.Dispose()
        }
    }

    $sentinelPresent = $false
    $sentinelHash = ''
    $sentinelLocal = $false
    $fullSentinel = ''
    if (-not [string]::IsNullOrWhiteSpace($sentinelPath)) {
        try {
            $fullSentinel = [IO.Path]::GetFullPath($sentinelPath)
            $sentinelLocal = $fullSentinel.StartsWith('C:\', [StringComparison]::OrdinalIgnoreCase) -and
                -not $fullSentinel.StartsWith('C:\PolicyOutput\', [StringComparison]::OrdinalIgnoreCase) -and
                -not $fullSentinel.StartsWith('C:\PolicyWork\', [StringComparison]::OrdinalIgnoreCase)
            $sentinelPresent = Test-Path -LiteralPath $fullSentinel -PathType Leaf
            if ($sentinelPresent) {
                $item = Get-Item -LiteralPath $fullSentinel -ErrorAction Stop
                $sentinelPresent = ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0
                if ($sentinelPresent) {
                    $sentinelHash = (Get-FileHash -LiteralPath $fullSentinel -Algorithm SHA256).Hash.ToLowerInvariant()
                }
            }
        }
        catch [IO.IOException] {
            $sentinelPresent = $false
            $sentinelHash = ''
        }
        catch [UnauthorizedAccessException] {
            $sentinelPresent = $false
            $sentinelHash = ''
        }
    }

    $accountIsGuest = $accountName.Split('\')[-1] -ceq 'WDAGUtilityAccount'
    return [pscustomobject]@{
        authorization_kind = $authorizationKind
        sandbox_id = $registrySandboxId
        expected_sandbox_id = $expectedSandboxId
        is_windows_sandbox_guest = $accountIsGuest -and
            $sentinelLocal -and
            $registrySandboxId -cne '' -and
            $registrySandboxId -ceq $expectedSandboxId
        sentinel_present = $sentinelPresent
        sentinel_path = $fullSentinel
        sentinel_sha256 = $sentinelHash
        account_name = $accountName
        expected_sentinel_sha256 = $expectedSentinelHash
    }
}

function Test-WixInstallerScenarioRuntimeElevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $principal = New-Object Security.Principal.WindowsPrincipal($identity)
        return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
    finally {
        $identity.Dispose()
    }
}

function Invoke-WixInstallerScenarioNativeCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$WorkDir
    )

    $id = [Guid]::NewGuid().ToString('N')
    $stdout = Join-Path $WorkDir ('native-' + $id + '.stdout.txt')
    $stderr = Join-Path $WorkDir ('native-' + $id + '.stderr.txt')
    $process = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList `
        -WorkingDirectory $WorkDir -WindowStyle Hidden -PassThru -Wait `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $exitCode = $process.ExitCode
    $process.Dispose()
    $stdoutText = if (Test-Path -LiteralPath $stdout -PathType Leaf) {
        [IO.File]::ReadAllText($stdout, [Text.Encoding]::UTF8)
    }
    else {
        ''
    }
    $stderrText = if (Test-Path -LiteralPath $stderr -PathType Leaf) {
        [IO.File]::ReadAllText($stderr, [Text.Encoding]::UTF8)
    }
    else {
        ''
    }
    return [pscustomobject]@{
        exit_code = $exitCode
        stdout = [string]$stdoutText
        stderr = [string]$stderrText
        stdout_path = $stdout
        stderr_path = $stderr
    }
}

function Resolve-WixInstallerScenarioFixtures {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [AllowEmptyString()][string]$ToolchainBin
    )

    $mappedSource = Join-Path $PSScriptRoot 'mapped_module_fixture.cpp'
    $stateSource = Join-Path $PSScriptRoot 'wix-installer-state-fixture.cpp'
    $fixtureBin = Join-Path $OutputDir 'fixtures'
    [IO.Directory]::CreateDirectory($fixtureBin) | Out-Null
    $mappedExe = Join-Path $fixtureBin 'mapped-module-fixture.exe'
    $stateExe = Join-Path $fixtureBin 'wix-installer-state-fixture.exe'

    if (-not [string]::IsNullOrWhiteSpace($ToolchainBin)) {
        $bin = (Resolve-Path -LiteralPath $ToolchainBin -ErrorAction Stop).Path
        $compiler = Join-Path $bin 'g++.exe'
        if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
            throw '[SC031] ToolchainBin must contain g++.exe.'
        }
        $mapped = Invoke-WixInstallerScenarioNativeCapture -FilePath $compiler -WorkDir $fixtureBin `
            -ArgumentList @(
                '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DNTDDI_VERSION=0x0A000000',
                '-std=c++17', '-O2', '-static', '-municode', $mappedSource, '-o', $mappedExe
            )
        if ($mapped.exit_code -ne 0) {
            throw "[SC031] Cannot compile mapped-module fixture: $($mapped.stderr)"
        }
        $state = Invoke-WixInstallerScenarioNativeCapture -FilePath $compiler -WorkDir $fixtureBin `
            -ArgumentList @(
                '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DNTDDI_VERSION=0x0A000000',
                '-std=c++17', '-O2', '-static', '-municode', $stateSource,
                '-lole32', '-loleaut32', '-luuid', '-lgdi32', '-ladvapi32', '-lbcrypt', '-lshlwapi',
                '-o', $stateExe
            )
        if ($state.exit_code -ne 0) {
            throw "[SC031] Cannot compile state fixture: $($state.stderr)"
        }
    }
    else {
        $fixtures = Get-WixInstallerScenarioProperty $Manifest 'fixtures'
        $mappedExe = [string](Get-WixInstallerScenarioProperty $fixtures 'mapped_module_exe')
        $stateExe = [string](Get-WixInstallerScenarioProperty $fixtures 'state_fixture_exe')
        $expectedMapped = [string](Get-WixInstallerScenarioProperty $fixtures 'mapped_module_exe_sha256')
        $expectedState = [string](Get-WixInstallerScenarioProperty $fixtures 'state_fixture_exe_sha256')
        foreach ($entry in @(
            @{ path = $mappedExe; hash = $expectedMapped; label = 'mapped-module' },
            @{ path = $stateExe; hash = $expectedState; label = 'state' }
        )) {
            if (-not (Test-Path -LiteralPath $entry.path -PathType Leaf) -or
                $entry.hash -cnotmatch '^[0-9a-f]{64}$' -or
                (Get-FileHash -LiteralPath $entry.path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.hash) {
                throw "[SC031] The manifest-supplied $($entry.label) fixture is missing or hash-mismatched."
            }
        }
    }
    return [pscustomobject]@{
        mapped_module_exe = (Resolve-Path -LiteralPath $mappedExe).Path
        state_fixture_exe = (Resolve-Path -LiteralPath $stateExe).Path
        mapped_sha256 = (Get-FileHash -LiteralPath $mappedExe -Algorithm SHA256).Hash.ToLowerInvariant()
        state_sha256 = (Get-FileHash -LiteralPath $stateExe -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Initialize-WixInstallerScenarioState {
    param(
        [Parameter(Mandatory = $true)][string]$FixtureExe,
        [Parameter(Mandatory = $true)][string]$OutputDir
    )

    $marker = Join-Path (Join-Path $env:LOCALAPPDATA 'LiuKai') 'installer-scenario-fixture-v1.json'
    $operation = if (Test-Path -LiteralPath $marker -PathType Leaf) { '--read' } else { '--seed' }
    $result = Invoke-WixInstallerScenarioNativeCapture -FilePath $FixtureExe `
        -ArgumentList @($operation) -WorkDir $OutputDir
    if ($result.exit_code -ne 0) {
        throw "[SC032] Synthetic settings/Doctor state $operation failed: $($result.stderr.Trim())"
    }
    try {
        $state = $result.stdout | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "[SC032] Synthetic state fixture returned invalid readback: $($_.Exception.Message)"
    }
    if ($state.synthetic_fixture -ne $true -or $state.settings_present -ne $true -or
        $state.settings_valid -ne $true -or
        $state.doctor_present -ne $true -or $state.doctor_enabled -ne $false -or
        $state.doctor_record_count -ne 1) {
        throw '[SC032] Synthetic settings/Doctor state did not meet the OFF/one-record contract.'
    }
    return $state
}

function Get-WixInstallerScenarioStateRetention {
    param(
        [Parameter(Mandatory = $true)][string]$FixtureExe,
        [Parameter(Mandatory = $true)][string]$OutputDir
    )

    $result = Invoke-WixInstallerScenarioNativeCapture -FilePath $FixtureExe `
        -ArgumentList @('--read') -WorkDir $OutputDir
    if ($result.exit_code -ne 0) {
        throw "[SC032] Synthetic state readback failed: $($result.stderr.Trim())"
    }
    $state = $result.stdout | ConvertFrom-Json -ErrorAction Stop
    $userRoot = Join-Path $env:LOCALAPPDATA 'LiuKai'
    $paths = @(
        (Join-Path $userRoot 'settings.conf'),
        (Join-Path $userRoot 'installer-scenario-fixture-v1.json')
    )
    $doctorRoot = Join-Path $userRoot 'doctor'
    if (Test-Path -LiteralPath $doctorRoot -PathType Container) {
        $paths += @(
            Get-ChildItem -LiteralPath $doctorRoot -File -Recurse |
                ForEach-Object { $_.FullName }
        )
    }
    $files = @()
    foreach ($path in @($paths | Sort-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "[SC032] Synthetic retention file disappeared: $path"
        }
        $item = Get-Item -LiteralPath $path
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "[SC032] Synthetic retention file became a reparse point: $path"
        }
        $files += [pscustomobject]@{
            relative_path = $path.Substring($userRoot.Length).TrimStart('\').Replace('\', '/')
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            bytes = $item.Length
        }
    }
    return [pscustomobject]@{
        fixture = $state
        files = @($files | Sort-Object relative_path)
        fingerprint = (
            @($files | Sort-Object relative_path |
                ForEach-Object { "$($_.relative_path)=$($_.sha256):$($_.bytes)" }
            ) -join "`n"
        )
    }
}

function Get-WixInstallerScenarioProductRoot {
    $programs = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
    return Join-Path $programs 'LiuKai'
}

function Get-WixInstallerScenarioMsiProducts {
    $installer = $null
    $related = $null
    $products = @()
    $issues = @()
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $upgradeCode = '{7E8341DC-CE5A-4A3A-A6BC-2C5E70FEFC39}'
        $related = $installer.GetType().InvokeMember(
            'RelatedProducts', [Reflection.BindingFlags]::GetProperty,
            $null, $installer, @($upgradeCode))
        foreach ($productCode in $related) {
            try {
                $values = @{}
                foreach ($property in @('ProductName', 'VersionString', 'InstallLocation')) {
                    $values[$property] = [string]$installer.GetType().InvokeMember(
                        'ProductInfo', [Reflection.BindingFlags]::GetProperty,
                        $null, $installer, @([string]$productCode, $property))
                }
                $products += [pscustomobject]@{
                    product_code = [string]$productCode
                    product_name = $values.ProductName
                    version = $values.VersionString
                    install_location = $values.InstallLocation
                    upgrade_code = $upgradeCode
                }
            }
            catch {
                $issues += "MSI product $productCode could not be read: $($_.Exception.Message)"
            }
        }
    }
    catch {
        $issues += "Windows Installer product enumeration failed: $($_.Exception.Message)"
    }
    finally {
        if ($null -ne $related) {
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($related)
        }
        if ($null -ne $installer) {
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
        }
    }
    return [pscustomobject]@{
        products = @($products)
        issues = @($issues)
    }
}

function Get-WixInstallerScenarioUninstallEntries {
    $entries = @()
    $issues = @()
    $views = @(
        @{ hive = [Microsoft.Win32.RegistryHive]::LocalMachine; name = 'HKLM'; views = @(
            [Microsoft.Win32.RegistryView]::Registry64,
            [Microsoft.Win32.RegistryView]::Registry32
        ) },
        @{ hive = [Microsoft.Win32.RegistryHive]::CurrentUser; name = 'HKCU'; views = @(
            [Microsoft.Win32.RegistryView]::Registry64,
            [Microsoft.Win32.RegistryView]::Registry32
        ) }
    )
    foreach ($hive in $views) {
        foreach ($view in $hive.views) {
            $base = $null
            $root = $null
            try {
                $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive.hive, $view)
                $root = $base.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', $false)
                if ($null -eq $root) {
                    continue
                }
                foreach ($keyName in @($root.GetSubKeyNames())) {
                    $key = $root.OpenSubKey($keyName, $false)
                    if ($null -eq $key) {
                        continue
                    }
                    try {
                        $displayName = [string]$key.GetValue('DisplayName', '')
                        $publisher = [string]$key.GetValue('Publisher', '')
                        $owner = [string]$key.GetValue('WuGaiOwner', '')
                        $bundleUpgrade = [string]$key.GetValue('BundleUpgradeCode', '')
                        $windowsInstaller = $key.GetValue('WindowsInstaller', 0)
                        $relevant =
                            $owner -ceq 'LiuKai-TSF-v1' -or
                            $publisher -match '(?i)LiuKai' -or
                            $displayName -match '(?i)LiuKai|WuGai|嘸' -or
                            $bundleUpgrade -match '94C1F608-8C0F-424C-A572-E0F4BCB10327'
                        if (-not $relevant) {
                            continue
                        }
                        $values = [ordered]@{}
                        foreach ($name in @($key.GetValueNames())) {
                            $value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                            $values[$name] = $value
                        }
                        $entries += [pscustomobject]@{
                            hive = $hive.name
                            view = $view.ToString()
                            key_name = $keyName
                            kind = if (-not [string]::IsNullOrWhiteSpace($bundleUpgrade)) {
                                'burn_bundle'
                            }
                            elseif ([int]$windowsInstaller -eq 1) {
                                'msi_product'
                            }
                            else {
                                'product'
                            }
                            display_name = $displayName
                            display_version = [string]$key.GetValue('DisplayVersion', '')
                            install_location = [string]$key.GetValue('InstallLocation', '')
                            uninstall_string = [string]$key.GetValue('UninstallString', '')
                            values = [pscustomobject]$values
                        }
                    }
                    finally {
                        $key.Dispose()
                    }
                }
            }
            catch {
                $issues += "$($hive.name)/$view uninstall registry could not be read: $($_.Exception.Message)"
            }
            finally {
                if ($null -ne $root) {
                    $root.Dispose()
                }
                if ($null -ne $base) {
                    $base.Dispose()
                }
            }
        }
    }
    return [pscustomobject]@{
        entries = @($entries)
        issues = @($issues)
    }
}

function Get-WixInstallerScenarioComServer {
    param(
        [Parameter(Mandatory = $true)][string]$ExpectedDll,
        [Parameter(Mandatory = $true)]$Identity
    )

    $base = $null
    $classKey = $null
    $serverKey = $null
    try {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::LocalMachine,
            [Microsoft.Win32.RegistryView]::Registry64)
        $classKey = $base.OpenSubKey(
            'SOFTWARE\Classes\CLSID\{5D7C4291-2B34-4CE8-8A11-39627105C42E}',
            $false)
        if ($null -eq $classKey) {
            return [pscustomobject]@{
                observed = $true
                present = $false
                matches = $false
                path = $null
                sha256 = $null
                issue = 'Machine COM CLSID is absent.'
            }
        }
        $serverKey = $classKey.OpenSubKey('InprocServer32', $false)
        if ($null -eq $serverKey) {
            return [pscustomobject]@{
                observed = $true
                present = $false
                matches = $false
                path = $null
                sha256 = $null
                issue = 'Machine COM InprocServer32 key is absent.'
            }
        }
        $path = [string]$serverKey.GetValue(
            '',
            '',
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $threadingModel = [string]$serverKey.GetValue('ThreadingModel', '')
        $owner = [string]$classKey.GetValue('LiuKaiOwner', '')
        $sid = [string]$classKey.GetValue('LiuKaiInstallSid', '')
        $installRoot = [string]$classKey.GetValue('LiuKaiInstallRoot', '')
        $expectedPath = (Resolve-Path -LiteralPath $ExpectedDll -ErrorAction Stop).Path
        $present = Test-Path -LiteralPath $path -PathType Leaf
        $hash = if ($present) {
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        else {
            $null
        }
        $expectedHash = (Get-FileHash -LiteralPath $expectedPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $matches =
            $present -and
            $path -ieq $expectedPath -and
            $hash -ceq $expectedHash -and
            $threadingModel -ceq 'Apartment' -and
            $owner -ceq 'LiuKai-TSF-v1' -and
            $sid -ceq [string](Get-WixInstallerScenarioProperty $Identity 'Sid') -and
            $installRoot -ieq (Get-WixInstallerScenarioProductRoot)
        return [pscustomobject]@{
            observed = $true
            present = $true
            matches = [bool]$matches
            path = $path
            sha256 = $hash
            expected_path = $expectedPath
            expected_sha256 = $expectedHash
            issue = if ($matches) { $null } else { 'Machine COM identity/path/hash does not match the installed candidate DLL.' }
        }
    }
    catch {
        return [pscustomobject]@{
            observed = $false
            present = $null
            matches = $null
            path = $null
            sha256 = $null
            issue = "Machine COM readback failed: $($_.Exception.Message)"
        }
    }
    finally {
        if ($null -ne $serverKey) {
            $serverKey.Dispose()
        }
        if ($null -ne $classKey) {
            $classKey.Dispose()
        }
        if ($null -ne $base) {
            $base.Dispose()
        }
    }
}

function Get-WixInstallerScenarioSnapshot {
    param(
        [Parameter(Mandatory = $true)][string]$StateFixture,
        [Parameter(Mandatory = $true)][string]$OutputDir
    )

    function Test-WixInstallerScenarioOwnedCleanupPath {
        param(
            [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Path,
            [Parameter(Mandatory = $true)][string]$ProductRoot,
            [switch]$RetiredOnly
        )

        if ([string]::IsNullOrEmpty($Path)) { return $false }
        $candidate = $Path.TrimStart('!')
        foreach ($prefix in @('\??\', '\\?\')) {
            if ($candidate.StartsWith($prefix, [StringComparison]::Ordinal)) {
                $candidate = $candidate.Substring($prefix.Length)
                break
            }
        }
        try {
            if (-not [IO.Path]::IsPathRooted($candidate)) { return $false }
            $candidate = [IO.Path]::GetFullPath($candidate)
            $productPath = [IO.Path]::GetFullPath($ProductRoot).TrimEnd('\')
            if (-not $RetiredOnly -and
                ($candidate.Equals($productPath, [StringComparison]::OrdinalIgnoreCase) -or
                 $candidate.StartsWith($productPath + '\', [StringComparison]::OrdinalIgnoreCase))) {
                return $true
            }
            $parent = [IO.Path]::GetDirectoryName($productPath)
            return [string]::Equals([IO.Path]::GetDirectoryName($candidate), $parent,
                [StringComparison]::OrdinalIgnoreCase) -and
                [IO.Path]::GetFileName($candidate) -match '^\.liukai-retired-[0-9a-f]{32}-(tsf|branding)\.dll$'
        }
        catch { return $false }
    }

    $root = (Get-WixInstallerScenarioProductRoot)
    $issues = @()
    $owner = $null
    $installState = $null
    $runtimeVersion = $null
    $payloadFiles = @()
    $manifestObject = $null
    $manifestPath = $null
    $manifestSha256 = $null
    $expectedPayloadCount = $null
    $manifestIntegrityOk = $null
    $registration = $null
    $registrationStatusOk = $null
    $registrationAbsent = $null
    $productDirectoryAbsent = $null
    $ownedPendingDeletes = $null
    $ownedRetiredFiles = $null
    $msiIdentity = $null
    $identityIntegrityOk = $null
    $identityBase = $null
    $identityKey = $null
    try {
        $identityBase = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::LocalMachine,
            [Microsoft.Win32.RegistryView]::Registry64)
        $identityKey = $identityBase.OpenSubKey(
            'SOFTWARE\LiuKai\MSI\{7E8341DC-CE5A-4A3A-A6BC-2C5E70FEFC39}',
            $false)
        if ($null -ne $identityKey) {
            $values = [ordered]@{}
            foreach ($name in @($identityKey.GetValueNames())) {
                $values[$name] = $identityKey.GetValue(
                    $name,
                    $null,
                    [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            }
            $msiIdentity = [pscustomobject]$values
        }
    }
    finally {
        if ($null -ne $identityKey) {
            $identityKey.Dispose()
        }
        if ($null -ne $identityBase) {
            $identityBase.Dispose()
        }
    }

    $installed = $null -ne $msiIdentity -and (Test-Path -LiteralPath $root -PathType Container)
    if ($installed) {
        $owner = [string](Get-WixInstallerScenarioProperty $msiIdentity 'Owner')
        $runtimeVersion = [string](Get-WixInstallerScenarioProperty $msiIdentity 'Version')
        $identitySid = [string](Get-WixInstallerScenarioProperty $msiIdentity 'Sid')
        $identityToken = [string](Get-WixInstallerScenarioProperty $msiIdentity 'Token')
        $identityManifestSha256 = [string](Get-WixInstallerScenarioProperty $msiIdentity 'ManifestSha256')
        $identitySafeRetire = [string](Get-WixInstallerScenarioProperty $msiIdentity 'SafeRetire')
        $identityDoctorLifecycle = [string](Get-WixInstallerScenarioProperty $msiIdentity 'DoctorLifecycle')
        $identityIntegrityOk =
            $owner -ceq 'LiuKai-TSF-v1' -and
            -not [string]::IsNullOrWhiteSpace($runtimeVersion) -and
            -not [string]::IsNullOrWhiteSpace($identitySid) -and
            -not [string]::IsNullOrWhiteSpace($identityToken) -and
            $identityManifestSha256 -cmatch '^[0-9a-f]{64}$' -and
            $identitySafeRetire -ceq '1' -and
            $identityDoctorLifecycle -ceq '1'
        if (-not $identityIntegrityOk) {
            $issues += 'MSI installation identity values are incomplete or inconsistent.'
        }
        $manifestPath = Join-Path $root (
            'versions\' + $runtimeVersion + '\package-manifest.json'
        )
        if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
            try {
                $manifestSha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
                $manifestObject = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 |
                    ConvertFrom-Json -ErrorAction Stop
                $expectedPayloadCount = @($manifestObject.files).Count
                $manifestIntegrityOk = $identityIntegrityOk -and $expectedPayloadCount -gt 0
                if ([string]$manifestObject.version -cne $runtimeVersion) {
                    $manifestIntegrityOk = $false
                    $issues += 'Installed package manifest runtime version differs from MSI identity.'
                }
                if ($manifestSha256 -cne $identityManifestSha256) {
                    $manifestIntegrityOk = $false
                    $issues += 'Installed package manifest hash differs from MSI identity.'
                }
                $seenPaths = @{}
                foreach ($row in @($manifestObject.files)) {
                    $relative = [string]$row.path
                    if ($relative -notmatch '^[A-Za-z0-9._/-]+$' -or
                        $relative.Contains('..') -or $seenPaths.ContainsKey($relative)) {
                        $manifestIntegrityOk = $false
                        $issues += "Unsafe or duplicate manifest path: $relative"
                        continue
                    }
                    $seenPaths[$relative] = $true
                    $path = Join-Path $root $relative.Replace('/', '\')
                    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                        $manifestIntegrityOk = $false
                        $issues += "Manifest payload is absent: $relative"
                        continue
                    }
                    $item = Get-Item -LiteralPath $path
                    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
                    $hashMatches = $actualHash -ceq [string]$row.sha256
                    $sizeMatches = $item.Length -eq [int64]$row.bytes
                    if (-not $hashMatches -or -not $sizeMatches) {
                        $manifestIntegrityOk = $false
                        $issues += "Manifest payload hash/size mismatch: $relative"
                    }
                    $payloadFiles += [pscustomobject]@{
                        path = $relative
                        expected_sha256 = [string]$row.sha256
                        sha256 = $actualHash
                        bytes = $item.Length
                        expected_bytes = [int64]$row.bytes
                    }
                }
            }
            catch {
                $manifestIntegrityOk = $false
                $issues += "Installed package manifest read failed: $($_.Exception.Message)"
            }
        }
        else {
            $issues += 'Installed package manifest is absent.'
        }

        $register = Join-Path $root 'LiuKaiRegister.exe'
        $dll = Join-Path $root (
            'versions\' + $runtimeVersion + '\x64\BoshiamyTsf.dll'
        )
        if ((Test-Path -LiteralPath $dll -PathType Leaf) -and
            (Test-Path -LiteralPath $register -PathType Leaf)) {
            # Keep a verified, read-only observer outside the product tree so
            # the same runtime snapshot can query TSF after ordinary uninstall.
            if ($manifestIntegrityOk -eq $true) {
                $observer = Join-Path $OutputDir 'registration-status.exe'
                $observerHashFile = $observer + '.sha256'
                try {
                    if (-not (Test-Path -LiteralPath $observer)) {
                        $observerHash = (Get-FileHash -LiteralPath $register -Algorithm SHA256).Hash.ToLowerInvariant()
                        [IO.File]::Copy($register, $observer, $false)
                        [IO.File]::WriteAllText($observerHashFile, $observerHash)
                    }
                    if (-not (Test-Path -LiteralPath $observerHashFile -PathType Leaf) -or
                        (Get-FileHash -LiteralPath $observer -Algorithm SHA256).Hash.ToLowerInvariant() -cne
                            [IO.File]::ReadAllText($observerHashFile).Trim()) {
                        throw 'Cached registration observer hash is not verified.'
                    }
                }
                catch {
                    $issues += "Registration observer cache failed: $($_.Exception.Message)"
                }
            }
            $registrationText = ''
            $registrationExit = 1
            $registrationAttempted = $false
            try {
                $registrationText = (& $register --status $dll 2>&1 | Out-String).Trim()
                $registrationExit = $LASTEXITCODE
                $registrationAttempted = $true
            }
            catch {
                $issues += "Registration status invocation failed: $($_.Exception.Message)"
            }
            $registrationValue = $null
            if (-not [string]::IsNullOrWhiteSpace($registrationText)) {
                try {
                    $registrationValue = $registrationText | ConvertFrom-Json -ErrorAction Stop
                }
                catch {
                    $issues += 'Registration status JSON could not be parsed.'
                }
            }
            $comServer = Get-WixInstallerScenarioComServer -ExpectedDll $dll -Identity $msiIdentity
            if (-not [string]::IsNullOrWhiteSpace([string]$comServer.issue)) {
                $issues += [string]$comServer.issue
            }
            $helperOk = $registrationExit -eq 0 -and $null -ne $registrationValue -and
                $registrationValue.registered -eq $true -and
                $registrationValue.immersive_supported -eq $true -and
                $registrationValue.input_mode_supported -eq $true -and
                $registrationValue.machine_com_registered -eq $true -and
                $registrationValue.machine_com -eq 0
            if ($comServer.observed -eq $true -and $registrationAttempted) {
                $registrationStatusOk = [bool]($helperOk -and $comServer.matches -eq $true)
            }
            $registration = [pscustomobject]@{
                exit_code = $registrationExit
                raw_json = $registrationText
                result = $registrationValue
                dll_path = $dll
                dll_sha256 = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
                dll_file_version = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
                dll_product_version = (Get-Item -LiteralPath $dll).VersionInfo.ProductVersion
                com_server = $comServer
            }
        }
        else {
            $issues += 'COM registration readback inputs are absent.'
        }
    }

    try {
        $productDirectoryAbsent = -not (Test-Path -LiteralPath $root -ErrorAction Stop)
    }
    catch {
        $issues += "Product-directory absence could not be read: $($_.Exception.Message)"
    }

    # Observe both registry views explicitly; merged HKCR and a missing product
    # identity alone cannot prove that COM or a disabled TSF profile is absent.
    if (-not $installed) {
        $comViews = @()
        $profileViews = @()
        foreach ($hive in @('CurrentUser', 'LocalMachine')) {
            foreach ($view in @('Registry32', 'Registry64')) {
                foreach ($entry in @(
                    @{ kind = 'com'; key = 'SOFTWARE\Classes\CLSID\{5D7C4291-2B34-4CE8-8A11-39627105C42E}' },
                    @{ kind = 'profile'; key = 'SOFTWARE\Microsoft\CTF\TIP\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\LanguageProfile\0x00000404\{7254A21F-6EB1-4217-A42C-C27301175D90}' }
                )) {
                    $base = $null; $key = $null; $absent = $null
                    try {
                        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
                            [Microsoft.Win32.RegistryHive]$hive, [Microsoft.Win32.RegistryView]$view)
                        $key = $base.OpenSubKey($entry.key, $false)
                        $absent = $null -eq $key
                    }
                    catch {
                        $issues += "Absence registry read failed ($hive/$view/$($entry.kind)): $($_.Exception.Message)"
                    }
                    finally {
                        if ($null -ne $key) { $key.Dispose() }
                        if ($null -ne $base) { $base.Dispose() }
                    }
                    $row = [pscustomobject]@{ hive = $hive; view = $view; absent = $absent }
                    if ($entry.kind -ceq 'com') { $comViews += $row } else { $profileViews += $row }
                }
            }
        }
        $comAbsent = if (@($comViews | Where-Object { $_.absent -eq $false }).Count) { $false }
            elseif (@($comViews | Where-Object { $null -eq $_.absent }).Count) { $null } else { $true }
        $profileRegistryAbsent = if (@($profileViews | Where-Object { $_.absent -eq $false }).Count) { $false }
            elseif (@($profileViews | Where-Object { $null -eq $_.absent }).Count) { $null } else { $true }
        $nativeAbsent = $null
        $nativeReadKnown = $false
        $observer = Join-Path $OutputDir 'registration-status.exe'
        $observerHashFile = $observer + '.sha256'
        try {
            if (-not (Test-Path -LiteralPath $observer -PathType Leaf) -or
                -not (Test-Path -LiteralPath $observerHashFile -PathType Leaf) -or
                (Get-FileHash -LiteralPath $observer -Algorithm SHA256).Hash.ToLowerInvariant() -cne
                    [IO.File]::ReadAllText($observerHashFile).Trim()) {
                throw 'A verified external registration observer is unavailable.'
            }
            # --status only reads registration; its DLL argument need not exist.
            $statusDll = Join-Path $root 'versions\absence\x64\BoshiamyTsf.dll'
            $nativeText = (& $observer --status $statusDll 2>&1 | Out-String).Trim()
            $nativeStatus = $nativeText | ConvertFrom-Json -ErrorAction Stop
            if ((Get-WixInstallerScenarioProperty $nativeStatus 'result' $null) -ne 0 -or
                (Get-WixInstallerScenarioProperty $nativeStatus 'cleanup_result' $null) -ne 0) {
                throw 'The native registration observer did not initialize successfully.'
            }
            $nativeAbsent = $true
            foreach ($name in @('profile', 'keyboard_category', 'system_tray_category', 'immersive_category', 'input_mode_category')) {
                $status = Get-WixInstallerScenarioProperty $nativeStatus $name $null
                if ($null -eq $status -or $status -notin @(0, 1)) {
                    throw "Native registration observation is missing or failed: $name."
                }
                if ($status -eq 0) { $nativeAbsent = $false }
            }
            $nativeReadKnown = $true
        }
        catch {
            $nativeAbsent = $null
            $issues += "Native registration absence could not be read: $($_.Exception.Message)"
        }
        $profileAbsent = if ($profileRegistryAbsent -eq $false -or $nativeAbsent -eq $false) { $false }
            elseif ($profileRegistryAbsent -eq $true -and $nativeAbsent -eq $true) { $true } else { $null }
        $registrationAbsent = if ($comAbsent -eq $false -or $profileAbsent -eq $false) { $false }
            elseif ($comAbsent -eq $true -and $profileAbsent -eq $true) { $true } else { $null }
        $registrationStatusOk = if ($nativeReadKnown) { $true } else { $null }
        $registration = [pscustomobject]@{
            absent = $registrationAbsent
            profile = [pscustomobject]@{ absent = $profileAbsent; views = @($profileViews); native_absent = $nativeAbsent }
            com_server = [pscustomobject]@{ absent = $comAbsent; views = @($comViews) }
        }
    }

    $pendingBase = $null; $pendingKey = $null
    try {
        $pendingBase = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
        $pendingKey = $pendingBase.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager', $false)
        if ($null -eq $pendingKey) { throw 'Session Manager registry key is unavailable.' }
        $ownedPendingDeletes = @()
        foreach ($name in @('PendingFileRenameOperations', 'PendingFileRenameOperations2')) {
            $pairs = $pendingKey.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            if ($null -eq $pairs) { continue }
            if ($pairs -isnot [string[]] -or $pairs.Count % 2 -ne 0) { throw "Malformed pending-file operations: $name." }
            for ($index = 0; $index -lt $pairs.Count; $index += 2) {
                $owned = $false
                foreach ($rawPath in @($pairs[$index], $pairs[$index + 1])) {
                    if (Test-WixInstallerScenarioOwnedCleanupPath -Path $rawPath -ProductRoot $root) {
                        $owned = $true
                    }
                }
                if ($owned) { $ownedPendingDeletes += [pscustomobject]@{ value = $name; source = $pairs[$index]; destination = $pairs[$index + 1] } }
            }
        }
    }
    catch {
        $ownedPendingDeletes = $null
        $issues += "Owned pending-file operations could not be read: $($_.Exception.Message)"
    }
    finally {
        if ($null -ne $pendingKey) { $pendingKey.Dispose() }
        if ($null -ne $pendingBase) { $pendingBase.Dispose() }
    }
    try {
        $retiredParent = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($root).TrimEnd('\'))
        $ownedRetiredFiles = @(Get-ChildItem -LiteralPath $retiredParent -Force -ErrorAction Stop |
            Where-Object {
                Test-WixInstallerScenarioOwnedCleanupPath -Path $_.FullName -ProductRoot $root -RetiredOnly
            } | ForEach-Object { $_.FullName } | Sort-Object)
    }
    catch {
        $ownedRetiredFiles = $null
        $issues += "Owned retirement files could not be read: $($_.Exception.Message)"
    }
    $msi = Get-WixInstallerScenarioMsiProducts
    $uninstall = Get-WixInstallerScenarioUninstallEntries
    $retention = Get-WixInstallerScenarioStateRetention -FixtureExe $StateFixture -OutputDir $OutputDir
    return [pscustomobject]@{
        collected_utc = [DateTime]::UtcNow.ToString('o')
        installed = [bool]$installed
        install_root = $root
        runtime_version = $runtimeVersion
        owner = $owner
        install_state = $installState
        manifest_path = $manifestPath
        manifest_sha256 = $manifestSha256
        manifest = $manifestObject
        manifest_expected_file_count = $expectedPayloadCount
        manifest_payload_count = $payloadFiles.Count
        payload_files = @($payloadFiles)
        manifest_integrity_ok = $manifestIntegrityOk
        identity_integrity_ok = $identityIntegrityOk
        registration = $registration
        registration_status_ok = $registrationStatusOk
        registration_absent = $registrationAbsent
        product_directory_absent = $productDirectoryAbsent
        owned_pending_deletes = $ownedPendingDeletes
        owned_retired_files = $ownedRetiredFiles
        msi_identity = $msiIdentity
        msi_products = @($msi.products)
        msi_product_issues = @($msi.issues)
        uninstall_entries = @($uninstall.entries)
        uninstall_registry_issues = @($uninstall.issues)
        retention = $retention
        issues = @($issues + $msi.issues + $uninstall.issues)
    }
}

function Ensure-WixInstallerScenarioProcessNative {
    if ([bool](Get-Variable -Name WixInstallerScenarioProcessNativeLoaded -Scope Script -ErrorAction SilentlyContinue)) {
        return
    }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class WixInstallerScenarioProcessNative
{
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inherit, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool GetTokenInformation(
        IntPtr token, int information, out uint elevation, int size, out int returned);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetProcessId(IntPtr process);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetProcessTimes(IntPtr process,
        out long created, out long exited, out long kernel, out long user);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr process, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool TerminateProcess(IntPtr process, uint exitCode);

    public static bool TryTerminateVerifiedHandle(
        IntPtr process, int expectedPid, long expectedCreated, uint exitCode, out int error)
    {
        error = 0;
        if (process == IntPtr.Zero || process == new IntPtr(-1) || expectedPid <= 0) {
            error = 6;
            return false;
        }
        uint pid = GetProcessId(process);
        long created, exited, kernel, user;
        if (pid == 0 || !GetProcessTimes(process, out created, out exited, out kernel, out user)) {
            error = Marshal.GetLastWin32Error();
            return false;
        }
        if (pid != (uint)expectedPid || created != expectedCreated) {
            error = 13;
            return false;
        }
        if (WaitForSingleObject(process, 0) != 258) {
            error = 1168;
            return false;
        }
        if (!TerminateProcess(process, exitCode)) {
            error = Marshal.GetLastWin32Error();
            return false;
        }
        return true;
    }

    public static bool TryIsElevated(int processId, out bool elevated, out int error)
    {
        const uint ProcessQueryLimitedInformation = 0x1000;
        const uint TokenQuery = 0x0008;
        const int TokenElevation = 20;
        elevated = false;
        error = 0;
        IntPtr process = OpenProcess(ProcessQueryLimitedInformation, false, processId);
        if (process == IntPtr.Zero) {
            error = Marshal.GetLastWin32Error();
            return false;
        }
        IntPtr token = IntPtr.Zero;
        try {
            if (!OpenProcessToken(process, TokenQuery, out token)) {
                error = Marshal.GetLastWin32Error();
                return false;
            }
            uint value;
            int returned;
            if (!GetTokenInformation(token, TokenElevation, out value, sizeof(uint), out returned)) {
                error = Marshal.GetLastWin32Error();
                return false;
            }
            elevated = value != 0;
            return true;
        }
        finally {
            if (token != IntPtr.Zero) CloseHandle(token);
            CloseHandle(process);
        }
    }
}
'@
    $script:WixInstallerScenarioProcessNativeLoaded = $true
}

function Get-WixInstallerScenarioProcessObservation {
    param(
        [Parameter(Mandatory = $true)][int]$ProcessId,
        [AllowEmptyCollection()][string[]]$TrustedBaHashes = @(),
        [string]$SetupHash = '',
        [int]$ParentPid = 0
    )

    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        return $null
    }
    $path = ''
    try {
        $path = [string]$process.Path
    }
    catch [System.ComponentModel.Win32Exception] {
        $path = ''
    }
    catch [System.InvalidOperationException] {
        $path = ''
    }
    $processName = ''
    try {
        $processName = [string]$process.ProcessName
    }
    catch [System.ComponentModel.Win32Exception] {
        $processName = ''
    }
    catch [System.InvalidOperationException] {
        $processName = ''
    }
    $sha256 = ''
    if (-not [string]::IsNullOrWhiteSpace($path) -and (Test-Path -LiteralPath $path -PathType Leaf)) {
        try {
            $sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        catch [IO.IOException] {
            $sha256 = ''
        }
        catch [UnauthorizedAccessException] {
            $sha256 = ''
        }
    }
    Ensure-WixInstallerScenarioProcessNative
    [bool]$elevated = $false
    [int]$elevationError = 0
    $elevationKnown = [WixInstallerScenarioProcessNative]::TryIsElevated(
        $ProcessId,
        [ref]$elevated,
        [ref]$elevationError)
    $creation = ''
    try {
        $creation = $process.StartTime.ToUniversalTime().ToString('o')
    }
    catch [System.ComponentModel.Win32Exception] {
        $creation = ''
    }
    catch [System.InvalidOperationException] {
        $creation = ''
    }
    $alive = $null
    try {
        $process.Refresh()
        if ($process.HasExited) {
            $alive = $false
        }
        elseif (-not [string]::IsNullOrWhiteSpace($path) -and
            -not [string]::IsNullOrWhiteSpace($processName) -and
            -not [string]::IsNullOrWhiteSpace($creation) -and
            $elevationKnown) {
            $alive = $true
        }
    }
    catch [System.ComponentModel.Win32Exception] {
        $alive = $null
    }
    catch [System.InvalidOperationException] {
        $alive = $null
    }

    $fileName = if ([string]::IsNullOrWhiteSpace($path)) {
        $processName
    }
    else {
        [IO.Path]::GetFileName($path)
    }
    $trusted = -not [string]::IsNullOrWhiteSpace($sha256) -and
        @($TrustedBaHashes | Where-Object { $_ -ceq $sha256 }).Count -gt 0
    $isSetup = -not [string]::IsNullOrWhiteSpace($SetupHash) -and $sha256 -ceq $SetupHash
    $kind = if ($fileName -match '(?i)^msiexec\.exe$' -or $processName -ceq 'msiserver') {
        'windows_installer'
    }
    elseif (-not $elevationKnown) {
        'unknown_elevation'
    }
    elseif ($isSetup) {
        if ($elevated) { 'elevated_burn_engine' } else { 'burn_engine' }
    }
    elseif ($trusted) {
        if ($elevated) { 'elevated_burn_ba' } else { 'candidate_ba_ui' }
    }
    else {
        'other'
    }
    return [pscustomobject]@{
        process_kind = $kind
        process_name = $fileName
        path = $path
        sha256 = $sha256
        pid = $ProcessId
        parent_pid = $ParentPid
        creation_time_utc = $creation
        elevated = if ($elevationKnown) { [bool]$elevated } else { $null }
        elevation_error = if ($elevationKnown) { 0 } else { $elevationError }
        trusted_ba_hash = [bool]$trusted
        setup_hash_match = [bool]$isSetup
        alive = $alive
    }
}

function Get-WixInstallerScenarioProcessTree {
    param(
        [Parameter(Mandatory = $true)][int]$RootPid,
        [Parameter(Mandatory = $true)][string]$RootCreationTimeUtc,
        [Parameter(Mandatory = $true)]$RootProcess,
        [Parameter(Mandatory = $true)][string]$SetupHash,
        [Parameter(Mandatory = $true)][string[]]$TrustedBaHashes
    )

    $RootProcess.Refresh()
    if ($RootProcess.HasExited -or $RootProcess.Id -ne $RootPid -or
        $RootProcess.StartTime.ToUniversalTime().ToString('o') -cne $RootCreationTimeUtc) {
        return @()
    }
    $rows = @(Get-CimInstance Win32_Process -ErrorAction Stop)
    $children = @{}
    foreach ($row in $rows) {
        $parent = [int]$row.ParentProcessId
        if (-not $children.ContainsKey($parent)) {
            $children[$parent] = @()
        }
        $children[$parent] += $row
    }
    $queue = [Collections.Generic.Queue[object]]::new()
    $root = @($rows | Where-Object { [int]$_.ProcessId -eq $RootPid })
    if ($root.Count -eq 1) {
        $queue.Enqueue([pscustomobject]@{
            pid = $RootPid
            parent_pid = [int]$root[0].ParentProcessId
            ancestry_pids = @($RootPid)
        })
    }
    $visited = @{}
    $observations = @()
    while ($queue.Count -gt 0) {
        $item = $queue.Dequeue()
        if ($visited.ContainsKey([int]$item.pid)) {
            continue
        }
        $visited[[int]$item.pid] = $true
        $observation = Get-WixInstallerScenarioProcessObservation `
            -ProcessId ([int]$item.pid) `
            -SetupHash $SetupHash `
            -TrustedBaHashes $TrustedBaHashes `
            -ParentPid ([int]$item.parent_pid)
        if ([int]$item.pid -eq $RootPid -and
            ($null -eq $observation -or $observation.setup_hash_match -ne $true)) {
            return @()
        }
        if ($null -ne $observation) {
            $observation | Add-Member -NotePropertyName root_pid -NotePropertyValue $RootPid
            $observation | Add-Member -NotePropertyName root_creation_time_utc `
                -NotePropertyValue $RootCreationTimeUtc
            $observation | Add-Member -NotePropertyName ancestry_pids `
                -NotePropertyValue @($item.ancestry_pids)
            $observations += $observation
        }
        $childRows = if ($children.ContainsKey([int]$item.pid)) { $children[[int]$item.pid] } else { @() }
        foreach ($child in $childRows) {
            $queue.Enqueue([pscustomobject]@{
                pid = [int]$child.ProcessId
                parent_pid = [int]$item.pid
                ancestry_pids = @($item.ancestry_pids) + [int]$child.ProcessId
            })
        }
    }
    return @($observations)
}

function Get-WixInstallerScenarioTrustedBaHashes {
    param([Parameter(Mandatory = $true)]$CandidateIdentity)

    $setupHash = [string](Get-WixInstallerScenarioProperty $CandidateIdentity 'setup_sha256')
    if ($setupHash -cnotmatch '^[0-9a-f]{64}$') {
        throw '[SC034] Candidate setup hash is malformed.'
    }
    $payloads = @(Get-WixInstallerScenarioProperty $CandidateIdentity 'ba_payloads' @())
    if ($payloads.Count -eq 0) {
        throw '[SC034] Candidate extracted BA payload identities are required.'
    }
    $hashes = @()
    foreach ($payload in $payloads) {
        $hash = [string](Get-WixInstallerScenarioProperty $payload 'sha256')
        $path = [string](Get-WixInstallerScenarioProperty $payload 'path')
        if ($hash -cnotmatch '^[0-9a-f]{64}$' -or $hash -ceq $setupHash) {
            throw '[SC034] Candidate BA payload hash must be a distinct extracted BA image.'
        }
        if (-not [string]::IsNullOrWhiteSpace($path)) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
                (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash) {
                throw '[SC034] Candidate BA payload extraction hash does not match the manifest.'
            }
        }
        $hashes += $hash
    }
    return @($hashes | Where-Object { $_ -cmatch '^[0-9a-f]{64}$' } | Select-Object -Unique)
}

function Start-WixInstallerScenarioSetup {
    param(
        [Parameter(Mandatory = $true)][string]$SetupPath,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [string]$BurnLog = '',
        [switch]$Passive
    )

    $arguments = @()
    if ($Passive) {
        $arguments += '/passive'
        $arguments += '/norestart'
        if (-not [string]::IsNullOrWhiteSpace($BurnLog)) {
            $arguments += @('/log', ('"' + $BurnLog + '"'))
        }
    }
    elseif (-not [string]::IsNullOrWhiteSpace($BurnLog)) {
        $arguments += @('/log', ('"' + $BurnLog + '"'))
    }
    $process = if ($arguments.Count -gt 0) {
        Start-Process -FilePath $SetupPath -ArgumentList $arguments `
            -WorkingDirectory $WorkingDirectory -PassThru
    }
    else {
        Start-Process -FilePath $SetupPath `
            -WorkingDirectory $WorkingDirectory -PassThru
    }
    [void]$process.Handle
    return [pscustomobject]@{
        process = $process
        pid = $process.Id
        creation_time_utc = $process.StartTime.ToUniversalTime().ToString('o')
        started_utc = [DateTime]::UtcNow.ToString('o')
        arguments = @($arguments)
        setup_path = $SetupPath
        burn_log = $BurnLog
    }
}

function Wait-WixInstallerScenarioMarker {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [Parameter(Mandatory = $true)][string]$Phase
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            try {
                $text = Read-WixInstallerScenarioSharedLog $Path
                if ([regex]::IsMatch($text, $Pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
                    return [pscustomobject]@{ observed = $true; text = $text }
                }
                if ($text -match 'Apply complete') {
                    return [pscustomobject]@{ observed = $false; text = $text }
                }
            }
            catch [IO.IOException] {
                # The live installer may be replacing or holding the log.
            }
        }
        Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase $Phase -Status 'waiting_for_evidence'
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    return [pscustomobject]@{ observed = $false; text = $null }
}

function Wait-WixInstallerScenarioProcess {
    param(
        [Parameter(Mandatory = $true)]$Process,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [Parameter(Mandatory = $true)][string]$Phase
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $Process.Refresh()
        if ($Process.HasExited) {
            return [pscustomobject]@{
                exited = $true
                exit_code = $Process.ExitCode
                ended_utc = $Process.ExitTime.ToUniversalTime().ToString('o')
            }
        }
        Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase $Phase -Status 'process_running' -Detail @{
            pid = $Process.Id
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    return [pscustomobject]@{
        exited = $false
        exit_code = $null
        ended_utc = $null
    }
}

function Start-WixInstallerScenarioMappedModule {
    param(
        [Parameter(Mandatory = $true)][string]$FixtureExe,
        [Parameter(Mandatory = $true)][string]$DllPath,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [Parameter(Mandatory = $true)][int]$HoldSeconds
    )

    $stdout = Join-Path $OutputDir 'mapped-module.stdout.txt'
    $stderr = Join-Path $OutputDir 'mapped-module.stderr.txt'
    $ready = Join-Path $OutputDir 'mapped-module.ready.txt'
    $process = Start-Process -FilePath $FixtureExe `
        -ArgumentList @(('"' + $DllPath + '"'), ('"' + $ready + '"'), [string]$HoldSeconds) `
        -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    [void]$process.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $readyText = if (Test-Path -LiteralPath $ready -PathType Leaf) {
            [IO.File]::ReadAllText($ready)
        }
        else {
            ''
        }
        if ($readyText -match ('ready pid=' + $process.Id + '\b')) {
            $process.Refresh()
            $modules = @()
            try {
                $modules = @(
                    $process.Modules | Where-Object {
                        [string]$_.FileName -ieq (Resolve-Path -LiteralPath $DllPath).Path
                    }
                )
            }
            catch [System.ComponentModel.Win32Exception] {
                $modules = @()
            }
            catch [System.InvalidOperationException] {
                $modules = @()
            }
            if ($modules.Count -ne 1) {
                throw '[SC040] The mapped-module fixture is ready but the target DLL module is not observable.'
            }
            $creationTimeUtc = ''
            try {
                $creationTimeUtc = $process.StartTime.ToUniversalTime().ToString('o')
            }
            catch [System.ComponentModel.Win32Exception] {
                $creationTimeUtc = ''
            }
            catch [System.InvalidOperationException] {
                $creationTimeUtc = ''
            }
            if ([string]::IsNullOrWhiteSpace($creationTimeUtc)) {
                throw '[SC040] The mapped-module fixture creation time is not observable.'
            }
            return [pscustomobject]@{
                process = $process
                pid = $process.Id
                creation_time_utc = $creationTimeUtc
                fixture_path = (Resolve-Path -LiteralPath $FixtureExe).Path
                fixture_sha256 = (Get-FileHash -LiteralPath $FixtureExe -Algorithm SHA256).Hash.ToLowerInvariant()
                dll_path = (Resolve-Path -LiteralPath $DllPath).Path
                dll_sha256 = (Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash.ToLowerInvariant()
                module_path = $modules[0].FileName
                ready_text = $readyText.Trim()
                hold_seconds = $HoldSeconds
            }
        }
        if ($process.HasExited) {
            throw "[SC040] The mapped-module fixture exited before becoming ready: exit $($process.ExitCode)."
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw '[SC040] The mapped-module fixture did not become ready before its bounded observation timeout.'
}

function Get-WixInstallerScenarioMsiPathsFromBurn {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$BurnText)

    $paths = @()
    foreach ($match in [regex]::Matches(
        $BurnText,
        '(?<path>[A-Za-z]:\\[^:"/<>|?*\x00-\x1F]+?\.log)',
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
    )) {
        $path = $match.Groups['path'].Value.Trim().Trim([char[]]@('"', "'"))
        try {
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                $paths += (Resolve-Path -LiteralPath $path).Path
            }
        }
        catch [System.ArgumentException] { continue }
        catch [System.NotSupportedException] { continue }
        catch [System.IO.IOException] { continue }
        catch [System.Management.Automation.RuntimeException] { continue }
    }
    return @($paths | Select-Object -Unique)
}

function Get-WixInstallerScenarioDiscoveredLogFiles {
    param(
        [DateTime]$StartedUtc = [DateTime]::MinValue,
        [Parameter(Mandatory = $true)][string]$ExplicitBurnLog,
        [DateTime]$EndedUtc = [DateTime]::MaxValue
    )

    $files = @()
    if (Test-Path -LiteralPath $ExplicitBurnLog -PathType Leaf) {
        $files += Get-Item -LiteralPath $ExplicitBurnLog
        foreach ($path in @(Get-WixInstallerScenarioMsiPathsFromBurn (
            Read-WixInstallerScenarioSharedLog $ExplicitBurnLog
        ))) {
            $files += Get-Item -LiteralPath $path
        }
    }
    return @($files | Sort-Object FullName -Unique)
}

function Copy-WixInstallerScenarioRawLogs {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Logs,
        [Parameter(Mandatory = $true)][string]$OutputDir
    )

    $raw = Join-Path $OutputDir 'raw-logs'
    [IO.Directory]::CreateDirectory($raw) | Out-Null
    $copied = @()
    $sequence = 0
    foreach ($log in @($Logs)) {
        $source = [string](Get-WixInstallerScenarioProperty $log 'path')
        if ([string]::IsNullOrWhiteSpace($source) -or -not (Test-Path -LiteralPath $source -PathType Leaf)) {
            continue
        }
        $run = [string](Get-WixInstallerScenarioProperty $log 'run')
        $kind = [string](Get-WixInstallerScenarioProperty $log 'kind')
        $name = ('{0:d3}-{1}-{2}-{3}' -f
            $sequence, $run, $kind, [IO.Path]::GetFileName($source))
        $destination = Join-Path $raw $name
        [IO.File]::Copy($source, $destination, $false)
        $copied += [pscustomobject]@{
            run = $run
            kind = $kind
            source_path = $source
            output_path = $destination
            sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        $sequence++
    }
    return @($copied)
}

function Get-WixInstallerScenarioMsiLogEntries {
    param(
        [Parameter(Mandatory = $true)][DateTime]$StartedUtc,
        [DateTime]$EndedUtc = [DateTime]::MaxValue,
        [Parameter(Mandatory = $true)][string]$ExplicitBurnLog,
        [Parameter(Mandatory = $true)][string]$Run,
        [string]$SecondaryBurnLog = ''
    )

    $burnText = if (Test-Path -LiteralPath $ExplicitBurnLog -PathType Leaf) {
        Read-WixInstallerScenarioSharedLog $ExplicitBurnLog
    }
    else {
        ''
    }
    $firstPaths = @(Get-WixInstallerScenarioMsiPathsFromBurn $burnText)
    $secondPaths = @()
    if (-not [string]::IsNullOrWhiteSpace($SecondaryBurnLog) -and
        (Test-Path -LiteralPath $SecondaryBurnLog -PathType Leaf)) {
        $secondPaths = @(Get-WixInstallerScenarioMsiPathsFromBurn (Read-WixInstallerScenarioSharedLog $SecondaryBurnLog))
    }
    $candidateFiles = @(
        Get-WixInstallerScenarioDiscoveredLogFiles `
            -StartedUtc $StartedUtc `
            -EndedUtc $EndedUtc `
            -ExplicitBurnLog $ExplicitBurnLog
    )
    $candidateFiles += @($secondPaths | ForEach-Object { Get-Item -LiteralPath $_ })
    $entries = @()
    $unattributed = @()
    foreach ($file in @($candidateFiles | Sort-Object FullName -Unique)) {
        if ($file.FullName -ieq $ExplicitBurnLog -or
            ($SecondaryBurnLog -and $file.FullName -ieq $SecondaryBurnLog)) {
            continue
        }
        try {
            $text = Read-WixInstallerScenarioSharedLog $file.FullName
        }
        catch [IO.IOException] {
            continue
        }
        if (-not [regex]::IsMatch($text, '(?i)Action start[^\r\n]*Install|MSI \(s\)|=== Verbose logging started')) {
            continue
        }
        $assignedRun = $Run
        if (@($secondPaths | Where-Object { $_ -ieq $file.FullName }).Count -gt 0) {
            $assignedRun = 'second'
        }
        elseif (@($firstPaths | Where-Object { $_ -ieq $file.FullName }).Count -eq 0 -and
            -not [string]::IsNullOrWhiteSpace($SecondaryBurnLog)) {
            continue
        }
        $entries += [pscustomobject]@{
            run = $assignedRun
            kind = 'msi'
            path = $file.FullName
            text = $text
        }
    }
    return [pscustomobject]@{
        entries = @($entries)
        unattributed_paths = @($unattributed)
    }
}

function Test-WixInstallerScenarioBundleMatches {
    param(
        [Parameter(Mandatory = $true)]$Snapshot,
        [Parameter(Mandatory = $true)]$Identity
    )

    # Read raw arrays so a complete empty observation stays distinct from unknown.
    $observations = @{}
    foreach ($name in @('uninstall_registry_issues', 'uninstall_entries')) {
        $observations[$name] = $null
        if ($Snapshot -is [System.Collections.IDictionary]) {
            if ($Snapshot.Contains($name)) { $observations[$name] = $Snapshot[$name] }
        }
        elseif ($null -ne $Snapshot.PSObject.Properties[$name]) {
            $observations[$name] = $Snapshot.PSObject.Properties[$name].Value
        }
    }
    if ($null -eq $observations['uninstall_registry_issues'] -or
        $null -eq $observations['uninstall_entries'] -or
        @($observations['uninstall_registry_issues']).Count -ne 0) {
        return $null
    }
    $entries = @($observations['uninstall_entries'])
    $expectedVersion = [string](Get-WixInstallerScenarioProperty $Identity 'msi_version')
    if ([string]::IsNullOrWhiteSpace($expectedVersion)) { return $null }
    $bundleMatches = @($entries | Where-Object {
        (Get-WixInstallerScenarioProperty $_ 'kind') -ceq 'burn_bundle' -and
        (Get-WixInstallerScenarioProperty $_ 'display_version') -ceq $expectedVersion -and
        (Get-WixInstallerScenarioProperty (Get-WixInstallerScenarioProperty $_ 'values') 'Installed' 0) -eq 1
    })
    $bundleMismatch = @($entries | Where-Object {
        (Get-WixInstallerScenarioProperty $_ 'kind') -ceq 'burn_bundle' -and
        (Get-WixInstallerScenarioProperty $_ 'display_version') -cne $expectedVersion
    })
    if ($bundleMatches.Count -eq 0 -or $bundleMismatch.Count -ne 0) { return $false }

    $unknownResume = $false
    foreach ($entry in $bundleMatches) {
        $resume = Get-WixInstallerScenarioProperty (
            Get-WixInstallerScenarioProperty $entry 'values') 'Resume' $null
        $resumeCode = 0
        if ($null -eq $resume -or -not [int]::TryParse([string]$resume, [ref]$resumeCode)) {
            $unknownResume = $true
            continue
        }
        # WiX 6.0.2: Active=1, Suspend=2, ARP=3, RebootPending=4.
        # The caller must establish that the original engine has exited.
        if ($resumeCode -eq 1 -or $resumeCode -eq 2) { return $false }
        if ($resumeCode -ne 3) { $unknownResume = $true }
    }
    if ($unknownResume) { return $null }
    return $true
}

function Test-WixInstallerScenarioSnapshotMatches {
    param(
        [Parameter(Mandatory = $true)]$Snapshot,
        [Parameter(Mandatory = $true)]$Identity,
        [switch]$RequireConsistentBundle,
        [switch]$RequireConsistentPayload
    )

    if ((Get-WixInstallerScenarioProperty $Snapshot 'installed' $false) -ne $true -or
        [string](Get-WixInstallerScenarioProperty $Snapshot 'runtime_version') -cne
            [string](Get-WixInstallerScenarioProperty $Identity 'runtime_version')) {
        return $false
    }
    $msiIdentity = Get-WixInstallerScenarioProperty $Snapshot 'msi_identity'
    if ($null -eq $msiIdentity -or
        [string](Get-WixInstallerScenarioProperty $msiIdentity 'Owner') -cne 'LiuKai-TSF-v1' -or
        [string](Get-WixInstallerScenarioProperty $msiIdentity 'Version') -cne
            [string](Get-WixInstallerScenarioProperty $Identity 'runtime_version') -or
        (Get-WixInstallerScenarioProperty $Snapshot 'identity_integrity_ok' $false) -ne $true) {
        return $false
    }
    if ($Snapshot.manifest_expected_file_count -le 0 -or
        $Snapshot.manifest_payload_count -ne $Snapshot.manifest_expected_file_count -or
        [string]::IsNullOrWhiteSpace([string]$Snapshot.manifest_sha256) -or
        [string](Get-WixInstallerScenarioProperty $msiIdentity 'ManifestSha256') -cne
            [string](Get-WixInstallerScenarioProperty $Snapshot 'manifest_sha256') -or
        $Snapshot.manifest_integrity_ok -ne $true -or
        @($Snapshot.payload_files | Where-Object {
            $_.sha256 -cne $_.expected_sha256 -or $_.bytes -ne $_.expected_bytes
        }).Count -ne 0) {
        return $false
    }
    if ($Snapshot.registration_status_ok -ne $true -or
        $null -eq $Snapshot.registration -or
        $Snapshot.registration.com_server.matches -ne $true) {
        return $false
    }
    $msiMatches = @($Snapshot.msi_products | Where-Object {
        $_.version -ceq [string]$Identity.msi_version
    })
    if ($msiMatches.Count -ne 1 -or
        @($Snapshot.msi_products | Where-Object {
            $_.version -cne [string]$Identity.msi_version
        }).Count -ne 0) {
        return $false
    }
    if ($RequireConsistentBundle) {
        if (-not (Test-WixInstallerScenarioBundleMatches -Snapshot $Snapshot -Identity $Identity)) {
            return $false
        }
    }
    if ($RequireConsistentPayload -and $Snapshot.issues.Count -ne 0) {
        return $false
    }
    return $true
}

function Get-WixInstallerScenarioRetentionComparison {
    param(
        [Parameter(Mandatory = $true)]$Before,
        [Parameter(Mandatory = $true)]$After
    )

    $beforeFiles = @($Before.files)
    $afterFiles = @($After.files)
    $settingsBefore = @($beforeFiles | Where-Object { $_.relative_path -ceq 'settings.conf' })
    $settingsAfter = @($afterFiles | Where-Object { $_.relative_path -ceq 'settings.conf' })
    $doctorBefore = @($beforeFiles | Where-Object { $_.relative_path -like 'doctor/*' })
    $doctorAfter = @($afterFiles | Where-Object { $_.relative_path -like 'doctor/*' })
    $equalRows = {
        param($Left, $Right)
        $leftRows = @($Left | Sort-Object relative_path |
            ForEach-Object { "$($_.relative_path)=$($_.sha256):$($_.bytes)" })
        $rightRows = @($Right | Sort-Object relative_path |
            ForEach-Object { "$($_.relative_path)=$($_.sha256):$($_.bytes)" })
        return $leftRows.Count -eq $rightRows.Count -and ($leftRows -join "`n") -ceq ($rightRows -join "`n")
    }
    return [pscustomobject]@{
        settings_preserved = & $equalRows $settingsBefore $settingsAfter
        doctor_preserved = (& $equalRows $doctorBefore $doctorAfter) -and
            $After.fixture.synthetic_fixture -eq $true -and
            $After.fixture.doctor_enabled -eq $false -and
            $After.fixture.doctor_record_count -eq 1
        fingerprint_preserved = $Before.fingerprint -ceq $After.fingerprint
    }
}

function Get-WixInstallerScenarioOldRegistrationAbsence {
    param(
        [Parameter(Mandatory = $true)]$Before,
        [Parameter(Mandatory = $true)]$After,
        [Parameter(Mandatory = $true)]$BaselineIdentity
    )

    if (@($Before.msi_product_issues).Count -ne 0 -or
        @($After.msi_product_issues).Count -ne 0 -or
        @($Before.uninstall_registry_issues).Count -ne 0 -or
        @($After.uninstall_registry_issues).Count -ne 0) {
        return $null
    }
    $oldProductCodes = @($Before.msi_products | ForEach-Object { [string]$_.product_code })
    $oldCodePresent = @($After.msi_products | Where-Object {
        $oldProductCodes -contains [string]$_.product_code
    }).Count -gt 0
    $oldMsiVersionPresent = @($After.msi_products | Where-Object {
        $_.version -ceq [string]$BaselineIdentity.msi_version
    }).Count -gt 0
    $oldBurnVersionPresent = @($After.uninstall_entries | Where-Object {
        $_.kind -ceq 'burn_bundle' -and
        $_.display_version -ceq [string]$BaselineIdentity.msi_version
    }).Count -gt 0
    return -not $oldCodePresent -and -not $oldMsiVersionPresent -and -not $oldBurnVersionPresent
}

function New-WixInstallerScenarioReadback {
    param(
        [Parameter(Mandatory = $true)]$Before,
        [Parameter(Mandatory = $true)]$After,
        [Parameter(Mandatory = $true)]$Preflight,
        [int]$TargetedReentryRetries = 0,
        [int]$InjectionAttempts = 0,
        [string]$InjectionOrigin = '',
        $BundleConsistentAtInterrupt = $null,
        [string]$NormalUiRerunMode = '',
        [string[]]$RerunArguments = @(),
        [bool]$RerunConverged = $false,
        [bool]$UninstallSucceeded = $false
    )

    $candidateMsi = @($After.msi_products | Where-Object {
        $_.version -ceq [string]$Preflight.candidate.msi_version
    })
    $retention = Get-WixInstallerScenarioRetentionComparison `
        -Before $Before.retention -After $After.retention
    return [pscustomobject]@{
        expected_runtime_version = $Preflight.candidate.runtime_version
        expected_msi_version = $Preflight.candidate.msi_version
        observed_runtime_version = $After.runtime_version
        observed_payload_version = if ($After.manifest_integrity_ok -eq $true -and
            $null -ne $After.manifest) {
            [string]$After.manifest.version
        }
        else {
            $null
        }
        observed_com_version = if ($After.registration_status_ok -eq $true -and
            $candidateMsi.Count -eq 1) {
            [string]$After.runtime_version
        }
        else {
            $null
        }
        manifest_integrity_ok = $After.manifest_integrity_ok
        registration_status_ok = $After.registration_status_ok
        new_bundle_registered = if (@($After.uninstall_registry_issues).Count -gt 0) {
            $null
        }
        else {
            Test-WixInstallerScenarioBundleMatches `
                -Snapshot $After -Identity $Preflight.candidate
        }
        old_bundle_and_msi_registrations_absent = Get-WixInstallerScenarioOldRegistrationAbsence `
            -Before $Before -After $After -BaselineIdentity $Preflight.baseline
        targeted_reentry_retries = $TargetedReentryRetries
        injection_attempts = $InjectionAttempts
        injection_origin = $InjectionOrigin
        bundle_consistent_at_interrupt = $BundleConsistentAtInterrupt
        normal_ui_rerun_mode = $NormalUiRerunMode
        rerun_arguments = @($RerunArguments)
        rerun_converged = $RerunConverged
        uninstall_succeeded = $UninstallSucceeded
        settings_preserved = $retention.settings_preserved
        doctor_preserved = $retention.doctor_preserved
        retention_fingerprint_preserved = $retention.fingerprint_preserved
    }
}

function Test-WixInstallerScenarioInstalledAbsent {
    param([Parameter(Mandatory = $true)]$Snapshot)

    $missing = $false
    $remnant = $false
    # Keep empty observed arrays distinct from null; pipeline-returned arrays unroll.
    $arrays = @{}
    foreach ($name in @('msi_products', 'msi_product_issues', 'uninstall_entries',
                       'uninstall_registry_issues', 'owned_pending_deletes', 'owned_retired_files')) {
        $arrays[$name] = $null
        if ($Snapshot -is [System.Collections.IDictionary]) {
            if ($Snapshot.Contains($name)) { $arrays[$name] = $Snapshot[$name] }
        }
        elseif ($null -ne $Snapshot.PSObject.Properties[$name]) {
            $arrays[$name] = $Snapshot.PSObject.Properties[$name].Value
        }
    }
    if (-not (Test-WixInstallerScenarioProperty $Snapshot 'installed') -or
        $null -eq (Get-WixInstallerScenarioProperty $Snapshot 'installed' $null)) {
        $missing = $true
    }
    elseif ((Get-WixInstallerScenarioProperty $Snapshot 'installed') -ne $false) {
        $remnant = $true
    }
    if (-not (Test-WixInstallerScenarioProperty $Snapshot 'msi_identity')) {
        $missing = $true
    }
    elseif ($null -ne (Get-WixInstallerScenarioProperty $Snapshot 'msi_identity')) {
        $remnant = $true
    }
    $products = $arrays['msi_products']
    $productIssues = $arrays['msi_product_issues']
    if ($null -eq $products -or $null -eq $productIssues) {
        $missing = $true
    }
    else {
        if (@($products).Count -gt 0) { $remnant = $true }
        if (@($productIssues).Count -gt 0) { $missing = $true }
    }
    $entries = $arrays['uninstall_entries']
    $registryIssues = $arrays['uninstall_registry_issues']
    if ($null -eq $entries -or $null -eq $registryIssues) {
        $missing = $true
    }
    else {
        if (@($entries | Where-Object {
            (Get-WixInstallerScenarioProperty $_ 'kind') -in @('burn_bundle', 'msi_product')
        }).Count -gt 0) { $remnant = $true }
        if (@($registryIssues).Count -gt 0) { $missing = $true }
    }
    if ((Get-WixInstallerScenarioProperty $Snapshot 'registration_absent' $null) -ne $true) {
        if ((Test-WixInstallerScenarioProperty $Snapshot 'registration_absent') -and
            $null -ne (Get-WixInstallerScenarioProperty $Snapshot 'registration_absent' $null)) {
            $remnant = $true
        }
        else {
            $missing = $true
        }
    }
    if ((Get-WixInstallerScenarioProperty $Snapshot 'registration_status_ok' $null) -ne $true) {
        if ((Test-WixInstallerScenarioProperty $Snapshot 'registration_status_ok') -and
            $null -ne (Get-WixInstallerScenarioProperty $Snapshot 'registration_status_ok' $null)) {
            $remnant = $true
        }
        else {
            $missing = $true
        }
    }
    $registration = Get-WixInstallerScenarioProperty $Snapshot 'registration' $null
    if ($null -eq $registration) {
        $missing = $true
    }
    else {
        foreach ($proof in @(
            @{ object = $registration; name = 'absent' },
            @{ object = (Get-WixInstallerScenarioProperty $registration 'profile'); name = 'absent' },
            @{ object = (Get-WixInstallerScenarioProperty $registration 'com_server'); name = 'absent' }
        )) {
            $value = Get-WixInstallerScenarioProperty $proof.object $proof.name $null
            if (-not (Test-WixInstallerScenarioProperty $proof.object $proof.name) -or
                $null -eq $value) {
                $missing = $true
            }
            elseif ($value -ne $true) {
                $remnant = $true
            }
        }
    }
    $directoryAbsent = Get-WixInstallerScenarioProperty $Snapshot 'product_directory_absent' $null
    if ($null -eq $directoryAbsent) {
        $missing = $true
    }
    elseif ($directoryAbsent -ne $true) {
        $remnant = $true
    }
    foreach ($name in @('owned_pending_deletes', 'owned_retired_files')) {
        $cleanupRemnants = $arrays[$name]
        if ($null -eq $cleanupRemnants) {
            $missing = $true
        }
        elseif (@($cleanupRemnants).Count -gt 0) {
            $remnant = $true
        }
    }
    if ($remnant) { return $false }
    if ($missing) { return $null }
    return $true
}

function New-WixInstallerScenarioBurnLogEntry {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Run
    )

    return [pscustomobject]@{
        run = $Run
        kind = 'burn'
        path = $Path
        text = if (Test-Path -LiteralPath $Path -PathType Leaf) {
            Read-WixInstallerScenarioSharedLog $Path
        }
        else {
            ''
        }
    }
}

function Invoke-WixInstallerScenarioUpgradeOrReentry {
    param(
        [Parameter(Mandatory = $true)]$Preflight,
        [Parameter(Mandatory = $true)]$Fixtures,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][int]$MappedModuleHoldSeconds
    )

    Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'baseline' -Status 'reading'
    Initialize-WixInstallerScenarioState -FixtureExe $Fixtures.state_fixture_exe -OutputDir $OutputDir | Out-Null
    $before = Get-WixInstallerScenarioSnapshot -StateFixture $Fixtures.state_fixture_exe -OutputDir $OutputDir
    if (-not (Test-WixInstallerScenarioSnapshotMatches `
        -Snapshot $before -Identity $Preflight.baseline `
        -RequireConsistentBundle -RequireConsistentPayload)) {
        Write-WixInstallerScenarioJson (Join-Path $OutputDir 'baseline-snapshot.json') $before
        throw '[SC041] The owned guest is not in the trusted baseline state.'
    }
    Write-WixInstallerScenarioJson (Join-Path $OutputDir 'before.json') $before

    $oldDll = Join-Path (Get-WixInstallerScenarioProductRoot) (
        'versions\' + $Preflight.baseline.runtime_version + '\x64\BoshiamyTsf.dll'
    )
    Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'mapped_module' -Status 'starting'
    $mapped = Start-WixInstallerScenarioMappedModule -FixtureExe $Fixtures.mapped_module_exe `
        -DllPath $oldDll -OutputDir $OutputDir -HoldSeconds $MappedModuleHoldSeconds
    Write-WixInstallerScenarioJson (Join-Path $OutputDir 'mapped-module.json') (
        $mapped | Select-Object -Property * -ExcludeProperty process
    )

    $firstBurnPath = Join-Path $OutputDir 'first-burn.log'
    Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'upgrade' -Status 'starting_candidate_passive'
    $started = Start-WixInstallerScenarioSetup -SetupPath $Preflight.candidate.setup_path `
        -WorkingDirectory (Split-Path -Parent $Preflight.candidate.setup_path) `
        -BurnLog $firstBurnPath -Passive
    $second = $null
    if ($Preflight.scenario -ceq 'Reentry') {
        $apply = Wait-WixInstallerScenarioMarker -Path $firstBurnPath `
            -Pattern 'Applying execute package:\s+LiuKaiMsi' -TimeoutSeconds $TimeoutSeconds `
            -OutputDir $OutputDir -Phase 'reentry'
        if ($apply.observed -and -not $started.process.HasExited) {
            $secondBurnPath = Join-Path $OutputDir 'second-burn.log'
            Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'reentry' `
                -Status 'starting_one_second_candidate' -Detail @{
                    first_pid = $started.pid
                }
            $second = Start-WixInstallerScenarioSetup -SetupPath $Preflight.candidate.setup_path `
                -WorkingDirectory (Split-Path -Parent $Preflight.candidate.setup_path) `
                -BurnLog $secondBurnPath -Passive
        }
        else {
            Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'reentry' `
                -Status 'not_covered_no_first_apply'
        }
    }
    $firstWait = Wait-WixInstallerScenarioProcess -Process $started.process `
        -TimeoutSeconds $TimeoutSeconds -OutputDir $OutputDir -Phase 'upgrade'
    $secondWait = if ($null -ne $second) {
        Wait-WixInstallerScenarioProcess -Process $second.process `
            -TimeoutSeconds ([Math]::Min($TimeoutSeconds, 120)) `
            -OutputDir $OutputDir -Phase 'reentry'
    }
    else {
        $null
    }
    Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'post_install' -Status 'reading'
    $after = Get-WixInstallerScenarioSnapshot -StateFixture $Fixtures.state_fixture_exe -OutputDir $OutputDir
    Write-WixInstallerScenarioJson (Join-Path $OutputDir 'after.json') $after

    $burnLogs = @(
        New-WixInstallerScenarioBurnLogEntry -Path $firstBurnPath -Run first
    )
    $secondBurnPath = Join-Path $OutputDir 'second-burn.log'
    if ($null -ne $second) {
        $burnLogs += New-WixInstallerScenarioBurnLogEntry -Path $secondBurnPath -Run second
    }
    $startedUtc = [DateTimeOffset]::Parse($started.started_utc).UtcDateTime
    $endedUtc = if ($firstWait.exited) {
        [DateTimeOffset]::Parse($firstWait.ended_utc).UtcDateTime.AddSeconds(10)
    }
    else {
        [DateTime]::UtcNow
    }
    $secondaryBurn = if ($null -ne $second) { $secondBurnPath } else { '' }
    $msiCollection = Get-WixInstallerScenarioMsiLogEntries `
        -StartedUtc $startedUtc -EndedUtc $endedUtc `
        -ExplicitBurnLog $firstBurnPath -Run first `
        -SecondaryBurnLog $secondaryBurn
    $evidenceLogs = @(
        $burnLogs + @($msiCollection.entries | Where-Object { $_.run -ne 'unattributed' })
    )
    $rawReceipt = Copy-WixInstallerScenarioRawLogs -Logs $evidenceLogs -OutputDir $OutputDir
    $readback = New-WixInstallerScenarioReadback -Before $before -After $after -Preflight $Preflight `
        -TargetedReentryRetries 0
    $evidence = ConvertFrom-WixInstallerScenarioLogs `
        -Scenario $Preflight.scenario -BurnLogs @($burnLogs) `
        -MsiLogs @($msiCollection.entries | Where-Object { $_.run -ne 'unattributed' }) `
        -Readback $readback -EvidenceClass runtime_observed
    $evidence | Add-Member -NotePropertyName unattributed_msi_transactions `
        -NotePropertyValue @($msiCollection.unattributed_paths).Count
    $evidence | Add-Member -NotePropertyName first_process -NotePropertyValue @{
        pid = $started.pid
        exited = $firstWait.exited
        exit_code = $firstWait.exit_code
        arguments = @($started.arguments)
    }
    $evidence | Add-Member -NotePropertyName second_process -NotePropertyValue $(
        if ($null -ne $second) {
            @{
                pid = $second.pid
                exited = $secondWait.exited
                exit_code = $secondWait.exit_code
                arguments = @($second.arguments)
            }
        }
        else {
            $null
        }
    )
    $verdict = Get-WixInstallerScenarioVerdict -Scenario $Preflight.scenario -Evidence $evidence
    $rootGuestRebootRequired = $evidence.restart_required -or
        ($firstWait.exited -and $firstWait.exit_code -eq 3010)
    if ($rootGuestRebootRequired -and $verdict.actual_scenario_pass) {
        $verdict.verdict = 'NOT_COVERED'
        $verdict.receipt_status = 'RUNTIME_OBSERVED_NOT_COVERED'
        $verdict.actual_scenario_pass = $false
        $verdict.reason = 'The install committed with restart required; Root must complete the guest-only reboot phase before final readback.'
    }
    return [pscustomobject]@{
        format = 'liukai-installer-scenario-receipt-v1'
        scenario = $Preflight.scenario
        verdict = $verdict
        evidence = $evidence
        mapped_module = $mapped | Select-Object -Property * -ExcludeProperty process
        raw_log_copies = @($rawReceipt)
        root_guest_reboot_required = [bool]$rootGuestRebootRequired
        unattributed_msi_paths = @($msiCollection.unattributed_paths)
        unexpected_process_live = (-not $firstWait.exited) -or
            ($null -ne $secondWait -and -not $secondWait.exited)
    }
}

function Get-WixInstallerScenarioLiveLogs {
    param(
        [Parameter(Mandatory = $true)][DateTime]$StartedUtc,
        [string]$ExplicitBurnLog = ''
    )

    $logs = @()
    $dummy = if ([string]::IsNullOrWhiteSpace($ExplicitBurnLog)) {
        Join-Path $env:TEMP '.installer-scenario-no-explicit-burn.log'
    }
    else {
        $ExplicitBurnLog
    }
    $files = @(
        Get-WixInstallerScenarioDiscoveredLogFiles `
            -StartedUtc $StartedUtc `
            -ExplicitBurnLog $dummy
    )
    if (-not [string]::IsNullOrWhiteSpace($ExplicitBurnLog) -and
        (Test-Path -LiteralPath $ExplicitBurnLog -PathType Leaf)) {
        foreach ($path in @(Get-WixInstallerScenarioMsiPathsFromBurn (
            Read-WixInstallerScenarioSharedLog $ExplicitBurnLog
        ))) {
            $files += Get-Item -LiteralPath $path
        }
    }
    foreach ($file in @($files | Sort-Object FullName -Unique)) {
        try {
            $text = Read-WixInstallerScenarioSharedLog $file.FullName
        }
        catch [IO.IOException] {
            continue
        }
        $kind = if ([regex]::IsMatch(
            $text,
            '(?im)Burn(?:\s+(?:x86|x64|arm64))?\s+v[0-9]'
        )) {
            'burn'
        }
        elseif ([regex]::IsMatch($text, '(?i)Action start[^\r\n]*Install|=== Verbose logging started|MSI \(s\)')) {
            'msi'
        }
        else {
            continue
        }
        $logs += [pscustomobject]@{
            run = 'recovery'
            kind = $kind
            path = $file.FullName
            text = $text
        }
    }
    return @($logs)
}

function Get-WixInstallerScenarioSingleBaTarget {
    param(
        [Parameter(Mandatory = $true)][int]$RootPid,
        [Parameter(Mandatory = $true)][string]$RootCreationTimeUtc,
        [Parameter(Mandatory = $true)]$RootProcess,
        [Parameter(Mandatory = $true)][string]$SetupHash,
        [Parameter(Mandatory = $true)][string[]]$TrustedBaHashes,
        [switch]$AllowElevatedBaUi
    )

    $observations = @(
        Get-WixInstallerScenarioProcessTree `
            -RootPid $RootPid `
            -RootCreationTimeUtc $RootCreationTimeUtc `
            -RootProcess $RootProcess `
            -SetupHash $SetupHash `
            -TrustedBaHashes $TrustedBaHashes
    )
    $eligible = @($observations | Where-Object {
        (Test-WixInstallerScenarioKillEligible -ExpectedTarget $_ -ObservedProcess $_ `
            -AllowElevatedBaUi:$AllowElevatedBaUi).eligible -and
        $_.alive -eq $true -and
        $_.root_pid -eq $RootPid -and
        $_.root_creation_time_utc -ceq $RootCreationTimeUtc
    })
    $target = if ($eligible.Count -eq 1) { $eligible[0] } else { $null }
    $targetProcess = if ($null -ne $target) {
        Get-Process -Id ([int]$target.pid) -ErrorAction SilentlyContinue
    }
    else {
        $null
    }
    if ($null -ne $targetProcess) {
        try {
            [void]$targetProcess.Handle
            if ($targetProcess.HasExited -or $targetProcess.Id -ne [int]$target.pid -or
                $targetProcess.StartTime.ToUniversalTime().ToString('o') -cne
                    [string]$target.creation_time_utc) {
                $targetProcess.Dispose(); $targetProcess = $null; $target = $null
            }
        }
        catch [System.ComponentModel.Win32Exception] {
            $targetProcess.Dispose(); $targetProcess = $null; $target = $null
        }
        catch [InvalidOperationException] {
            $targetProcess.Dispose(); $targetProcess = $null; $target = $null
        }
    }
    return [pscustomobject]@{
        observations = @($observations)
        target = $target
        target_process = $targetProcess
        ambiguous = $eligible.Count -gt 1
    }
}

function Wait-WixInstallerScenarioRecoveryCommitTarget {
    param(
        [Parameter(Mandatory = $true)]$StartedUtc,
        [Parameter(Mandatory = $true)][int]$RootPid,
        [Parameter(Mandatory = $true)][string[]]$TrustedBaHashes,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [Parameter(Mandatory = $true)][int]$Attempt,
        [Parameter(Mandatory = $true)][string]$ExplicitBurnLog,
        [Parameter(Mandatory = $true)]$RootProcess,
        [Parameter(Mandatory = $true)][string]$RootCreationTimeUtc,
        [Parameter(Mandatory = $true)][string]$SetupHash,
        [Parameter(Mandatory = $true)][string]$ExpectedMsiVersion,
        [switch]$AllowElevatedBaUi
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastMsiLogs = @()
    do {
        $liveLogs = @(
            Get-WixInstallerScenarioLiveLogs -StartedUtc $StartedUtc `
                -ExplicitBurnLog $ExplicitBurnLog
        )
        $lastMsiLogs = @($liveLogs | Where-Object { $_.kind -ceq 'msi' } | ForEach-Object {
            [pscustomobject]@{
                run = 'first'
                kind = 'msi'
                path = $_.path
                text = $_.text
            }
        })
        $evidence = ConvertFrom-WixInstallerScenarioLogs -Scenario Recovery `
            -BurnLogs @() -MsiLogs $lastMsiLogs -Readback ([pscustomobject]@{
                expected_msi_version = $ExpectedMsiVersion
            }) `
            -EvidenceClass runtime_observed
        $tree = Get-WixInstallerScenarioSingleBaTarget `
            -RootPid $RootPid `
            -RootCreationTimeUtc $RootCreationTimeUtc `
            -RootProcess $RootProcess `
            -SetupHash $SetupHash `
            -TrustedBaHashes $TrustedBaHashes -AllowElevatedBaUi:$AllowElevatedBaUi
        if ($evidence.first_msi_commits -gt 0 -and $null -ne $tree.target) {
            Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'recovery_inject' `
                -Status 'msi_committed_ba_identity_available' -Detail @{
                    attempt = $Attempt
                    target = $tree.target
                }
            return [pscustomobject]@{
                commit_observed = $true
                target = $tree.target
                target_process = $tree.target_process
                root_process = $RootProcess
                root_creation_time_utc = $RootCreationTimeUtc
                process_observations = @($tree.observations)
                msi_logs = @($lastMsiLogs)
                all_logs = @($liveLogs)
                restart_required = [bool]$evidence.restart_required
                ambiguous_target = $false
            }
        }
        if ($evidence.first_msi_commits -gt 0 -and $tree.ambiguous) {
            return [pscustomobject]@{
                commit_observed = $true
                target = $null
                target_process = $null
                root_process = $RootProcess
                root_creation_time_utc = $RootCreationTimeUtc
                process_observations = @($tree.observations)
                msi_logs = @($lastMsiLogs)
                all_logs = @($liveLogs)
                restart_required = [bool]$evidence.restart_required
                ambiguous_target = $true
            }
        }
        if ($null -ne $tree.target_process) { $tree.target_process.Dispose() }
        $RootProcess.Refresh()
        if ($RootProcess.HasExited) {
            return [pscustomobject]@{
                commit_observed = ($evidence.first_msi_commits -gt 0)
                target = $null
                target_process = $null
                root_process = $RootProcess
                root_creation_time_utc = $RootCreationTimeUtc
                process_observations = @($tree.observations)
                msi_logs = @($lastMsiLogs)
                all_logs = @($liveLogs)
                restart_required = [bool]$evidence.restart_required
                ambiguous_target = $false
            }
        }
        Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'recovery_inject' `
            -Status 'waiting_for_root_ui_and_msi_commit' -Detail @{
                attempt = $Attempt
                root_pid = $RootPid
                manual_action = 'Operate the ordinary candidate UI; do not pass installer arguments or automate clicks.'
            }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    return [pscustomobject]@{
        commit_observed = $false
        target = $null
        target_process = $null
        root_process = $RootProcess
        root_creation_time_utc = $RootCreationTimeUtc
        process_observations = @()
        msi_logs = @($lastMsiLogs)
        all_logs = @(
            Get-WixInstallerScenarioLiveLogs -StartedUtc $StartedUtc `
                -ExplicitBurnLog $ExplicitBurnLog
        )
        restart_required = $false
        ambiguous_target = $false
        timed_out = $true
    }
}

function Stop-WixInstallerScenarioVerifiedBa {
    param(
        [Parameter(Mandatory = $true)]$ExpectedTarget,
        [Parameter(Mandatory = $true)]$HeldTargetProcess,
        [Parameter(Mandatory = $true)]$RootProcess,
        [Parameter(Mandatory = $true)][string]$SetupHash,
        [Parameter(Mandatory = $true)][string[]]$TrustedBaHashes,
        [switch]$AllowElevatedBaUi
    )

    $HeldTargetProcess.Refresh()
    if ($HeldTargetProcess.HasExited -or
        $HeldTargetProcess.Id -ne [int]$ExpectedTarget.pid -or
        $HeldTargetProcess.StartTime.ToUniversalTime().ToString('o') -cne
            [string]$ExpectedTarget.creation_time_utc) {
        return [pscustomobject]@{
            stopped = $false
            reason = 'Held BA process handle no longer represents the captured target.'
            observed = $null
        }
    }
    $tree = @(
        Get-WixInstallerScenarioProcessTree `
            -RootPid ([int]$ExpectedTarget.root_pid) `
            -RootCreationTimeUtc ([string]$ExpectedTarget.root_creation_time_utc) `
            -RootProcess $RootProcess `
            -SetupHash $SetupHash `
            -TrustedBaHashes $TrustedBaHashes
    )
    $fresh = @($tree | Where-Object { [int]$_.pid -eq [int]$ExpectedTarget.pid })
    if ($fresh.Count -ne 1) {
        return [pscustomobject]@{
            stopped = $false
            reason = 'The BA target is no longer under the freshly verified candidate process tree.'
            observed = $null
        }
    }
    $fresh = $fresh[0]
    $decision = Test-WixInstallerScenarioKillEligible `
        -ExpectedTarget $ExpectedTarget `
        -ObservedProcess $fresh -AllowElevatedBaUi:$AllowElevatedBaUi
    if ($fresh.root_pid -ne $ExpectedTarget.root_pid -or
        [string]$fresh.root_creation_time_utc -cne [string]$ExpectedTarget.root_creation_time_utc) {
        $decision = [pscustomobject]@{
            eligible = $false
            reason = 'The captured BA ancestry changed.'
        }
    }
    if (-not $decision.eligible) {
        return [pscustomobject]@{
            stopped = $false
            reason = $decision.reason
            observed = $fresh
        }
    }
    $finalTree = @(
        Get-WixInstallerScenarioProcessTree `
            -RootPid ([int]$ExpectedTarget.root_pid) `
            -RootCreationTimeUtc ([string]$ExpectedTarget.root_creation_time_utc) `
            -RootProcess $RootProcess `
            -SetupHash $SetupHash `
            -TrustedBaHashes $TrustedBaHashes
    )
    $final = @($finalTree | Where-Object { [int]$_.pid -eq [int]$ExpectedTarget.pid })
    if ($final.Count -ne 1 -or
        -not (Test-WixInstallerScenarioKillEligible `
            -ExpectedTarget $ExpectedTarget -ObservedProcess $final[0] `
            -AllowElevatedBaUi:$AllowElevatedBaUi).eligible) {
        return [pscustomobject]@{
            stopped = $false
            reason = 'Final BA identity, elevation, or ancestry changed before the stop.'
            observed = $final
        }
    }
    $final = $final[0]
    $HeldTargetProcess.Refresh()
    if ($HeldTargetProcess.HasExited -or
        $HeldTargetProcess.StartTime.ToUniversalTime().ToString('o') -cne
            [string]$ExpectedTarget.creation_time_utc) {
        return [pscustomobject]@{
            stopped = $false
            reason = 'Held BA process changed during the final identity check.'
            observed = $final
        }
    }
    Ensure-WixInstallerScenarioProcessNative
    $nativeError = 0
    $expectedCreated = [DateTimeOffset]::Parse(
        [string]$ExpectedTarget.creation_time_utc).UtcDateTime.ToFileTimeUtc()
    if (-not [WixInstallerScenarioProcessNative]::TryTerminateVerifiedHandle(
            $HeldTargetProcess.Handle, [int]$ExpectedTarget.pid, $expectedCreated,
            23063, [ref]$nativeError)) {
        return [pscustomobject]@{
            stopped = $false
            reason = "Held BA native identity check or stop failed: $nativeError."
            observed = $final
        }
    }
    $exited = $HeldTargetProcess.WaitForExit(5000)
    return [pscustomobject]@{
        stopped = [bool]$exited
        reason = if ($exited) { 'Stopped exactly the reverified candidate BA/UI process.' } else { 'BA stop did not exit within five seconds.' }
        observed = $fresh
        expected = $ExpectedTarget
        exit_code = if ($exited) { $HeldTargetProcess.ExitCode } else { $null }
    }
}

function Reset-WixInstallerScenarioToBaseline {
    param(
        [Parameter(Mandatory = $true)]$Preflight,
        [Parameter(Mandatory = $true)]$Fixtures,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $state = Get-WixInstallerScenarioSnapshot -StateFixture $Fixtures.state_fixture_exe -OutputDir $OutputDir
    if (Test-WixInstallerScenarioSnapshotMatches `
        -Snapshot $state -Identity $Preflight.baseline `
        -RequireConsistentBundle -RequireConsistentPayload) {
        return [pscustomobject]@{ ready = $true; reboot_required = $false; snapshot = $state }
    }
    if ($state.installed) {
        Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase $Label `
            -Status 'manual_normal_uninstall_required' -Detail @{
                manual_action = 'Use the ordinary candidate EXE UI to uninstall; the harness passes zero installer arguments.'
            }
        $uninstall = Start-WixInstallerScenarioSetup -SetupPath $Preflight.candidate.setup_path `
            -WorkingDirectory (Split-Path -Parent $Preflight.candidate.setup_path)
        $uninstallWait = Wait-WixInstallerScenarioProcess -Process $uninstall.process `
            -TimeoutSeconds $TimeoutSeconds -OutputDir $OutputDir -Phase $Label
        if (-not $uninstallWait.exited) {
            return [pscustomobject]@{
                ready = $false
                reboot_required = $false
                live_pid = $uninstall.pid
                snapshot = $state
            }
        }
        $state = Get-WixInstallerScenarioSnapshot -StateFixture $Fixtures.state_fixture_exe -OutputDir $OutputDir
        if (-not (Test-WixInstallerScenarioInstalledAbsent $state)) {
            return [pscustomobject]@{
                ready = $false
                reboot_required = $false
                snapshot = $state
                uninstall_exit_code = $uninstallWait.exit_code
            }
        }
    }
    $baselineBurn = Join-Path $OutputDir ($Label + '-baseline-burn.log')
    Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase $Label `
        -Status 'reinstalling_baseline_passive'
    $baseline = Start-WixInstallerScenarioSetup -SetupPath $Preflight.baseline.setup_path `
        -WorkingDirectory (Split-Path -Parent $Preflight.baseline.setup_path) `
        -BurnLog $baselineBurn -Passive
    $baselineWait = Wait-WixInstallerScenarioProcess -Process $baseline.process `
        -TimeoutSeconds $TimeoutSeconds -OutputDir $OutputDir -Phase $Label
    $state = Get-WixInstallerScenarioSnapshot -StateFixture $Fixtures.state_fixture_exe -OutputDir $OutputDir
    $ready = $baselineWait.exited -and (Test-WixInstallerScenarioSnapshotMatches `
        -Snapshot $state -Identity $Preflight.baseline `
        -RequireConsistentBundle -RequireConsistentPayload)
    return [pscustomobject]@{
        ready = [bool]$ready
        reboot_required = $baselineWait.exited -and $baselineWait.exit_code -eq 3010
        process_exit_code = $baselineWait.exit_code
        snapshot = $state
        burn_log = $baselineBurn
    }
}

function Invoke-WixInstallerScenarioRecovery {
    param(
        [Parameter(Mandatory = $true)]$Preflight,
        [Parameter(Mandatory = $true)]$Fixtures,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [switch]$AllowElevatedBaUi,
        [ValidateRange(1, 2)][int]$MaxAttempts = 2,
        [switch]$AcceptRetainedBundle
    )

    Initialize-WixInstallerScenarioState -FixtureExe $Fixtures.state_fixture_exe -OutputDir $OutputDir | Out-Null
    $before = Get-WixInstallerScenarioSnapshot -StateFixture $Fixtures.state_fixture_exe -OutputDir $OutputDir
    if (-not (Test-WixInstallerScenarioSnapshotMatches `
        -Snapshot $before -Identity $Preflight.baseline `
        -RequireConsistentBundle -RequireConsistentPayload)) {
        Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'recovery_prepare' `
            -Status 'serial_candidate_uninstall_and_baseline_restore'
        $preparation = Reset-WixInstallerScenarioToBaseline -Preflight $Preflight -Fixtures $Fixtures `
            -OutputDir $OutputDir -TimeoutSeconds $TimeoutSeconds -Label 'recovery-prepare'
        Write-WixInstallerScenarioJson (Join-Path $OutputDir 'preparation.json') (
            $preparation | Select-Object -Property * -ExcludeProperty snapshot
        )
        if (-not $preparation.ready -or $preparation.reboot_required) {
            $readback = New-WixInstallerScenarioReadback `
                -Before $before -After $preparation.snapshot -Preflight $Preflight
            $evidence = ConvertFrom-WixInstallerScenarioLogs -Scenario Recovery `
                -BurnLogs @() -MsiLogs @() -Readback $readback -EvidenceClass runtime_observed
            $verdict = Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $evidence
            return [pscustomobject]@{
                format = 'liukai-installer-scenario-receipt-v1'
                scenario = 'Recovery'
                verdict = $verdict
                evidence = $evidence
                preparation = $preparation
                root_guest_reboot_required = [bool]$preparation.reboot_required
                unexpected_process_live = $null -ne (Get-WixInstallerScenarioProperty $preparation 'live_pid')
            }
        }
        $before = $preparation.snapshot
    }
    Write-WixInstallerScenarioJson (Join-Path $OutputDir 'before.json') $before
    $trustedBaHashes = @(Get-WixInstallerScenarioTrustedBaHashes $Preflight.candidate)
    $scenarioStartedUtc = [DateTime]::UtcNow
    $attempts = 0
    $formed = $null
    $allAttemptLogs = @()
    $attemptReceipts = @()
    $unexpectedProcessPid = $null

    for ($attempt = 1; $attempt -le $MaxAttempts -and $null -eq $formed; $attempt++) {
        $attempts++
        Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'recovery_attempt' `
            -Status 'starting_normal_candidate_ui' -Detail @{
                attempt = $attempt
                manual_action = 'Operate the ordinary candidate Install UI. Only an explicit Burn /log path is supplied.'
            }
        $injectionBurn = Join-Path $OutputDir ("recovery-$attempt-burn.log")
        $started = Start-WixInstallerScenarioSetup -SetupPath $Preflight.candidate.setup_path `
            -WorkingDirectory (Split-Path -Parent $Preflight.candidate.setup_path) `
            -BurnLog $injectionBurn
        $observation = Wait-WixInstallerScenarioRecoveryCommitTarget `
            -StartedUtc ([DateTimeOffset]::Parse($started.started_utc).UtcDateTime) `
            -RootPid $started.pid -TrustedBaHashes $trustedBaHashes `
            -TimeoutSeconds $TimeoutSeconds -OutputDir $OutputDir -Attempt $attempt `
            -ExplicitBurnLog $injectionBurn `
            -RootProcess $started.process `
            -RootCreationTimeUtc $started.creation_time_utc `
            -SetupHash $Preflight.candidate.setup_sha256 `
            -ExpectedMsiVersion $Preflight.candidate.msi_version `
            -AllowElevatedBaUi:$AllowElevatedBaUi
        $attemptLogs = @($observation.all_logs)
        $firstObservation = $observation | Select-Object -Property * -ExcludeProperty target_process, root_process
        $stop = $null
        $interrupted = $null
        $bundleConsistent = $null
        $bundleRegistryVerified = $null
        $interruptedProcess = $null
        $postExit = $null
        if ($observation.commit_observed -and $null -ne $observation.target -and
            $null -ne $observation.target_process) {
            $stop = Stop-WixInstallerScenarioVerifiedBa `
                -ExpectedTarget $observation.target `
                -HeldTargetProcess $observation.target_process `
                -RootProcess $observation.root_process `
                -SetupHash $Preflight.candidate.setup_sha256 `
                -TrustedBaHashes $trustedBaHashes -AllowElevatedBaUi:$AllowElevatedBaUi
            if ($stop.stopped) {
                $interruptedProcess = Wait-WixInstallerScenarioProcess -Process $started.process `
                    -TimeoutSeconds $TimeoutSeconds -OutputDir $OutputDir -Phase 'recovery_original_engine_exit'
                if (-not $interruptedProcess.exited) {
                    $unexpectedProcessPid = $started.pid
                }
                else {
                    $attemptLogs = @(Get-WixInstallerScenarioLiveLogs `
                        -StartedUtc ([DateTimeOffset]::Parse($started.started_utc).UtcDateTime) `
                        -ExplicitBurnLog $injectionBurn)
                    $postExitMsiLogs = @($attemptLogs | Where-Object { $_.kind -ceq 'msi' } | ForEach-Object {
                        [pscustomobject]@{ run = 'first'; kind = 'msi'; path = $_.path; text = $_.text }
                    })
                    $postExitBurnLogs = @($attemptLogs | Where-Object { $_.kind -ceq 'burn' } | ForEach-Object {
                        [pscustomobject]@{ run = 'first'; kind = 'burn'; path = $_.path; text = $_.text }
                    })
                    $postExitEvidence = ConvertFrom-WixInstallerScenarioLogs -Scenario Recovery `
                        -BurnLogs $postExitBurnLogs -MsiLogs $postExitMsiLogs `
                        -Readback ([pscustomobject]@{ expected_msi_version = $Preflight.candidate.msi_version }) `
                        -EvidenceClass runtime_observed
                    $postExit = [pscustomobject]@{
                        collected_utc = [DateTime]::UtcNow.ToString('o')
                        explicit_burn_log = $injectionBurn
                        evidence = $postExitEvidence
                        state = Get-WixInstallerScenarioRecoveryPostExitState -Evidence $postExitEvidence
                        msi_logs = @($postExitMsiLogs)
                    }
                    Write-WixInstallerScenarioJson (
                        Join-Path $OutputDir ("recovery-$attempt-post-exit.json")) $postExit
                }
                Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'recovery_attempt' `
                    -Status 'verified_ba_stopped_reading_state' -Detail @{
                        attempt = $attempt
                        target = $observation.target
                    }
                $interrupted = Get-WixInstallerScenarioSnapshot `
                    -StateFixture $Fixtures.state_fixture_exe -OutputDir $OutputDir
                Write-WixInstallerScenarioJson (
                    Join-Path $OutputDir ("recovery-$attempt-interrupted.json")) $interrupted
                $bundleRegistryVerified = @($interrupted.uninstall_registry_issues).Count -eq 0
                $bundleConsistent = if ($bundleRegistryVerified) {
                    Test-WixInstallerScenarioBundleMatches `
                        -Snapshot $interrupted -Identity $Preflight.candidate
                }
                else {
                    $null
                }
                if ($interruptedProcess.exited -and $null -ne $postExit -and $postExit.state.known -and
                    (Test-WixInstallerScenarioSnapshotMatches `
                    -Snapshot $interrupted -Identity $Preflight.candidate) -and
                    $bundleRegistryVerified -and ($bundleConsistent -eq $false -or
                        ($AcceptRetainedBundle -and $bundleConsistent -eq $true))) {
                    $formed = [pscustomobject]@{
                        attempt = $attempt
                        started = $started
                        observation = $observation
                        stop = $stop
                        snapshot = $interrupted
                        original_process = $interruptedProcess
                        post_exit = $postExit
                        bundle_consistent = $bundleConsistent
                    }
                    Write-WixInstallerScenarioJson (Join-Path $OutputDir 'formed-state.json') ([ordered]@{
                        attempt = $formed.attempt
                        target = $formed.observation.target
                        stop = $formed.stop
                        snapshot = $formed.snapshot
                        original_process = $formed.original_process
                        post_exit = $formed.post_exit
                        bundle_consistent = $formed.bundle_consistent
                    })
                }
            }
            else {
                $observation.target_process.Refresh()
                if (-not $observation.target_process.HasExited) {
                    $unexpectedProcessPid = $observation.target.pid
                }
            }
        }
        Write-WixInstallerScenarioJson (
            Join-Path $OutputDir ("recovery-$attempt-first-observation.json")) $firstObservation
        $allAttemptLogs += @($attemptLogs)
        $attemptReceipts += [pscustomobject]@{
            attempt = $attempt
            candidate_pid = $started.pid
            arguments = @($started.arguments)
            commit_observed = $observation.commit_observed
            target_observed = $null -ne $observation.target
            ambiguous_target = $observation.ambiguous_target
            stop = $stop
            original_process = $interruptedProcess
            first_observation = $firstObservation
            post_exit = $postExit
            bundle_consistent_at_interrupt = $bundleConsistent
            bundle_registry_verified = $bundleRegistryVerified
            timed_out = [bool](Get-WixInstallerScenarioProperty $observation 'timed_out' $false)
            formed = $null -ne $formed -and $formed.attempt -eq $attempt
        }
        Write-WixInstallerScenarioJson (Join-Path $OutputDir 'recovery-attempts.json') $attemptReceipts
        if ($null -ne $formed) {
            break
        }
        if ((Get-WixInstallerScenarioProperty $observation 'timed_out' $false)) {
            $unexpectedProcessPid = $started.pid
            break
        }
        if ($null -ne $unexpectedProcessPid) {
            break
        }
        if ($null -ne $postExit -and -not $postExit.state.known) {
            Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'recovery_post_exit' `
                -Status 'candidate_native_result_unknown'
            break
        }
        if ($attempt -lt $MaxAttempts) {
            $reset = Reset-WixInstallerScenarioToBaseline -Preflight $Preflight -Fixtures $Fixtures `
                -OutputDir $OutputDir -TimeoutSeconds $TimeoutSeconds -Label ("retry-$attempt")
            if (-not $reset.ready) {
                $unexpectedProcessPid = Get-WixInstallerScenarioProperty $reset 'live_pid'
                Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'recovery_retry' `
                    -Status $(if ($reset.reboot_required) {
                        'root_guest_reboot_required'
                    }
                    else {
                        'not_covered_reset_failed'
                    })
                break
            }
            if ($reset.reboot_required) {
                Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'recovery_retry' `
                    -Status 'root_guest_reboot_required'
                break
            }
            $before = $reset.snapshot
        }
    }

    $rerun = $null
    $rerunWait = $null
    $uninstall = $null
    $uninstallWait = $null
    $converged = $null
    $final = $null
    $injectionOrigin = ''
    if ($null -ne $formed) {
        $injectionOrigin = 'candidate_ba_ui'
    }
    if ($null -ne $formed -and
        -not $formed.post_exit.state.restart_required) {
        Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'recovery_normal_rerun' `
            -Status 'manual_normal_ui_required' -Detail @{
                manual_action = 'Double-click the same candidate and complete the ordinary UI with no passive, repair, or hidden arguments.'
            }
        $rerun = Start-WixInstallerScenarioSetup -SetupPath $Preflight.candidate.setup_path `
            -WorkingDirectory (Split-Path -Parent $Preflight.candidate.setup_path)
        $rerunWait = Wait-WixInstallerScenarioProcess -Process $rerun.process `
            -TimeoutSeconds $TimeoutSeconds -OutputDir $OutputDir -Phase 'recovery_normal_rerun'
        if ($rerunWait.exited) {
            $converged = Get-WixInstallerScenarioSnapshot `
                -StateFixture $Fixtures.state_fixture_exe -OutputDir $OutputDir
            Write-WixInstallerScenarioJson (Join-Path $OutputDir 'converged.json') $converged
        }
        if ($null -ne $converged -and (Test-WixInstallerScenarioSnapshotMatches `
            -Snapshot $converged -Identity $Preflight.candidate `
            -RequireConsistentBundle -RequireConsistentPayload) -and $rerunWait.exit_code -eq 0) {
            Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'recovery_uninstall' `
                -Status 'manual_normal_ui_required' -Detail @{
                    manual_action = 'Use the ordinary same-candidate EXE UI to uninstall; the harness passes zero arguments.'
                }
            $uninstall = Start-WixInstallerScenarioSetup -SetupPath $Preflight.candidate.setup_path `
                -WorkingDirectory (Split-Path -Parent $Preflight.candidate.setup_path)
            $uninstallWait = Wait-WixInstallerScenarioProcess -Process $uninstall.process `
                -TimeoutSeconds $TimeoutSeconds -OutputDir $OutputDir -Phase 'recovery_uninstall'
            if ($uninstallWait.exited) {
                $final = Get-WixInstallerScenarioSnapshot `
                    -StateFixture $Fixtures.state_fixture_exe -OutputDir $OutputDir
                Write-WixInstallerScenarioJson (Join-Path $OutputDir 'after-uninstall.json') $final
            }
        }
    }
    elseif ($null -ne $formed) {
        Write-WixInstallerScenarioProgress -OutputDir $OutputDir -Phase 'recovery_guest_reboot' `
            -Status 'root_guest_reboot_required' -Detail @{
                manual_action = 'Root must restart this owned guest, then rerun the same candidate through normal UI with zero arguments.'
            }
    }
    $convergedValid = if ($null -eq $converged) {
        $null
    }
    else {
        Test-WixInstallerScenarioSnapshotMatches `
            -Snapshot $converged -Identity $Preflight.candidate `
            -RequireConsistentBundle -RequireConsistentPayload
    }
    $uninstallSucceeded = if ($null -eq $final) {
        $null
    }
    else {
        Test-WixInstallerScenarioInstalledAbsent $final
    }
    $readbackSource = if ($null -ne $converged) {
        $converged
    }
    elseif ($null -ne $formed) {
        $formed.snapshot
    }
    else {
        $before
    }
    $readback = New-WixInstallerScenarioReadback -Before $before -After $readbackSource -Preflight $Preflight `
        -InjectionAttempts $attempts -InjectionOrigin $injectionOrigin `
        -BundleConsistentAtInterrupt $(if ($null -ne $formed) { $formed.bundle_consistent } else { $null }) `
        -NormalUiRerunMode $(if ($null -ne $rerun) { 'normal_double_click' } else { '' }) `
        -RerunArguments $(if ($null -ne $rerun) { @($rerun.arguments) } else { @() }) `
        -RerunConverged ([bool]$convergedValid) `
        -UninstallSucceeded ([bool]$uninstallSucceeded)
    if ($null -ne $formed) {
        $readback | Add-Member -NotePropertyName verified_ba_stop -NotePropertyValue ([bool]$formed.stop.stopped)
        $readback | Add-Member -NotePropertyName original_engine_exited -NotePropertyValue ([bool]$formed.original_process.exited)
    }
    if ((Test-WixInstallerScenarioProperty $readback 'rerun_converged')) {
        $readback.rerun_converged = $convergedValid
    }
    if ((Test-WixInstallerScenarioProperty $readback 'uninstall_succeeded')) {
        $readback.uninstall_succeeded = $uninstallSucceeded
    }
    if ($null -ne $formed -and $null -eq $rerun) {
        foreach ($unobserved in @(
            'normal_ui_rerun_mode',
            'rerun_converged',
            'uninstall_succeeded',
            'observed_runtime_version', 'observed_payload_version', 'observed_com_version',
            'manifest_integrity_ok', 'registration_status_ok', 'new_bundle_registered',
            'old_bundle_and_msi_registrations_absent'
        )) {
            $readback.PSObject.Properties.Remove($unobserved) | Out-Null
        }
    }
    if ($null -ne $rerunWait) {
        $readback | Add-Member -NotePropertyName normal_rerun_process -NotePropertyValue (
            [pscustomobject]@{
                exited = $rerunWait.exited
                exit_code = $rerunWait.exit_code
            }
        )
    }
    if ($null -ne $uninstallWait) {
        $readback | Add-Member -NotePropertyName normal_uninstall_process -NotePropertyValue (
            [pscustomobject]@{
                exited = $uninstallWait.exited
                exit_code = $uninstallWait.exit_code
            }
        )
    }
    if ($null -ne $final) {
        $retention = Get-WixInstallerScenarioRetentionComparison `
            -Before $before.retention -After $final.retention
        $readback.settings_preserved = $retention.settings_preserved
        $readback.doctor_preserved = $retention.doctor_preserved
        $readback.retention_fingerprint_preserved = $retention.fingerprint_preserved
    }
    $allMsiLogs = @($allAttemptLogs | Where-Object { $_.kind -ceq 'msi' } | ForEach-Object {
        [pscustomobject]@{
            run = 'first'
            kind = 'msi'
            path = $_.path
            text = $_.text
        }
    })
    $formedMsiLogs = @(if ($null -ne $formed) { $formed.post_exit.msi_logs })
    $burnEvidence = @($allAttemptLogs | Where-Object { $_.kind -ceq 'burn' } | ForEach-Object {
        [pscustomobject]@{ run = 'first'; kind = 'burn'; path = $_.path; text = $_.text }
    })
    $evidence = ConvertFrom-WixInstallerScenarioLogs -Scenario Recovery `
        -BurnLogs @($burnEvidence) -MsiLogs @(
            if ($formedMsiLogs.Count -gt 0) { $formedMsiLogs } else { $allMsiLogs }
        ) -Readback $readback -EvidenceClass runtime_observed
    if ($null -ne $formed) {
        $evidence.restart_required = [bool]$formed.post_exit.state.restart_required
    }
    $verdict = Get-WixInstallerScenarioVerdict -Scenario Recovery -Evidence $evidence `
        -AcceptRetainedBundle:$AcceptRetainedBundle
    $rawLogs = @($allAttemptLogs + @(Get-WixInstallerScenarioLiveLogs -StartedUtc $scenarioStartedUtc) |
        Sort-Object path -Unique)
    $rawReceipt = Copy-WixInstallerScenarioRawLogs -Logs $rawLogs -OutputDir $OutputDir
    return [pscustomobject]@{
        format = 'liukai-installer-scenario-receipt-v1'
        scenario = 'Recovery'
        verdict = $verdict
        evidence = $evidence
        attempt_receipts = @($attemptReceipts)
        normal_rerun = if ($null -ne $rerun) {
            @{
                pid = $rerun.pid
                arguments = @($rerun.arguments)
                exited = $rerunWait.exited
                exit_code = $rerunWait.exit_code
            }
        }
        else {
            $null
        }
        normal_uninstall = if ($null -ne $uninstall) {
            @{
                pid = $uninstall.pid
                arguments = @($uninstall.arguments)
                exited = $uninstallWait.exited
                exit_code = $uninstallWait.exit_code
            }
        }
        else {
            $null
        }
        root_guest_reboot_required =
            ($null -ne $formed -and
                $formed.post_exit.state.restart_required) -or
            ($null -ne $rerunWait -and $rerunWait.exited -eq $true -and
                ([string]$rerunWait.exit_code -ceq '3010' -or
                 [string]$rerunWait.exit_code -ieq '0x0BC2')) -or
            ($null -ne $uninstallWait -and $uninstallWait.exited -eq $true -and
                ([string]$uninstallWait.exit_code -ceq '3010' -or
                 [string]$uninstallWait.exit_code -ieq '0x0BC2'))
        raw_log_copies = @($rawReceipt)
        unexpected_process_live = $null -ne $unexpectedProcessPid -or
            ($null -ne $rerunWait -and -not $rerunWait.exited) -or
            ($null -ne $uninstallWait -and -not $uninstallWait.exited)
    }
}

$runtimeManifest = Get-WixInstallerScenarioManifestData $InputManifest
$runtimeGuest = Get-WixInstallerScenarioGuestObservation $runtimeManifest
$runtimePreflight = Assert-WixInstallerScenarioPreflight `
    -BaselineSetup $BaselineSetup `
    -CandidateSetup $CandidateSetup `
    -OutputDir $OutputDir `
    -Scenario $Scenario `
    -InputManifest $InputManifest `
    -GuestObservation $runtimeGuest

$fixedRoot = (Get-WixInstallerScenarioProductRoot).TrimEnd('\')
$runtimeOutput = $runtimePreflight.output_path.TrimEnd('\')
if ($runtimeOutput.Equals($fixedRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $runtimeOutput.StartsWith($fixedRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw '[SC013] OutputDir must not overlap the fixed LiuKai product root.'
}
if ($Scenario -ceq 'Recovery' -and (Test-WixInstallerScenarioRuntimeElevated) -and -not $AllowElevatedBaUi) {
    throw '[SC010] Recovery requires the normal interactive WDAGUtilityAccount token; an elevated harness shell is not eligible to launch or stop the BA/UI.'
}

[IO.Directory]::CreateDirectory($runtimeOutput) | Out-Null
Write-WixInstallerScenarioJson (Join-Path $runtimeOutput 'input-proof.json') ([ordered]@{
    format = 'liukai-installer-scenario-input-proof-v1'
    scenario = $runtimePreflight.scenario
    manifest_sha256 = (Get-FileHash -LiteralPath $runtimePreflight.manifest_path -Algorithm SHA256).Hash.ToLowerInvariant()
    baseline = $runtimePreflight.baseline
    candidate = $runtimePreflight.candidate
    owned_authorization_kind = $runtimePreflight.owned_instance.authorization_kind
    owned_sandbox_id = $runtimePreflight.owned_instance.sandbox_id
    sentinel_path = $runtimeGuest.sentinel_path
    sentinel_sha256 = $runtimeGuest.sentinel_sha256
    guest_boot_utc = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    guest_account = $runtimeGuest.account_name
    observation_timeout_seconds = $ObservationTimeoutSeconds
    allow_elevated_ba_ui = [bool]$AllowElevatedBaUi
    recovery_attempt_limit = $RecoveryAttempts
    accept_retained_bundle = [bool]$AcceptRetainedBundle
})
Write-WixInstallerScenarioProgress -OutputDir $runtimeOutput -Phase 'guard' -Status 'owned_guest_verified'

try {
    $runtimeFixtures = Resolve-WixInstallerScenarioFixtures -Manifest $runtimeManifest `
        -OutputDir $runtimeOutput -ToolchainBin $ToolchainBin
    Write-WixInstallerScenarioJson (Join-Path $runtimeOutput 'fixtures.json') $runtimeFixtures
    $receipt = if ($Scenario -in @('Upgrade', 'Reentry')) {
        Invoke-WixInstallerScenarioUpgradeOrReentry `
            -Preflight $runtimePreflight `
            -Fixtures $runtimeFixtures `
            -OutputDir $runtimeOutput `
            -TimeoutSeconds $ObservationTimeoutSeconds `
            -MappedModuleHoldSeconds $MappedModuleHoldSeconds
    }
    else {
        Invoke-WixInstallerScenarioRecovery `
            -Preflight $runtimePreflight `
            -Fixtures $runtimeFixtures `
            -OutputDir $runtimeOutput `
            -TimeoutSeconds $ObservationTimeoutSeconds -AllowElevatedBaUi:$AllowElevatedBaUi `
            -MaxAttempts $RecoveryAttempts -AcceptRetainedBundle:$AcceptRetainedBundle
    }
    Write-WixInstallerScenarioJson (Join-Path $runtimeOutput 'receipt.json') $receipt
    Write-WixInstallerScenarioProgress -OutputDir $runtimeOutput -Phase 'complete' `
        -Status ([string]$receipt.verdict.receipt_status) -Detail $receipt.verdict
    Write-Output (Get-WixInstallerScenarioJson $receipt)
    if ($receipt.verdict.actual_scenario_pass) {
        exit 0
    }
    if ($receipt.verdict.verdict -ceq 'NOT_COVERED') {
        exit 2
    }
    exit 1
}
catch {
    $failure = [ordered]@{
        format = 'liukai-installer-scenario-failure-v1'
        scenario = $Scenario
        verdict = 'FAIL'
        captured_utc = [DateTime]::UtcNow.ToString('o')
        error = $_.Exception.Message
        script_line = $_.InvocationInfo.ScriptLineNumber
    }
    Write-WixInstallerScenarioJson (Join-Path $runtimeOutput 'failure.json') $failure
    Write-WixInstallerScenarioProgress -OutputDir $runtimeOutput -Phase 'failed' `
        -Status 'FAIL' -Detail $failure
    throw
}
