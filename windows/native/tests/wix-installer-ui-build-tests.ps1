#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$WixExe,
    [Parameter(Mandatory = $true)][string]$BalExtension,
    [Parameter(Mandatory = $true)][string]$Msi,
    [Parameter(Mandatory = $true)][string]$MigrationBundle,
    [Parameter(Mandatory = $true)][string]$GuardDll,
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [string]$RuntimeVersion
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-WixUiMsiProductVersion {
    param([Parameter(Mandatory = $true)][string]$Path)

    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.OpenDatabase($Path, 0)
    $view = $database.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = 'ProductVersion'")
    try {
        [void]$view.Execute()
        $record = $view.Fetch()
        if ($null -eq $record) {
            throw 'MSI ProductVersion property is missing.'
        }
        $value = ([string]$record.StringData(1)).Trim()
        if ([string]::IsNullOrWhiteSpace($value)) {
            throw 'MSI ProductVersion property is empty.'
        }
        return $value
    }
    finally {
        [void]$view.Close()
    }
}

function Format-WixUiCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $parts = @($Executable) + @($Arguments)
    return (@($parts | ForEach-Object {
        $value = [string]$_
        if ($value -match '[\s"]') {
            '"' + $value.Replace('"', '\"') + '"'
        }
        else {
            $value
        }
    }) -join ' ')
}

$windowsRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$wixDir = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\wix')).Path
$wix = (Resolve-Path -LiteralPath $WixExe).Path
$bal = (Resolve-Path -LiteralPath $BalExtension).Path
$msiPath = (Resolve-Path -LiteralPath $Msi).Path
$migrationPath = (Resolve-Path -LiteralPath $MigrationBundle).Path
$guardPath = (Resolve-Path -LiteralPath $GuardDll).Path
$out = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $out) {
    throw "Output directory already exists: $out"
}
$outParent = Split-Path -Parent $out
if (-not (Test-Path -LiteralPath $outParent -PathType Container)) {
    throw "Output parent must exist: $outParent"
}

$wixVersion = (& $wix --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $wixVersion -cnotmatch '^6\.0\.2\+') {
    throw 'WiX 6.0.2 is required for the installer UI build test.'
}
$productVersion = Get-WixUiMsiProductVersion $msiPath

$theme = (Resolve-Path -LiteralPath (Join-Path $wixDir 'ui\rtf-theme.xml')).Path
$localization = (Resolve-Path -LiteralPath (Join-Path $wixDir 'ui\zh-TW.wxl')).Path
$uiLicense = (Resolve-Path -LiteralPath (Join-Path $wixDir 'ui\LICENSE.TXT')).Path
$notice = (Resolve-Path -LiteralPath (Join-Path $wixDir 'install-notice.rtf')).Path
$icon = (Resolve-Path -LiteralPath (Join-Path $windowsRoot 'assets\liukai.ico')).Path
$logo = (Resolve-Path -LiteralPath (Join-Path $windowsRoot 'assets\liukai-installer-logo.png')).Path
$developmentAuthoring = (Resolve-Path -LiteralPath (Join-Path $wixDir 'bundle.wxs')).Path
$migrationAuthoring = (Resolve-Path -LiteralPath (Join-Path $wixDir 'bundle-migration.wxs')).Path
$uiTest = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'wix-installer-ui-tests.ps1')).Path
$fontTest = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'wix-installer-ui-font-tests.ps1')).Path
$guardTest = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'wix-installer-guard-payload-tests.ps1')).Path

[IO.Directory]::CreateDirectory($out) | Out-Null
$fontTestCommand = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$fontTest`" -ThemeFile `"$theme`""
$fontTestJson = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $fontTest -ThemeFile $theme | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Installer UI font coverage test failed: $LASTEXITCODE"
}
$fontTestResult = $fontTestJson | ConvertFrom-Json
$developmentBundle = Join-Path $out 'LiuKai-ui-development-setup.exe'
$developmentBuildArguments = @(
    'build',
    '-arch', 'x64',
    '-ext', $bal,
    '-d', "BundleVersion=$productVersion",
    '-d', "IconPath=$icon",
    '-d', "LicensePath=$notice",
    '-d', "LogoPath=$logo",
    '-d', "UiThemePath=$theme",
    '-d', "UiLocalizationPath=$localization",
    '-d', "UiLicensePath=$uiLicense",
    '-d', "GuardDll=$guardPath",
    '-d', "MsiPath=$msiPath"
)
if (-not [string]::IsNullOrWhiteSpace($RuntimeVersion)) {
    $developmentBuildArguments += @('-d', "RuntimeVersion=$RuntimeVersion")
}
$developmentBuildArguments += @('-o', $developmentBundle, $developmentAuthoring)
$developmentBuildCommand = Format-WixUiCommand $wix $developmentBuildArguments
& $wix @developmentBuildArguments
if ($LASTEXITCODE -ne 0) {
    throw "Development Burn build failed: $LASTEXITCODE"
}

