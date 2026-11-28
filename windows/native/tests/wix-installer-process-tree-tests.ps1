#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'wix-installer-scenarios.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw '[SCT000] Harness syntax errors.' }
foreach ($name in @('Get-WixInstallerScenarioProperty', 'Get-WixInstallerScenarioProcessTree')) {
    $definition = $ast.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
    }, $false)
    if ($null -eq $definition) { throw "[SCT000] Missing $name." }
    . ([scriptblock]::Create($definition.Extent.Text))
}
function Require-Tree($Condition, [string]$Message) {
    if (-not $Condition) { throw "[SCT001] $Message" }
}
$script:treeRows = @(
    [pscustomobject]@{ ProcessId = 100; ParentProcessId = 7 },
    [pscustomobject]@{ ProcessId = 200; ParentProcessId = 100 },
    [pscustomobject]@{ ProcessId = 250; ParentProcessId = 100 },
    [pscustomobject]@{ ProcessId = 300; ParentProcessId = 200 },
    [pscustomobject]@{ ProcessId = 900; ParentProcessId = 7 },
    [pscustomobject]@{ ProcessId = 901; ParentProcessId = 900 }
)
$script:treeRootHashMatch = $true
$script:treeRootObservationPresent = $true
$script:treeObservedIds = @()
function Get-CimInstance {
    [CmdletBinding()]
    param([Parameter(Position = 0)][string]$ClassName)
    Require-Tree ($ClassName -ceq 'Win32_Process') 'Unexpected CIM inventory request.'
    return $script:treeRows
}
function Get-WixInstallerScenarioProcessObservation {
    param([int]$ProcessId, [string]$SetupHash, [string[]]$TrustedBaHashes, [int]$ParentPid)
    Require-Tree ($SetupHash -ceq ('a' * 64)) 'Setup hash was not forwarded.'
    Require-Tree (@($TrustedBaHashes).Count -eq 1 -and $TrustedBaHashes[0] -ceq ('b' * 64)) 'Trusted BA hash was not forwarded.'
    $script:treeObservedIds += $ProcessId
    if ($ProcessId -eq 100 -and -not $script:treeRootObservationPresent) { return $null }
    return [pscustomobject]@{
        pid = $ProcessId
        parent_pid = $ParentPid
        process_kind = if ($ProcessId -eq 200) { 'candidate_ba_ui' } else { 'burn_engine' }
        setup_hash_match = if ($ProcessId -eq 100) { $script:treeRootHashMatch } else { $ProcessId -ne 200 }
    }
}
$root = [pscustomobject]@{
    Id = 100
    HasExited = $false
    StartTime = [DateTime]::Parse('2026-11-28T03:00:00Z').ToUniversalTime()
}
$root | Add-Member -MemberType ScriptMethod -Name Refresh -Value { }
$birth = $root.StartTime.ToUniversalTime().ToString('o')
$arguments = @{
    RootPid = 100; RootCreationTimeUtc = $birth; RootProcess = $root
    SetupHash = ('a' * 64); TrustedBaHashes = @('b' * 64)
}
$tree = @(Get-WixInstallerScenarioProcessTree @arguments)
Require-Tree (($tree.pid | Sort-Object) -join ',' -ceq '100,200,250,300') 'Runtime traversal did not include all three descendant levels and exclude unrelated processes.'
Require-Tree (($script:treeObservedIds | Sort-Object) -join ',' -ceq '100,200,250,300') 'An unrelated process reached the identity observer.'
foreach ($expected in @(
    @{ id = 100; parent = 7; ancestry = '100' },
    @{ id = 200; parent = 100; ancestry = '100,200' },
    @{ id = 250; parent = 100; ancestry = '100,250' },
    @{ id = 300; parent = 200; ancestry = '100,200,300' }
)) {
    $row = @($tree | Where-Object { $_.pid -eq $expected.id })
    Require-Tree ($row.Count -eq 1 -and $row[0].parent_pid -eq $expected.parent -and
        ($row[0].ancestry_pids -join ',') -ceq $expected.ancestry -and
        $row[0].root_pid -eq 100 -and $row[0].root_creation_time_utc -ceq $birth) 'Runtime root identity or ancestry was lost.'
}
$arguments.RootCreationTimeUtc = $root.StartTime.AddSeconds(1).ToString('o')
Require-Tree (@(Get-WixInstallerScenarioProcessTree @arguments).Count -eq 0) 'A mismatched root birth time was accepted.'
$arguments.RootCreationTimeUtc = $birth
$script:treeRootHashMatch = $false
Require-Tree (@(Get-WixInstallerScenarioProcessTree @arguments).Count -eq 0) 'A mismatched root setup hash was accepted.'
$script:treeRootHashMatch = $true
$script:treeRootObservationPresent = $false
$script:treeObservedIds = @()
Require-Tree (@(Get-WixInstallerScenarioProcessTree @arguments).Count -eq 0) 'Missing root identity observation was accepted.'
Require-Tree (($script:treeObservedIds -join ',') -ceq '100') 'Missing root identity still allowed child traversal.'
$script:treeRootObservationPresent = $true
$root.HasExited = $true
Require-Tree (@(Get-WixInstallerScenarioProcessTree @arguments).Count -eq 0) 'An exited root was accepted.'

'wix_installer_process_tree_passed=true; runtime_traversal_chain=true; process_inventory=stub; installer_runs=0; kill_actions=0; registry_writes=0'
