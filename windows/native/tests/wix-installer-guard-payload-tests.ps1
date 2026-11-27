param([Parameter(Mandatory=$true)][string]$BaDir,[string]$ExpectedGuardDll='')
Set-StrictMode -Version 2.0
$ErrorActionPreference='Stop'
$guardName='LiuKaiInstallerGuard.dll'

function Throw-Guard([string]$Code,[string]$Message){throw "[$Code] $Message"}

function Read-GuardXml([string]$Path,[string]$Code,[string]$Label){
 if(-not(Test-Path -LiteralPath $Path -PathType Leaf)){Throw-Guard $Code "Missing $Label`: $Path"}
 try{
  $settings=[Xml.XmlReaderSettings]::new()
  $settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit;$settings.XmlResolver=$null
  # WiX metadata can be UTF-16 with a BOM. Let the XML reader honor the
  # encoding declaration/BOM while keeping external entities prohibited.
  $reader=[Xml.XmlReader]::Create($Path,$settings)
  try{
   $document=[Xml.XmlDocument]::new();$document.XmlResolver=$null;$document.Load($reader)
   return ,$document
  }finally{$reader.Dispose()}
 }catch{Throw-Guard $Code "$Label is not valid XML: $($_.Exception.Message)"}
}

if(-not(Test-Path -LiteralPath $BaDir -PathType Container)){Throw-Guard 'GUARD101' "Extracted BA directory not found: $BaDir"}
$root=(Resolve-Path -LiteralPath $BaDir).Path
$data=Read-GuardXml (Join-Path $root 'BootstrapperApplicationData.xml') 'GUARD101' 'BootstrapperApplicationData.xml'
$registrations=@($data.SelectNodes("//*[local-name()='WixBalBAFunctions']"))
if($registrations.Count -ne 1){Throw-Guard 'GUARD101' "Expected exactly one WixBalBAFunctions registration; found $($registrations.Count)."}
if($registrations[0].GetAttribute('FilePath') -cne $guardName){Throw-Guard 'GUARD101' "WixBalBAFunctions FilePath must strictly equal $guardName."}

$guardPath=Join-Path $root $guardName
if(-not(Test-Path -LiteralPath $guardPath -PathType Leaf)){Throw-Guard 'GUARD102' "Guard payload is not a file: $guardName"}
$manifest=Read-GuardXml (Join-Path $root 'manifest.xml') 'GUARD102' 'manifest.xml'
$payloads=@($manifest.SelectNodes("//*[local-name()='Payload']")|Where-Object{$_.GetAttribute('FilePath') -ceq $guardName})
if($payloads.Count -ne 1){Throw-Guard 'GUARD102' "Manifest must contain exactly one guard Payload; found $($payloads.Count)."}
$sha256=(Get-FileHash -LiteralPath $guardPath -Algorithm SHA256).Hash.ToLowerInvariant()

if($ExpectedGuardDll){
 if(-not(Test-Path -LiteralPath $ExpectedGuardDll -PathType Leaf)){Throw-Guard 'GUARD103' "Expected guard DLL is not a file: $ExpectedGuardDll"}
 $expectedHash=(Get-FileHash -LiteralPath $ExpectedGuardDll -Algorithm SHA256).Hash.ToLowerInvariant()
 if($sha256 -cne $expectedHash){Throw-Guard 'GUARD103' "Embedded guard SHA256 $sha256 does not match expected $expectedHash."}
}

$pe=[IO.File]::ReadAllBytes($guardPath)
if($pe.Length -lt 64 -or $pe[0] -ne 0x4d -or $pe[1] -ne 0x5a){Throw-Guard 'GUARD104' 'Guard DLL does not have a bounded DOS MZ header.'}
$peOffset=[long][BitConverter]::ToUInt32($pe,0x3c)
if($peOffset -lt 64 -or $peOffset -gt $pe.Length-24 -or $pe[$peOffset] -ne 0x50 -or
 $pe[$peOffset+1] -ne 0x45 -or $pe[$peOffset+2] -ne 0 -or $pe[$peOffset+3] -ne 0){
 Throw-Guard 'GUARD104' 'Guard DLL PE signature is invalid or out of bounds.'
}
$machine=[BitConverter]::ToUInt16($pe,[int]$peOffset+4)
$sections=[BitConverter]::ToUInt16($pe,[int]$peOffset+6)
$optionalSize=[BitConverter]::ToUInt16($pe,[int]$peOffset+20)
$characteristics=[BitConverter]::ToUInt16($pe,[int]$peOffset+22)
$optionalOffset=$peOffset+24
if($machine -ne 0x8664 -or $sections -eq 0 -or $optionalSize -lt 112 -or
 $optionalOffset+$optionalSize+($sections*40) -gt $pe.Length -or
 [BitConverter]::ToUInt16($pe,[int]$optionalOffset) -ne 0x020b -or ($characteristics -band 0x2000) -eq 0){
 Throw-Guard 'GUARD104' 'Guard DLL must contain a bounded x64 PE DLL header (machine 0x8664).'
}
[ordered]@{guard_payload_verified=$true;sha256=$sha256;registrationCount=$registrations.Count}|ConvertTo-Json -Compress
