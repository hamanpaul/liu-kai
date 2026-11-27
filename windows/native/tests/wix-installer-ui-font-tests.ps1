#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ThemeFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

try {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class LiuKaiWixUiGdi {
    [DllImport("gdi32.dll", SetLastError=true)] public static extern IntPtr CreateCompatibleDC(IntPtr hdc);
    [DllImport("gdi32.dll", SetLastError=true)] public static extern bool DeleteDC(IntPtr hdc);
    [DllImport("gdi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr CreateFontW(int height,int width,int escape,int orientation,int weight,uint italic,uint underline,uint strikeout,uint charset,uint outputPrecision,uint clipPrecision,uint quality,uint pitchAndFamily,string face);
    [DllImport("gdi32.dll", SetLastError=true)] public static extern IntPtr SelectObject(IntPtr hdc,IntPtr obj);
    [DllImport("gdi32.dll", SetLastError=true)] public static extern bool DeleteObject(IntPtr obj);
    [DllImport("gdi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern int GetTextFaceW(IntPtr hdc,int count,StringBuilder face);
    [DllImport("gdi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern uint GetGlyphIndicesW(IntPtr hdc,string text,int count,[Out] ushort[] indices,uint flags);
}
'@
}
catch {
    throw "[FONT002] Could not load the native GDI bindings: $($_.Exception.Message)"
}

function Throw-WixUiGdiError {
    param([string]$Operation)
    $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    throw "[FONT002] $Operation failed (Win32 error $code)."
}

function Get-WixUiFontResult {
    param(
        [Parameter(Mandatory = $true)][Xml.XmlElement]$FontNode,
        [Parameter(Mandatory = $true)][string]$Sample
    )

    $height = -12
    $weight = 500
    if ($FontNode.HasAttribute('Height') -and -not [int]::TryParse($FontNode.GetAttribute('Height'), [ref]$height)) {
        throw "[FONT003] Font '$($FontNode.GetAttribute('Id'))' has an invalid Height."
    }
    if ($FontNode.HasAttribute('Weight') -and -not [int]::TryParse($FontNode.GetAttribute('Weight'), [ref]$weight)) {
        throw "[FONT003] Font '$($FontNode.GetAttribute('Id'))' has an invalid Weight."
    }
    $requestedFace = $FontNode.InnerText.Trim()
    if ([string]::IsNullOrWhiteSpace($requestedFace)) {
        throw "[FONT003] Font '$($FontNode.GetAttribute('Id'))' has no face name."
    }

    $dc = [LiuKaiWixUiGdi]::CreateCompatibleDC([IntPtr]::Zero)
    if ($dc -eq [IntPtr]::Zero) { Throw-WixUiGdiError 'CreateCompatibleDC' }
    $fontHandle = [IntPtr]::Zero
    $previousFont = [IntPtr]::Zero
    try {
        $fontHandle = [LiuKaiWixUiGdi]::CreateFontW($height, 0, 0, 0, $weight, 0, 0, 0, 1, 0, 0, 2, 0, $requestedFace)
        if ($fontHandle -eq [IntPtr]::Zero) { Throw-WixUiGdiError 'CreateFontW' }
        $previousFont = [LiuKaiWixUiGdi]::SelectObject($dc, $fontHandle)
        if ($previousFont -eq [IntPtr]::Zero) { Throw-WixUiGdiError 'SelectObject' }

        $faceBuffer = [Text.StringBuilder]::new(256)
        $faceLength = [LiuKaiWixUiGdi]::GetTextFaceW($dc, $faceBuffer.Capacity, $faceBuffer)
        if ($faceLength -le 0) { Throw-WixUiGdiError 'GetTextFaceW' }

        $markNonexistingGlyphs = 0x0001
        $indices = [UInt16[]]::new($Sample.Length)
        $returned = [LiuKaiWixUiGdi]::GetGlyphIndicesW($dc, $Sample, $Sample.Length, $indices, $markNonexistingGlyphs)
        if ($returned -ne $Sample.Length) { Throw-WixUiGdiError 'GetGlyphIndicesW' }
        $missing = @()
        for ($index = 0; $index -lt $indices.Length; $index++) {
            if ($indices[$index] -eq 0xffff) {
                $missing += 'U+{0:X4}' -f [int][char]$Sample[$index]
            }
        }
        [pscustomobject]@{
            font_id        = $FontNode.GetAttribute('Id')
            requested_face = $requestedFace
            actual_face    = $faceBuffer.ToString()
            height         = $height
            weight         = $weight
            sample_count   = $Sample.Length
            missing_count  = $missing.Count
            missing        = $missing
        }
    }
    finally {
        if ($previousFont -ne [IntPtr]::Zero) { [void][LiuKaiWixUiGdi]::SelectObject($dc, $previousFont) }
        if ($fontHandle -ne [IntPtr]::Zero) { [void][LiuKaiWixUiGdi]::DeleteObject($fontHandle) }
        [void][LiuKaiWixUiGdi]::DeleteDC($dc)
    }
}

try {
    $themePath = (Resolve-Path -LiteralPath $ThemeFile).Path
    $settings = [Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $reader = [Xml.XmlReader]::Create($themePath, $settings)
    try {
        $theme = [Xml.XmlDocument]::new()
        $theme.XmlResolver = $null
        $theme.Load($reader)
    }
    finally {
        $reader.Dispose()
    }
    if ($theme.DocumentElement.LocalName -cne 'Theme' -or
        $theme.DocumentElement.NamespaceURI -cne 'http://wixtoolset.org/schemas/v4/thmutil') {
        throw '[FONT003] ThemeFile is not a WiX v4 thmutil document.'
    }
    $fonts = @($theme.SelectNodes("/*[local-name()='Theme']/*[local-name()='Font']"))
    if ($fonts.Count -eq 0) { throw '[FONT003] Theme contains no Font nodes.' }
}
catch {
    throw "[FONT003] Could not parse ThemeFile '$ThemeFile': $($_.Exception.Message)"
}

$sample = -join @([char]0x5638, [char]0x6539, [char]0x5b89, [char]0x88dd, [char]0x53d6, [char]0x6d88)
$results = @($fonts | ForEach-Object { Get-WixUiFontResult -FontNode $_ -Sample $sample })
[pscustomobject]@{
    theme_file   = $themePath
    sample       = @('U+5638', 'U+6539', 'U+5B89', 'U+88DD', 'U+53D6', 'U+6D88')
    fonts        = $results
    glyphs_valid = -not @($results | Where-Object { $_.missing_count -gt 0 })
} | ConvertTo-Json -Depth 4

$invalid = @($results | Where-Object { $_.missing_count -gt 0 })
if ($invalid.Count -gt 0) {
    $detail = @($invalid | ForEach-Object {
        "FontId=$($_.font_id) requested='$($_.requested_face)' actual='$($_.actual_face)' missing=$($_.missing_count)/$($_.sample_count)"
    }) -join '; '
    throw "[FONT001] Theme fonts do not cover the required CJK sample: $detail"
}
