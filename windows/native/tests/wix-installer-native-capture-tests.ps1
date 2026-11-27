#Requires -Version 5.1
param([string]$ToolchainBin = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$harness = Join-Path $PSScriptRoot 'wix-installer-scenarios.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($harness, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw '[SCN000] Harness syntax errors.' }
$functionAst = $ast.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Invoke-WixInstallerScenarioNativeCapture'
}, $false)
if ($null -eq $functionAst) { throw '[SCN000] Missing native capture helper.' }
. ([scriptblock]::Create($functionAst.Extent.Text))
$msiAst = $ast.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Get-WixInstallerScenarioMsiProducts'
}, $false)
if ($null -eq $msiAst) { throw '[SCN000] Missing MSI read-only inventory helper.' }
. ([scriptblock]::Create($msiAst.Extent.Text))
$mappedAst = $ast.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Start-WixInstallerScenarioMappedModule'
}, $false)
if ($null -eq $mappedAst) { throw '[SCN000] Missing mapped-module helper.' }
. ([scriptblock]::Create($mappedAst.Extent.Text))
$logAst = $ast.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Read-WixInstallerScenarioSharedLog'
}, $false)
if ($null -eq $logAst) { throw '[SCN000] Missing live-log shared reader.' }
. ([scriptblock]::Create($logAst.Extent.Text))
$setupAst = $ast.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Start-WixInstallerScenarioSetup'
}, $false)
if ($null -eq $setupAst) { throw '[SCN000] Missing synthetic setup helper.' }
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$work = Join-Path $tempBase ('liukai-native-capture-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($work)
$failures = [Collections.Generic.List[string]]::new()
$pathBefore = $env:PATH
try {
    & {
        param([string]$SetupFunctionSource)

        $startCalls = [Collections.Generic.List[object]]::new()
        function Start-Process {
            param(
                [string]$FilePath,
                [string[]]$ArgumentList = @(),
                [string]$WorkingDirectory,
                [switch]$PassThru
            )

            [void]$startCalls.Add([pscustomobject]@{
                file_path = $FilePath
                arguments = [string[]]@($ArgumentList)
                working_directory = $WorkingDirectory
                pass_thru = [bool]$PassThru
            })
            return [pscustomobject]@{
                Handle = [IntPtr]::Zero
                Id = 4242
                StartTime = [DateTime]::UtcNow
            }
        }

        . ([scriptblock]::Create($SetupFunctionSource))
        $setupPath = 'C:\Synthetic Setup\setup.exe'
        $setupDirectory = 'C:\Synthetic Setup'
        $burnLog = 'C:\Synthetic Logs\setup with spaces.log'
        [void](Start-WixInstallerScenarioSetup -SetupPath $setupPath `
            -WorkingDirectory $setupDirectory -BurnLog $burnLog -Passive)
        [void](Start-WixInstallerScenarioSetup -SetupPath $setupPath `
            -WorkingDirectory $setupDirectory)

        $setupFailures = [Collections.Generic.List[string]]::new()
        if ($startCalls.Count -ne 2) {
            $setupFailures.Add("Expected two synthetic setup calls; received $($startCalls.Count).")
        }
        else {
            $passiveArguments = [string[]]@($startCalls[0].arguments)
            $quotedBurnLog = '"' + $burnLog + '"'
            if (@($passiveArguments | Where-Object { $_ -ceq '/norestart' }).Count -ne 1) {
                $setupFailures.Add('Passive setup must pass exactly one /norestart argument.')
            }
            if (@($passiveArguments | Where-Object { $_ -ceq '/passive' }).Count -ne 1) {
                $setupFailures.Add('Passive setup must retain exactly one /passive argument.')
            }
            if (@($passiveArguments | Where-Object { $_ -ceq '/log' }).Count -ne 1 -or
                @($passiveArguments | Where-Object { $_ -ceq $quotedBurnLog }).Count -ne 1) {
                $setupFailures.Add('Passive setup must retain the supplied quoted Burn log path.')
            }
            if ($passiveArguments.Count -ne 4) {
                $setupFailures.Add("Passive setup passed $($passiveArguments.Count) arguments instead of 4.")
            }
            $normalArguments = [string[]]@($startCalls[1].arguments)
            if ($normalArguments.Count -ne 0) {
                $setupFailures.Add("Normal setup passed $($normalArguments.Count) arguments instead of zero.")
            }
        }
        if ($setupFailures.Count) {
            throw ('[SCN005] ' + ($setupFailures -join ' '))
        }
    } $setupAst.Extent.Text
    $livePath = Join-Path $work 'live-writer.log'
    $writer = [IO.File]::Open($livePath, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes('Apply begin while writer is alive')
        $writer.Write($bytes, 0, $bytes.Length); $writer.Flush()
        $liveText = Read-WixInstallerScenarioSharedLog $livePath
        if ($liveText -cne 'Apply begin while writer is alive') { throw '[SCN004] Live log text was not read while producer held write access.' }
    }
    finally { $writer.Dispose() }
    foreach ($expected in @(0, 23)) {
        $result = Invoke-WixInstallerScenarioNativeCapture -FilePath "$env:WINDIR\System32\cmd.exe" `
            -ArgumentList @('/d', '/c', 'exit', [string]$expected) -WorkDir $work
        if ($result.exit_code -isnot [int] -or $result.exit_code -ne $expected) {
            $failures.Add("Expected numeric exit $expected; received '$($result.exit_code)'.")
        }
        if ($result.stdout -isnot [string] -or $result.stdout.Length -ne 0 -or
            $result.stderr -isnot [string] -or $result.stderr.Length -ne 0) {
            $failures.Add("Empty native streams were not strings for exit $expected.")
        }
    }
    if ($failures.Count) { throw ('[SCN001] ' + ($failures -join ' ')) }
    $inventory = Get-WixInstallerScenarioMsiProducts
    if (@($inventory.issues).Count) { throw ('[SCN002] Read-only MSI inventory failed: ' + ($inventory.issues -join ' ')) }
    foreach ($product in @($inventory.products)) {
        if ([string]::IsNullOrWhiteSpace($product.version) -or
            $product.upgrade_code -ine '{7E8341DC-CE5A-4A3A-A6BC-2C5E70FEFC39}') {
            throw '[SCN002] Related MSI identity is incomplete.'
        }
    }
    $compiler = if ($ToolchainBin) { Join-Path $ToolchainBin 'g++.exe' } else { (Get-Command g++.exe -ErrorAction Stop).Source }
    $env:PATH = (Split-Path -Parent $compiler) + ';' + $env:PATH
    $fixture = Join-Path $work 'mapped-fixture.exe'
    $compiled = Invoke-WixInstallerScenarioNativeCapture -FilePath $compiler -WorkDir $work `
        -ArgumentList @('-std=c++17', '-O2', '-static', '-municode', (Join-Path $PSScriptRoot 'mapped_module_fixture.cpp'), '-o', $fixture)
    if ($compiled.exit_code -ne 0) { throw ('[SCN003] Fixture compilation failed: ' + $compiled.stderr) }
    $dllDir = Join-Path $work 'DLL path with spaces'; [void][IO.Directory]::CreateDirectory($dllDir)
    $output = Join-Path $work 'output path with spaces'; [void][IO.Directory]::CreateDirectory($output)
    $dll = Join-Path $dllDir 'version.dll'; Copy-Item -LiteralPath "$env:WINDIR\System32\version.dll" -Destination $dll
    $mapped = Start-WixInstallerScenarioMappedModule -FixtureExe $fixture -DllPath $dll -OutputDir $output -HoldSeconds 2
    if ($mapped.module_path -ine $dll -or $mapped.dll_sha256 -ne (Get-FileHash $dll).Hash.ToLowerInvariant()) {
        throw '[SCN003] Spaced-path DLL identity was not observed.'
    }
    if (-not $mapped.process.WaitForExit(5000)) { throw '[SCN003] Bounded fixture did not exit.' }
    'wix_installer_native_capture_passed=true; spaced_path_module_passed=true; installer_runs=0; kill_actions=0; registry_writes=0'
}
finally {
    $env:PATH = $pathBefore
    if ([IO.Path]::GetFullPath($work).StartsWith($tempBase + '\', [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $work) -like 'liukai-native-capture-*') {
        [IO.Directory]::Delete($work, $true)
    }
}
