param(
    [Parameter(Mandatory)][string]$WixExe,
    [Parameter(Mandatory)][string]$BalExtension,
    [Parameter(Mandatory)][string]$ToolchainBin,
    [Parameter(Mandatory)][string]$R64Msi,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$Tag
)
$ErrorActionPreference = 'Stop'
$windows = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$version = (Get-Content -Raw -LiteralPath (Join-Path $windows 'VERSION')).Trim()
if ($version -cnotmatch '^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})$') {
    throw 'windows/VERSION must be a three-part release version.'
}
$major = 70 + [int]$Matches[1]
$minor = [int]$Matches[2]
$build = 2 + [int]$Matches[3]
if ($major -gt 255 -or $minor -gt 255 -or $build -gt 65535) { throw 'Release version exceeds Windows Installer version limits.' }
$msiVersion = "$major.$minor.$build"
$expectedTag = "windows-v$version"
if ($Tag -and $Tag -cne $expectedTag) { throw "Tag must match windows/VERSION: $expectedTag" }
$commit = (& git -C $windows rev-parse HEAD)
if ($LASTEXITCODE -ne 0 -or $commit -cnotmatch '^[0-9a-f]{40}$') { throw 'Could not identify source commit.' }
$changes = & git -C $windows status --porcelain --untracked-files=all -- .
if ($LASTEXITCODE -ne 0 -or $changes) { throw 'Build Windows releases from a clean Windows source tree.' }
$output = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $output) { throw "Output directory already exists: $output" }
[IO.Directory]::CreateDirectory($output) | Out-Null
$source = Join-Path $output 'source'
$wixOut = Join-Path $output 'wix'
$release = Join-Path $output 'release'
$toolchain = (Resolve-Path -LiteralPath $ToolchainBin).Path
$oldPath = $env:PATH
try {
    $env:PATH = "$toolchain;$oldPath"
    & (Join-Path $windows 'native\build-fresh-release.ps1') -ToolchainBin $toolchain -Version $version -OutputDir $source -SetupName "WuGai-$version-x64-Setup.exe"
    & (Join-Path $PSScriptRoot 'build-installer.ps1') -ToolchainBin $toolchain -WixExe $WixExe -BalExtension $BalExtension -R64Msi $R64Msi -SourceBundle (Join-Path $source 'bundle') -OutputDir $wixOut -MsiVersion $msiVersion -PublishBuild
} finally { $env:PATH = $oldPath }
[IO.Directory]::CreateDirectory($release) | Out-Null
$name = "liu-kai-windows-$version-x64-setup.exe"
$installer = Join-Path $release $name
Copy-Item -LiteralPath (Join-Path $wixOut 'LiuKai-win-setup.exe') -Destination $installer
$digest = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $release 'SHA256SUMS.txt'), "$digest  $name`n", [Text.UTF8Encoding]::new($false))
$record = @("source_commit=$commit", "tag=$expectedTag", "package_version=$version", "msi_version=$msiVersion", 'wix=6.0.2')
[IO.File]::WriteAllLines((Join-Path $release 'SOURCE.txt'), $record, [Text.UTF8Encoding]::new($false))
