param(
    [Parameter(Mandatory=$true)][string]$WixExe,
    [Parameter(Mandatory=$true)][string]$Bundle,
    [Parameter(Mandatory=$true)][string]$Msi,
    [string]$Patch,
    [Parameter(Mandatory=$true)][string]$Icon,
    [Parameter(Mandatory=$true)][string]$Logo,
    [Parameter(Mandatory=$true)][string]$Notice,
    [string]$Theme,
    [string]$Localization,
    [string]$UiLicense,
    [Parameter(Mandatory=$true)][string]$GuardDll,
    [Parameter(Mandatory=$true)][string]$ExtractDir
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Use PowerShell 7 for consistent ICO decoding.' }
$wix=(Resolve-Path -LiteralPath $WixExe).Path
$bundlePath=(Resolve-Path -LiteralPath $Bundle).Path
$msiPath=(Resolve-Path -LiteralPath $Msi).Path
$iconPath=(Resolve-Path -LiteralPath $Icon).Path
$noticePath=(Resolve-Path -LiteralPath $Notice).Path
$logoPath=(Resolve-Path -LiteralPath $Logo).Path
if(-not $Theme){$Theme=Join-Path $PSScriptRoot 'ui\rtf-theme.xml'}
if(-not $Localization){$Localization=Join-Path $PSScriptRoot 'ui\zh-TW.wxl'}
if(-not $UiLicense){$UiLicense=Join-Path $PSScriptRoot 'ui\LICENSE.TXT'}
$themePath=(Resolve-Path -LiteralPath $Theme).Path
$localizationPath=(Resolve-Path -LiteralPath $Localization).Path
$uiLicensePath=(Resolve-Path -LiteralPath $UiLicense).Path
$guardDllPath=(Resolve-Path -LiteralPath $GuardDll).Path
$extract=[IO.Path]::GetFullPath($ExtractDir)
if (Test-Path -LiteralPath $extract) { throw 'Extraction directory already exists; choose a new directory.' }
& $wix msi validate $msiPath
if ($LASTEXITCODE -ne 0) { throw "MSI validation failed: $LASTEXITCODE" }
& $wix burn extract $bundlePath -o $extract -oba ($extract+'-ba')
if ($LASTEXITCODE -ne 0) { throw "Burn extraction failed: $LASTEXITCODE" }
$embeddedLogo=Join-Path ($extract+'-ba') 'logo.png'
if((Get-FileHash -LiteralPath $logoPath -Algorithm SHA256).Hash -cne (Get-FileHash -LiteralPath $embeddedLogo -Algorithm SHA256).Hash){throw 'Embedded installer UI logo differs from source.'}
$embeddedNotice=Join-Path ($extract+'-ba') 'license.rtf'
if((Get-FileHash -LiteralPath $noticePath -Algorithm SHA256).Hash -cne (Get-FileHash -LiteralPath $embeddedNotice -Algorithm SHA256).Hash){throw 'Embedded installation notice differs from source.'}
$embeddedTheme=Join-Path ($extract+'-ba') 'thm.xml'
if((Get-FileHash -LiteralPath $themePath -Algorithm SHA256).Hash -cne (Get-FileHash -LiteralPath $embeddedTheme -Algorithm SHA256).Hash){throw 'Embedded installer theme differs from the shared source.'}
$embeddedLocalization=Join-Path ($extract+'-ba') 'thm.wxl'
if((Get-FileHash -LiteralPath $localizationPath -Algorithm SHA256).Hash -cne (Get-FileHash -LiteralPath $embeddedLocalization -Algorithm SHA256).Hash){throw 'Embedded installer localization differs from the shared source.'}
$embeddedUiLicense=Join-Path ($extract+'-ba') 'wix-theme-license.txt'
if((Get-FileHash -LiteralPath $uiLicensePath -Algorithm SHA256).Hash -cne (Get-FileHash -LiteralPath $embeddedUiLicense -Algorithm SHA256).Hash){throw 'Embedded WiX theme license differs from the shared source.'}
$uiTest=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\tests\wix-installer-ui-tests.ps1')).Path
& $uiTest -BaDir ($extract+'-ba')
$guardTest=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\tests\wix-installer-guard-payload-tests.ps1')).Path
$guardJson=(& $guardTest -BaDir ($extract+'-ba') -ExpectedGuardDll $guardDllPath|Out-String).Trim()
$guardResult=$guardJson|ConvertFrom-Json
if(-not $guardResult.guard_payload_verified){throw 'Installer guard payload verification did not pass.'}
$expected=(Get-FileHash -LiteralPath $msiPath -Algorithm SHA256).Hash
$matches=@(Get-ChildItem -LiteralPath $extract -File -Recurse | Where-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -ceq $expected })
if ($matches.Count -ne 1) { throw "Embedded MSI hash match count was $($matches.Count), expected one." }
if($Patch){
    $patchHash=(Get-FileHash -LiteralPath (Resolve-Path -LiteralPath $Patch).Path -Algorithm SHA256).Hash
    $patchMatches=@(Get-ChildItem -LiteralPath $extract -File -Recurse | Where-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -ceq $patchHash })
    if($patchMatches.Count -ne 1){throw "Embedded R64 migration patch hash match count was $($patchMatches.Count), expected one."}
    [xml]$manifest=Get-Content -LiteralPath (Join-Path ($extract+'-ba') 'manifest.xml') -Raw
    $chain=@($manifest.SelectNodes("//*[local-name()='Chain']/*"))
    if($chain.Count -ne 2 -or $chain[0].LocalName -cne 'MspPackage' -or $chain[0].GetAttribute('Id') -cne 'R64Migration' -or $chain[1].LocalName -cne 'MsiPackage' -or $chain[1].GetAttribute('Id') -cne 'LiuKaiMsi'){throw 'Unexpected Burn migration chain order.'}
    $rm=$chain[0].SelectSingleNode("*[local-name()='MsiProperty' and @Id='MSIRESTARTMANAGERCONTROL']")
    $gate=$chain[1].SelectSingleNode("*[local-name()='MsiProperty' and @Id='LIUKAIBUNDLEUPGRADE']")
    if(-not $rm -or $rm.GetAttribute('Value') -cne 'DisableShutdown' -or -not $gate -or $gate.GetAttribute('Value') -cne '1'){throw 'Missing Burn migration safety properties.'}
    $targets=@($manifest.SelectNodes("//*[local-name()='PatchTargetCode']"))
    if($targets.Count -ne 1 -or $targets[0].GetAttribute('TargetCode') -cne '{7ED30030-AC1F-4E13-87F8-5648D0B84C38}'){throw 'Unexpected R64 migration patch target.'}
}
Add-Type -AssemblyName System.Drawing
$source=New-Object System.Drawing.Icon($iconPath,32,32)
$embedded=[Drawing.Icon]::ExtractAssociatedIcon($bundlePath)
if ($null -eq $embedded) { $source.Dispose(); throw 'Bundle has no associated icon.' }
$left=$null; $right=$null
try {
    $left=$source.ToBitmap(); $right=$embedded.ToBitmap()
    if ($left.Width -ne $right.Width -or $left.Height -ne $right.Height) { throw 'Bundle icon dimensions differ.' }
    for($x=0;$x -lt $left.Width;$x++) { for($y=0;$y -lt $left.Height;$y++) {
        if($left.GetPixel($x,$y).ToArgb() -ne $right.GetPixel($x,$y).ToArgb()) { throw 'Bundle icon pixels differ from the project icon.' }
    }}
} finally { if($left){$left.Dispose()}; if($right){$right.Dispose()}; $source.Dispose(); $embedded.Dispose() }
[pscustomobject]@{msi_sha256=$expected.ToLowerInvariant();msi_validated=$true;embedded_msi_verified=$true;icon_32px_verified=$true;notice_verified=$true;ui_theme_verified=$true;ui_localization_verified=$true;ui_license_verified=$true;ui_logo_verified=$true;guard_payload_verified=$true;guard_sha256=$guardResult.sha256} | ConvertTo-Json