$migrationPayload = Join-Path $out 'migration-payload'
$migrationBa = Join-Path $out 'migration-ba'
$migrationExtractArguments = @('burn', 'extract', $migrationPath, '-o', $migrationPayload, '-oba', $migrationBa)
$migrationExtractCommand = Format-WixUiCommand $wix $migrationExtractArguments
& $wix @migrationExtractArguments
if ($LASTEXITCODE -ne 0) {
    throw "Migration Burn extraction failed: $LASTEXITCODE"
}

$developmentPayload = Join-Path $out 'development-payload'
$developmentBa = Join-Path $out 'development-ba'
$developmentExtractArguments = @('burn', 'extract', $developmentBundle, '-o', $developmentPayload, '-oba', $developmentBa)
$developmentExtractCommand = Format-WixUiCommand $wix $developmentExtractArguments
& $wix @developmentExtractArguments
if ($LASTEXITCODE -ne 0) {
    throw "Development Burn extraction failed: $LASTEXITCODE"
}

$sourceLicenseHash = (Get-FileHash -LiteralPath $uiLicense -Algorithm SHA256).Hash
$embeddedLicenseHashes = [ordered]@{}
$guardValidation = [ordered]@{}
foreach ($case in @(
    [pscustomobject]@{ Name = 'migration'; Directory = $migrationBa },
    [pscustomobject]@{ Name = 'development'; Directory = $developmentBa }
)) {
    $embeddedLicense = Join-Path $case.Directory 'wix-theme-license.txt'
    if (-not (Test-Path -LiteralPath $embeddedLicense -PathType Leaf)) {
        throw "$($case.Name) BA is missing wix-theme-license.txt."
    }
    $embeddedHash = (Get-FileHash -LiteralPath $embeddedLicense -Algorithm SHA256).Hash
    if ($embeddedHash -cne $sourceLicenseHash) {
        throw "$($case.Name) BA embeds a different WiX theme license."
    }
    $embeddedLicenseHashes[$case.Name] = $embeddedHash.ToLowerInvariant()
    $guardJson = (& $guardTest -BaDir $case.Directory -ExpectedGuardDll $guardPath | Out-String).Trim()
    $guardValidation[$case.Name] = $guardJson | ConvertFrom-Json
}

$sharedValidationJson = (& $uiTest `
    -BaDir @($migrationBa, $developmentBa) `
    -SourceTheme $theme `
    -SourceLocalization $localization `
    -BundleAuthoring @($migrationAuthoring, $developmentAuthoring) | Out-String).Trim()
$sharedValidation = $sharedValidationJson | ConvertFrom-Json
$selfTestJson = (& $uiTest -BaDir $developmentBa -SelfTest | Out-String).Trim()
$selfTest = $selfTestJson | ConvertFrom-Json

$receipt = [ordered]@{
    wix_version                    = $wixVersion
    msi_product_version            = $productVersion
    runtime_version                = if ($RuntimeVersion) { $RuntimeVersion } else { $null }
    msi_sha256                     = (Get-FileHash -LiteralPath $msiPath -Algorithm SHA256).Hash.ToLowerInvariant()
    migration_bundle_sha256        = (Get-FileHash -LiteralPath $migrationPath -Algorithm SHA256).Hash.ToLowerInvariant()
    development_bundle_sha256      = (Get-FileHash -LiteralPath $developmentBundle -Algorithm SHA256).Hash.ToLowerInvariant()
    theme_sha256                   = (Get-FileHash -LiteralPath $theme -Algorithm SHA256).Hash.ToLowerInvariant()
    localization_sha256            = (Get-FileHash -LiteralPath $localization -Algorithm SHA256).Hash.ToLowerInvariant()
    ui_license_sha256              = $sourceLicenseHash.ToLowerInvariant()
    embedded_ui_license_sha256     = $embeddedLicenseHashes
    guard_dll_sha256              = (Get-FileHash -LiteralPath $guardPath -Algorithm SHA256).Hash.ToLowerInvariant()
    guard_payload_validation      = $guardValidation
    development_build_command      = $developmentBuildCommand
    font_test_command              = $fontTestCommand
    migration_extract_command      = $migrationExtractCommand
    development_extract_command    = $developmentExtractCommand
    font_test                       = $fontTestResult
    shared_validation              = $sharedValidation
    self_test                      = $selfTest
    migration_ba_directory         = $migrationBa
    development_ba_directory       = $developmentBa
    development_bundle              = $developmentBundle
}
$receiptPath = Join-Path $out 'ui-build-test-result.json'
$receiptJson = $receipt | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText($receiptPath, $receiptJson, [Text.UTF8Encoding]::new($false))
$receiptJson
