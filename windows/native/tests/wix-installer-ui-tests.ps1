#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string[]]$BaDir,

    [string]$SourceTheme,
    [string]$SourceLocalization,
    [string[]]$BundleAuthoring,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:LocReferencePattern = '[#!]\(loc\.([A-Za-z0-9_.-]+)\)'
$script:LocReferenceStartPattern = '[#!]\(loc\.'
$script:ErrorCodeVariablePattern = '(?i)\[(?:WixStdBA)?[A-Za-z0-9_.]*(?:ErrorCode|HRESULT)[A-Za-z0-9_.]*\]'
$script:ErrorCodeConditionPattern = '(?i)(?:ErrorCode|HRESULT)'
$script:RequiredLocalizationIds = @(
    'InstallAcceptCheckbox',
    'ProgressHeader',
    'ProgressLabel',
    'ProgressActionLabel',
    'ProgressActionFallback',
    'ProgressWaitText',
    'ProgressCancelButton',
    'SuccessRestartText',
    'SuccessUninstallRestartText',
    'SuccessRestartButton',
    'SuccessCloseButton',
    'FailureRestartText',
    'FailureRestartButton',
    'FailureCloseButton',
    'FailureInstallerBusyText',
    'FilesInUseTitle',
    'FilesInUseLabel',
    'FilesInUseNetfxCloseRadioButton',
    'FilesInUseCloseRadioButton',
    'FilesInUseDontCloseRadioButton',
    'FilesInUseRetryButton',
    'FilesInUseIgnoreButton',
    'FilesInUseExitButton'
)

function Add-WixUiIssue {
    param(
        [System.Collections.Generic.List[object]]$Issues,
        [string]$Code,
        [string]$Message
    )

    $Issues.Add([pscustomobject]@{
        Code    = $Code
        Message = $Message
    }) | Out-Null
}

function Read-WixUiXml {
    [OutputType([System.Xml.XmlDocument])]
    param([Parameter(Mandatory = $true)][string]$Path)

    $settings = [System.Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $reader = [System.Xml.XmlReader]::Create($Path, $settings)
    try {
        $document = [System.Xml.XmlDocument]::new()
        $document.XmlResolver = $null
        $document.PreserveWhitespace = $false
        $document.Load($reader)
        return ,$document
    }
    finally {
        $reader.Dispose()
    }
}

function Write-WixUiXml {
    param(
        [Parameter(Mandatory = $true)][System.Xml.XmlDocument]$Document,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $settings = [System.Xml.XmlWriterSettings]::new()
    $settings.Encoding = [System.Text.UTF8Encoding]::new($false)
    $settings.Indent = $true
    $settings.IndentChars = '  '
    $writer = [System.Xml.XmlWriter]::Create($Path, $settings)
    try {
        $Document.Save($writer)
    }
    finally {
        $writer.Dispose()
    }
}

function Get-WixUiLocReferences {
    param([AllowEmptyString()][string]$Text)
    return @([regex]::Matches($Text, $script:LocReferencePattern, [System.Text.RegularExpressions.RegexOptions]::None))
}

function Get-WixUiXmlStrings {
    param([Parameter(Mandatory = $true)][System.Xml.XmlDocument]$Document)
    foreach ($node in @($Document.SelectNodes('//@* | //text()'))) {
        [string]$node.Value
    }
}

function Test-WixUiNodeVisible {
    param([Parameter(Mandatory = $true)][System.Xml.XmlElement]$Node)

    $visible = $Node.GetAttribute('Visible')
    $width = $Node.GetAttribute('Width')
    $height = $Node.GetAttribute('Height')
    return -not [string]::IsNullOrEmpty($width) -and
        -not [string]::IsNullOrEmpty($height) -and
        $visible -notin @('no', 'false', '0') -and
        $Node.GetAttribute('Hidden') -notin @('yes', 'true', '1') -and
        $width -cne '0' -and
        $height -cne '0'
}

function Test-WixUiNodeLocReference {
    param(
        [Parameter(Mandatory = $true)][System.Xml.XmlElement]$Node,
        [Parameter(Mandatory = $true)][string]$Id
    )

    $pattern = '(?:^|[^A-Za-z0-9_.])[#!]\(loc\.' + [regex]::Escape($Id) + '\)'
    foreach ($candidate in @($Node.SelectNodes('.//@* | .//text()'))) {
        if ([regex]::IsMatch([string]$candidate.Value, $pattern)) {
            return $true
        }
    }
    return $false
}

function Get-WixUiVisibleButtonText {
    param([AllowEmptyString()][string]$Text)

    $withoutParenthesizedAccelerator = [regex]::Replace($Text, '\s*\(&[A-Za-z0-9]\)', '')
    return $withoutParenthesizedAccelerator.Replace('&', '').Trim()
}

function Test-WixUiRestartTextCoversUninstallActions {
    param(
        [Parameter(Mandatory = $true)][System.Xml.XmlElement]$Label,
        [Parameter(Mandatory = $true)][string]$LocalizationId
    )

    $coversUnsafeUninstall = $false
    $coversNormalUninstall = $false
    foreach ($text in @($Label.SelectNodes("./*[local-name()='Text']"))) {
        if (-not (Test-WixUiNodeLocReference $text $LocalizationId)) {
            continue
        }
        $condition = [regex]::Replace($text.GetAttribute('Condition').Trim(), '\s+', ' ')
        $condition = [regex]::Replace($condition, '\s*=\s*', ' = ')
        switch ($condition) {
            'WixBundleAction = 3' {
                $coversUnsafeUninstall = $true
            }
            'WixBundleAction = 4' {
                $coversNormalUninstall = $true
            }
            'WixBundleAction = 3 OR WixBundleAction = 4' {
                $coversUnsafeUninstall = $true
                $coversNormalUninstall = $true
            }
            'WixBundleAction = 4 OR WixBundleAction = 3' {
                $coversUnsafeUninstall = $true
                $coversNormalUninstall = $true
            }
            '(WixBundleAction = 3 OR WixBundleAction = 4)' {
                $coversUnsafeUninstall = $true
                $coversNormalUninstall = $true
            }
            '(WixBundleAction = 4 OR WixBundleAction = 3)' {
                $coversUnsafeUninstall = $true
                $coversNormalUninstall = $true
            }
        }
    }
    return $coversUnsafeUninstall -and $coversNormalUninstall
}

function Resolve-WixUiLocValue {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Values,
        [Parameter(Mandatory = $true)][string]$Id,
        [string[]]$Trail = @()
    )

    if (-not $Values.ContainsKey($Id)) {
        throw "Unknown localization '$Id'."
    }
    if ($Trail -contains $Id) {
        throw "Localization reference cycle: $($Trail + $Id -join ' -> ')."
    }

    $result = [string]$Values[$Id]
    $nextTrail = @($Trail) + $Id
    $matches = @(Get-WixUiLocReferences $result)
    for ($index = $matches.Count - 1; $index -ge 0; $index--) {
        $match = $matches[$index]
        $replacement = Resolve-WixUiLocValue -Values $Values -Id $match.Groups[1].Value -Trail $nextTrail
        $result = $result.Substring(0, $match.Index) + $replacement + $result.Substring($match.Index + $match.Length)
    }
    return $result
}

function Test-WixUiControl {
    param(
        [Parameter(Mandatory = $true)][System.Xml.XmlElement]$Page,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$LocalName
    )

    $nodes = @($Page.SelectNodes(".//*[@Name='$Name']"))
    return $nodes.Count -eq 1 -and $nodes[0].LocalName -ceq $LocalName -and (Test-WixUiNodeVisible $nodes[0])
}

function Get-WixUiPage {
    param(
        [Parameter(Mandatory = $true)][System.Xml.XmlDocument]$Theme,
        [Parameter(Mandatory = $true)][string]$Name
    )

    return $Theme.SelectSingleNode("/*[local-name()='Theme']/*[local-name()='Window']/*[local-name()='Page' and @Name='$Name']")
}

function Get-WixUiIssues {
    param([Parameter(Mandatory = $true)][string]$Directory)

    $issues = [System.Collections.Generic.List[object]]::new()
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        Add-WixUiIssue $issues 'UI001' "Extracted BA directory not found: $Directory"
        return @($issues.ToArray())
    }

    $baPath = (Resolve-Path -LiteralPath $Directory).Path
    $paths = @{}
    foreach ($name in @('manifest.xml', 'thm.xml', 'thm.wxl', 'logo.png', 'license.rtf')) {
        $candidate = Join-Path $baPath $name
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            Add-WixUiIssue $issues 'UI001' "Required extracted BA resource is missing: $name"
            continue
        }
        $paths[$name] = $candidate
    }
    if ($issues.Count -gt 0) {
        return @($issues.ToArray())
    }

    try {
        $manifest = Read-WixUiXml $paths['manifest.xml']
        $theme = Read-WixUiXml $paths['thm.xml']
        $localization = Read-WixUiXml $paths['thm.wxl']
    }
    catch {
        Add-WixUiIssue $issues 'UI002' "Extracted BA XML cannot be parsed with DTD/network resolution disabled: $($_.Exception.Message)"
        return @($issues.ToArray())
    }

    if ($theme.DocumentElement.LocalName -cne 'Theme' -or $theme.DocumentElement.NamespaceURI -cne 'http://wixtoolset.org/schemas/v4/thmutil') {
        Add-WixUiIssue $issues 'UI002' 'thm.xml must be a WiX v4 thmutil Theme document.'
    }
    if ($localization.DocumentElement.LocalName -cne 'WixLocalization' -or $localization.DocumentElement.NamespaceURI -cne 'http://wixtoolset.org/schemas/v4/wxl') {
        Add-WixUiIssue $issues 'UI002' 'thm.wxl must be a WiX v4 WixLocalization document.'
    }
    $uxPayloads = @($manifest.SelectNodes("//*[local-name()='UX']/*[local-name()='Payload']/@FilePath"))
    $embeddedFiles = @($uxPayloads | ForEach-Object { [string]$_.Value })
    foreach ($name in @('thm.xml', 'thm.wxl', 'logo.png', 'license.rtf')) {
        if (@($embeddedFiles | Where-Object { $_ -ceq $name }).Count -ne 1) {
            Add-WixUiIssue $issues 'UI003' "Burn UX manifest must embed exactly one $name payload."
        }
    }
    if (@($embeddedFiles | Where-Object { $_ -match '(?i)\.wxl$' }).Count -ne 1) {
        Add-WixUiIssue $issues 'UI003' 'Burn UX must embed exactly one WXL localization resource.'
    }
    if (@($embeddedFiles | Where-Object { $_ -match '(?i)(?:^|[\\/])BAFunctions.*\.dll$' }).Count -gt 0) {
        Add-WixUiIssue $issues 'UI036' 'The installer UI must not introduce a BAFunctions DLL for error-code text.'
    }
    if ($issues.Count -gt 0) {
        return @($issues.ToArray())
    }

    $localizationValues = [hashtable]::new([System.StringComparer]::Ordinal)
    foreach ($string in @($localization.SelectNodes("/*[local-name()='WixLocalization']/*[local-name()='String']"))) {
        $id = $string.GetAttribute('Id')
        if ([string]::IsNullOrWhiteSpace($id) -or -not $string.HasAttribute('Value')) {
            Add-WixUiIssue $issues 'UI011' 'Each localization String must have a non-empty Id and an explicit Value.'
            continue
        }
        if ($localizationValues.ContainsKey($id)) {
            Add-WixUiIssue $issues 'UI011' "Localization Id '$id' is duplicated in thm.wxl."
            continue
        }
        $localizationValues[$id] = $string.GetAttribute('Value')
    }

    $progress = Get-WixUiPage $theme 'Progress'
    $actionNodes = @($theme.SelectNodes("//*[@Name='ExecuteProgressActionDataText']"))
    $action = $null
    if ($null -eq $progress -or $actionNodes.Count -ne 1 -or $actionNodes[0].LocalName -cne 'Label') {
        Add-WixUiIssue $issues 'UI020' "Progress page must contain exactly one visible Label named 'ExecuteProgressActionDataText'."
    }
    else {
        $action = $actionNodes[0]
        $actionPage = $action.SelectSingleNode("ancestor::*[local-name()='Page'][1]")
        if ($null -eq $actionPage -or $actionPage.GetAttribute('Name') -cne 'Progress' -or
            -not (Test-WixUiNodeVisible $action) -or $action.HasAttribute('VisibleCondition') -or
            -not (Test-WixUiNodeLocReference $action 'ProgressActionFallback')) {
            Add-WixUiIssue $issues 'UI020' "ExecuteProgressActionDataText must be a directly usable, visible Progress Label with a localized generic fallback."
        }
    }
    if (@($issues | Where-Object { $_.Code -ceq 'UI020' }).Count -gt 0) {
        return @($issues.ToArray())
    }

    foreach ($document in @($theme, $localization)) {
        foreach ($value in @(Get-WixUiXmlStrings $document)) {
            $matches = @(Get-WixUiLocReferences $value)
            $starts = [regex]::Matches($value, $script:LocReferenceStartPattern, [System.Text.RegularExpressions.RegexOptions]::None).Count
            if ($starts -ne $matches.Count) {
                Add-WixUiIssue $issues 'UI010' "Malformed localization reference in extracted XML: $value"
            }
            foreach ($match in $matches) {
                $id = $match.Groups[1].Value
                if (-not $localizationValues.ContainsKey($id)) {
                    Add-WixUiIssue $issues 'UI010' "Unresolved extracted localization reference: $($match.Value)"
                }
            }
            if ([regex]::IsMatch($value, $script:ErrorCodeVariablePattern)) {
                Add-WixUiIssue $issues 'UI036' "Error-code theme variables are forbidden; found '$value'."
            }
        }
    }
    $conditionAttributes = @($theme.SelectNodes('//@Condition | //@VisibleCondition | //@EnableCondition | //@DisableCondition'))
    foreach ($attribute in $conditionAttributes) {
        if ([regex]::IsMatch([string]$attribute.Value, $script:ErrorCodeConditionPattern)) {
            Add-WixUiIssue $issues 'UI036' "Error-code conditions are forbidden in the theme: $($attribute.Value)"
        }
    }
    foreach ($id in @($localizationValues.Keys)) {
        try {
            Resolve-WixUiLocValue -Values $localizationValues -Id $id | Out-Null
        }
        catch {
            Add-WixUiIssue $issues 'UI010' $_.Exception.Message
        }
    }

    if ($localization.DocumentElement.GetAttribute('Culture') -cne 'zh-TW' -or
        $localization.DocumentElement.GetAttribute('Language') -cne '1028') {
        Add-WixUiIssue $issues 'UI011' 'The single embedded localization must be Traditional Chinese (Culture=zh-TW, Language=1028).'
    }

    $resolved = @{}
    foreach ($id in $script:RequiredLocalizationIds) {
        if (-not $localizationValues.ContainsKey($id)) {
            Add-WixUiIssue $issues 'UI034' "Required localization String '$id' is missing from thm.wxl."
            continue
        }
        try {
            $resolved[$id] = Resolve-WixUiLocValue -Values $localizationValues -Id $id
            if ([string]::IsNullOrWhiteSpace([string]$resolved[$id])) {
                Add-WixUiIssue $issues 'UI034' "Required localization String '$id' is empty."
            }
        }
        catch {
            Add-WixUiIssue $issues 'UI034' "Required localization String '$id' cannot be resolved: $($_.Exception.Message)"
        }
    }

    $expectedText = @{
        ProgressActionLabel    = 'Windows Installer 目前作業'
        ProgressActionFallback = '正在準備或執行安裝作業'
        ProgressWaitText       = '驗證或移除舊版可能需等待，請勿重複啟動安裝'
        FailureInstallerBusyText = '若顯示 1618／0x80070652，請等待其他安裝完成，再重新開啟本程式'
        SuccessRestartText     = '安裝完成，需要重新啟動以完成舊檔清理'
    }
    foreach ($entry in $expectedText.GetEnumerator()) {
        if ($resolved.ContainsKey($entry.Key) -and [string]$resolved[$entry.Key] -cne $entry.Value) {
            Add-WixUiIssue $issues 'UI035' "Localization '$($entry.Key)' must equal '$($entry.Value)'."
        }
    }
    if (-not $localizationValues.ContainsKey('SuccessCloseRestartLaterButton')) {
        Add-WixUiIssue $issues 'UI037' "Localization String 'SuccessCloseRestartLaterButton' is missing for the conditional later-restart close label."
    }
    else {
        try {
            $resolved['SuccessCloseRestartLaterButton'] = Resolve-WixUiLocValue -Values $localizationValues -Id 'SuccessCloseRestartLaterButton'
        }
        catch {
            Add-WixUiIssue $issues 'UI037' "Conditional later-restart close localization cannot be resolved: $($_.Exception.Message)"
        }
    }
    $filesInUseText = @(
        $resolved['FilesInUseLabel'],
        $resolved['FilesInUseCloseRadioButton'],
        $resolved['FilesInUseDontCloseRadioButton'],
        $resolved['FilesInUseRetryButton']
    ) -join ' '
    foreach ($phrase in @('自行儲存', '重試', '稍後重新啟動')) {
        if ($filesInUseText -notmatch [regex]::Escape($phrase)) {
            Add-WixUiIssue $issues 'UI035' "FilesInUse localized UI must retain the native self-save/retry/defer choice '$phrase'."
        }
    }

    $progressActionLabel = @($progress.SelectNodes(".//*[local-name()='Label']") | Where-Object {
        Test-WixUiNodeLocReference $_ 'ProgressActionLabel'
    })
    $progressWait = @($progress.SelectNodes(".//*[local-name()='Label']") | Where-Object {
        Test-WixUiNodeLocReference $_ 'ProgressWaitText'
    })
    if ($progressActionLabel.Count -ne 1 -or -not (Test-WixUiNodeVisible $progressActionLabel[0]) -or
        [object]::ReferenceEquals($progressActionLabel[0], $action) -or
        $progressWait.Count -ne 1 -or -not (Test-WixUiNodeVisible $progressWait[0]) -or
        [object]::ReferenceEquals($progressWait[0], $action) -or
        [object]::ReferenceEquals($progressActionLabel[0], $progressWait[0])) {
        Add-WixUiIssue $issues 'UI021' 'Progress must visibly show the Windows Installer action heading and the static wait-without-rerunning-install explanation.'
    }
    $progressCancel = $progress.SelectSingleNode(".//*[@Name='ProgressCancelButton']")
    if (-not (Test-WixUiControl $progress 'ProgressCancelButton' 'Button') -or
        -not (Test-WixUiNodeLocReference $progressCancel 'ProgressCancelButton') -or
        @($progressCancel.SelectNodes("./*[local-name() != 'Text']")).Count -ne 0) {
        Add-WixUiIssue $issues 'UI022' 'ProgressCancelButton must remain the native visible cancel button on Progress.'
    }

    $install = Get-WixUiPage $theme 'Install'
    $accept = if ($null -ne $install) {
        $install.SelectSingleNode(".//*[@Name='EulaAcceptCheckbox']")
    }
    else {
        $null
    }
    if ($null -eq $install -or -not (Test-WixUiControl $install 'EulaRichedit' 'Richedit') -or
        -not (Test-WixUiControl $install 'EulaAcceptCheckbox' 'Checkbox') -or
        -not (Test-WixUiNodeLocReference $accept 'InstallAcceptCheckbox')) {
        Add-WixUiIssue $issues 'UI030' 'Install must retain both the notice text and the visible EulaAcceptCheckbox.'
    }
    $noticeText = [System.IO.File]::ReadAllText($paths['license.rtf'])
    if ((Get-Item -LiteralPath $paths['license.rtf']).Length -eq 0 -or $noticeText -notmatch '^\s*\{\\rtf') {
        Add-WixUiIssue $issues 'UI030' 'Embedded license.rtf must be a non-empty RTF installation notice.'
    }

    $logoNodes = @($theme.SelectNodes("//*[local-name()='ImageControl' and @ImageFile='logo.png']"))
    if ($logoNodes.Count -ne 1 -or -not (Test-WixUiNodeVisible $logoNodes[0])) {
        Add-WixUiIssue $issues 'UI031' 'Theme must display exactly one visible embedded logo.png ImageControl.'
    }
    $logoBytes = [System.IO.File]::ReadAllBytes($paths['logo.png'])
    if ($logoBytes.Length -lt 8 -or
        $logoBytes[0] -ne 0x89 -or $logoBytes[1] -ne 0x50 -or
        $logoBytes[2] -ne 0x4e -or $logoBytes[3] -ne 0x47) {
        Add-WixUiIssue $issues 'UI031' 'Embedded logo.png is not a valid PNG resource.'
    }

    $failure = Get-WixUiPage $theme 'Failure'
    if ($null -eq $failure) {
        Add-WixUiIssue $issues 'UI032' 'Failure page is missing.'
    }
    else {
        $failureMessages = @($failure.SelectNodes(".//*[@Name='FailureMessageText']"))
        if ($failureMessages.Count -ne 1 -or $failureMessages[0].LocalName -cne 'Hypertext' -or
            -not (Test-WixUiNodeVisible $failureMessages[0]) -or
            -not [string]::IsNullOrWhiteSpace($failureMessages[0].InnerText)) {
            Add-WixUiIssue $issues 'UI032' 'FailureMessageText must remain the empty native Hypertext populated with the real HRESULT/system explanation.'
        }
        $busyLabels = @($failure.SelectNodes(".//*[local-name()='Label']") | Where-Object {
            Test-WixUiNodeLocReference $_ 'FailureInstallerBusyText'
        })
        if ($busyLabels.Count -ne 1 -or -not (Test-WixUiNodeVisible $busyLabels[0]) -or
            $busyLabels[0].HasAttribute('Condition') -or $busyLabels[0].HasAttribute('VisibleCondition') -or
            @($busyLabels[0].SelectNodes(".//*[@Condition or @VisibleCondition]")).Count -gt 0) {
            Add-WixUiIssue $issues 'UI035' 'The 1618/0x80070652 guidance must be a visible, static Failure label without an error-code condition.'
        }
    }

    foreach ($pageName in @('Success', 'Failure')) {
        $page = Get-WixUiPage $theme $pageName
        $restartButtonName = $pageName + 'RestartButton'
        $closeButtonName = $pageName + 'CloseButton'
        $restartTextId = $pageName + 'RestartText'
        if ($null -eq $page) {
            continue
        }
        $restartLabels = @($page.SelectNodes(".//*[@VisibleCondition='WixStdBARestartRequired']"))
        $restartTextLabels = @($restartLabels | Where-Object {
            Test-WixUiNodeLocReference $_ $restartTextId
        })
        $restartButtons = @($page.SelectNodes(".//*[@Name='$restartButtonName']"))
        $closeButtons = @($page.SelectNodes(".//*[@Name='$closeButtonName']"))
        if ($restartTextLabels.Count -ne 1 -or $restartLabels.Count -ne 1 -or
            $restartTextLabels[0].LocalName -cne 'Label' -or -not (Test-WixUiNodeVisible $restartTextLabels[0]) -or
            $restartButtons.Count -ne 1 -or $restartButtons[0].LocalName -cne 'Button' -or
            $restartButtons[0].GetAttribute('HideWhenDisabled') -cne 'yes' -or
            -not (Test-WixUiNodeLocReference $restartButtons[0] $restartButtonName) -or
            $closeButtons.Count -ne 1 -or $closeButtons[0].LocalName -cne 'Button' -or
            -not (Test-WixUiNodeVisible $closeButtons[0]) -or
            -not (Test-WixUiNodeLocReference $closeButtons[0] $closeButtonName) -or
            @($closeButtons[0].SelectNodes("./*[local-name()='CloseWindowAction']")).Count -ne 1) {
            Add-WixUiIssue $issues 'UI033' "$pageName restart/close controls must retain the WixStdBARestartRequired condition and native control names."
        }

        if ($pageName -ceq 'Success') {
            $closeTextNodes = @()
            $closeDefaultTexts = @()
            $closeRestartLaterTexts = @()
            if ($closeButtons.Count -eq 1) {
                $closeTextNodes = @($closeButtons[0].SelectNodes("./*[local-name()='Text']"))
                $closeDefaultTexts = @($closeButtons[0].SelectNodes("./*[local-name()='Text' and not(@Condition)]") | Where-Object {
                    Test-WixUiNodeLocReference $_ 'SuccessCloseButton'
                })
                $closeRestartLaterTexts = @($closeButtons[0].SelectNodes("./*[local-name()='Text' and @Condition='WixStdBARestartRequired']") | Where-Object {
                    Test-WixUiNodeLocReference $_ 'SuccessCloseRestartLaterButton'
                })
            }
            $defaultCloseRawText = if ($resolved.ContainsKey('SuccessCloseButton')) {
                [string]$resolved['SuccessCloseButton']
            }
            else {
                ''
            }
            $restartLaterCloseRawText = if ($resolved.ContainsKey('SuccessCloseRestartLaterButton')) {
                [string]$resolved['SuccessCloseRestartLaterButton']
            }
            else {
                ''
            }
            $defaultCloseText = Get-WixUiVisibleButtonText $defaultCloseRawText
            $restartLaterCloseText = Get-WixUiVisibleButtonText $restartLaterCloseRawText
            if ($closeButtons.Count -ne 1 -or $closeTextNodes.Count -ne 2 -or
                $closeDefaultTexts.Count -ne 1 -or $closeRestartLaterTexts.Count -ne 1 -or
                $defaultCloseText -cne '關閉' -or $restartLaterCloseText -cne '稍後重啟' -or
                -not [regex]::IsMatch($defaultCloseRawText, '&[A-Za-z0-9]') -or
                -not [regex]::IsMatch($restartLaterCloseRawText, '&[A-Za-z0-9]')) {
                Add-WixUiIssue $issues 'UI037' "SuccessCloseButton must have an unconditional generic '關閉' default and a Text with Condition='WixStdBARestartRequired' pointing to SuccessCloseRestartLaterButton ('稍後重啟')."
            }

            $coversBothUninstallActions = $false
            if ($restartTextLabels.Count -eq 1) {
                $coversBothUninstallActions = Test-WixUiRestartTextCoversUninstallActions `
                    -Label $restartTextLabels[0] `
                    -LocalizationId 'SuccessUninstallRestartText'
            }
            if (-not $coversBothUninstallActions) {
                Add-WixUiIssue $issues 'UI037' 'SuccessUninstallRestartText must cover normal uninstall action 4 as well as unsafe uninstall action 3, either in one explicit OR condition or two Text nodes.'
            }
        }
    }

    return @($issues.ToArray())
}

function Assert-WixUiValid {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $issues = @(Get-WixUiIssues $Directory)
    if ($issues.Count -gt 0) {
        $details = @($issues | ForEach-Object { "[$($_.Code)] $($_.Message)" }) -join [Environment]::NewLine
        throw "$Label failed:$([Environment]::NewLine)$details"
    }
}

function Get-WixUiSharedBindingIssues {
    param(
        [Parameter(Mandatory = $true)][string[]]$Directories,
        [Parameter(Mandatory = $true)][string]$ThemeSource,
        [Parameter(Mandatory = $true)][string]$LocalizationSource,
        [Parameter(Mandatory = $true)][string[]]$Authoring
    )

    $issues = [System.Collections.Generic.List[object]]::new()
    $distinctDirectories = @($Directories | ForEach-Object { (Resolve-Path -LiteralPath $_).Path } | Select-Object -Unique)
    if ($Directories.Count -ne 2 -or $distinctDirectories.Count -ne 2) {
        Add-WixUiIssue $issues 'UI050' 'Final source binding requires two distinct extracted BA directories, one per bundle authoring.'
    }
    $resolvedAuthoring = @()
    foreach ($authoringPath in $Authoring) {
        if (Test-Path -LiteralPath $authoringPath -PathType Leaf) {
            $resolvedAuthoring += (Resolve-Path -LiteralPath $authoringPath).Path
        }
        else {
            Add-WixUiIssue $issues 'UI050' "Bundle authoring file not found: $authoringPath"
        }
    }
    if ($Authoring.Count -ne 2 -or @($resolvedAuthoring | Select-Object -Unique).Count -ne 2) {
        Add-WixUiIssue $issues 'UI050' 'Final source binding requires the two distinct bundle authoring files.'
    }
    if (-not (Test-Path -LiteralPath $ThemeSource -PathType Leaf) -or
        -not (Test-Path -LiteralPath $LocalizationSource -PathType Leaf)) {
        Add-WixUiIssue $issues 'UI050' 'Source theme or localization file does not exist.'
        return @($issues.ToArray())
    }

    $themePath = (Resolve-Path -LiteralPath $ThemeSource).Path
    $localizationPath = (Resolve-Path -LiteralPath $LocalizationSource).Path
    $sourceThemeHash = (Get-FileHash -LiteralPath $themePath -Algorithm SHA256).Hash
    $sourceLocalizationHash = (Get-FileHash -LiteralPath $localizationPath -Algorithm SHA256).Hash
    foreach ($directory in $Directories) {
        $baPath = (Resolve-Path -LiteralPath $directory).Path
        foreach ($binding in @(
            @{ Name = 'thm.xml'; Hash = $sourceThemeHash },
            @{ Name = 'thm.wxl'; Hash = $sourceLocalizationHash }
        )) {
            $embeddedPath = Join-Path $baPath $binding.Name
            if ((Get-FileHash -LiteralPath $embeddedPath -Algorithm SHA256).Hash -cne $binding.Hash) {
                Add-WixUiIssue $issues 'UI050' "$($binding.Name) in '$baPath' differs from the shared source UI resource."
            }
        }
    }

    $themeReferences = @()
    $localizationReferences = @()
    foreach ($authoringPath in $resolvedAuthoring) {
        try {
            $document = Read-WixUiXml (Resolve-Path -LiteralPath $authoringPath).Path
        }
        catch {
            Add-WixUiIssue $issues 'UI050' "Bundle authoring XML cannot be parsed: $($_.Exception.Message)"
            continue
        }
        $applications = @($document.SelectNodes("//*[local-name()='WixStandardBootstrapperApplication']"))
        if ($applications.Count -ne 1) {
            Add-WixUiIssue $issues 'UI050' "Each bundle authoring must contain exactly one WixStandardBootstrapperApplication: $authoringPath"
            continue
        }
        $themeReference = $applications[0].GetAttribute('ThemeFile')
        $localizationReference = $applications[0].GetAttribute('LocalizationFile')
        if ([string]::IsNullOrWhiteSpace($themeReference) -or [string]::IsNullOrWhiteSpace($localizationReference)) {
            Add-WixUiIssue $issues 'UI050' "Bundle authoring must explicitly bind ThemeFile and LocalizationFile: $authoringPath"
            continue
        }
        $themeReferences += $themeReference
        $localizationReferences += $localizationReference
    }
    if ($themeReferences.Count -ne 2 -or @($themeReferences | Select-Object -Unique).Count -ne 1 -or
        $localizationReferences.Count -ne 2 -or @($localizationReferences | Select-Object -Unique).Count -ne 1) {
        Add-WixUiIssue $issues 'UI050' 'Both bundle authoring files must reference the same ThemeFile and LocalizationFile pair.'
    }

    return @($issues.ToArray())
}

function Invoke-WixUiMutationCase {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][scriptblock]$Mutation,
        [Parameter(Mandatory = $true)][string]$ExpectedCode
    )

    $destination = Join-Path $Root $Name
    Copy-Item -LiteralPath $Source -Destination $destination -Recurse | Out-Null
    & $Mutation $destination
    $issues = @(Get-WixUiIssues $destination)
    if ($issues.Count -eq 0 -or @($issues | Where-Object { $_.Code -ceq $ExpectedCode }).Count -eq 0) {
        $actual = @($issues | ForEach-Object { $_.Code }) -join ','
        throw "Mutation '$Name' was not rejected with $ExpectedCode (actual: $actual)."
    }
}

function Invoke-WixUiSelfTest {
    param([Parameter(Mandatory = $true)][string]$PositiveBa)

    $source = (Resolve-Path -LiteralPath $PositiveBa).Path
    Assert-WixUiValid $source 'Positive BA resource fixture'
    $protectedHashes = @{}
    foreach ($name in @('manifest.xml', 'thm.xml', 'thm.wxl', 'logo.png', 'license.rtf')) {
        $protectedHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $source $name) -Algorithm SHA256).Hash
    }

    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("liukai-installer-ui-selftest-" + [Guid]::NewGuid().ToString('N'))
    [System.IO.Directory]::CreateDirectory($tempRoot) | Out-Null
    $mutationCount = 0
    try {
        $cases = @(
            @{
                Name = 'action-moved-off-progress'
                Code = 'UI020'
                Change = {
                    param($dir)
                    $path = Join-Path $dir 'thm.xml'
                    $document = Read-WixUiXml $path
                    $action = $document.SelectSingleNode("//*[@Name='ExecuteProgressActionDataText']")
                    $failure = Get-WixUiPage $document 'Failure'
                    $failure.AppendChild($action) | Out-Null
                    Write-WixUiXml $document $path
                }
            },
            @{
                Name = 'action-hidden'
                Code = 'UI020'
                Change = {
                    param($dir)
                    $path = Join-Path $dir 'thm.xml'
                    $document = Read-WixUiXml $path
                    $document.SelectSingleNode("//*[@Name='ExecuteProgressActionDataText']").SetAttribute('Visible', 'no')
                    Write-WixUiXml $document $path
                }
            },
            @{
                Name = 'restart-condition-wrong'
                Code = 'UI033'
                Change = {
                    param($dir)
                    $path = Join-Path $dir 'thm.xml'
                    $document = Read-WixUiXml $path
                    $success = Get-WixUiPage $document 'Success'
                    $success.SelectSingleNode(".//*[@VisibleCondition='WixStdBARestartRequired']").SetAttribute('VisibleCondition', 'NeverRestart')
                    Write-WixUiXml $document $path
                }
            },
            @{
                Name = 'success-close-restart-condition-removed'
                Code = 'UI037'
                Change = {
                    param($dir)
                    $path = Join-Path $dir 'thm.xml'
                    $document = Read-WixUiXml $path
                    $success = Get-WixUiPage $document 'Success'
                    $closeButton = $success.SelectSingleNode(".//*[@Name='SuccessCloseButton']")
                    $conditionalText = @($closeButton.SelectNodes("./*[local-name()='Text' and @Condition='WixStdBARestartRequired']") | Where-Object {
                        Test-WixUiNodeLocReference $_ 'SuccessCloseRestartLaterButton'
                    })[0]
                    $conditionalText.RemoveAttribute('Condition')
                    Write-WixUiXml $document $path
                }
            },
            @{
                Name = 'normal-uninstall-restart-coverage-removed'
                Code = 'UI037'
                Change = {
                    param($dir)
                    $path = Join-Path $dir 'thm.xml'
                    $document = Read-WixUiXml $path
                    $success = Get-WixUiPage $document 'Success'
                    $restartLabel = @($success.SelectNodes(".//*[@VisibleCondition='WixStdBARestartRequired']") | Where-Object {
                        Test-WixUiNodeLocReference $_ 'SuccessRestartText'
                    })[0]
                    $uninstallTexts = @($restartLabel.SelectNodes("./*[local-name()='Text']") | Where-Object {
                        Test-WixUiNodeLocReference $_ 'SuccessUninstallRestartText'
                    })
                    foreach ($text in $uninstallTexts) {
                        $text.SetAttribute('Condition', 'WixBundleAction = 3')
                    }
                    Write-WixUiXml $document $path
                }
            },
            @{
                Name = 'files-in-use-string-missing'
                Code = 'UI034'
                Change = {
                    param($dir)
                    $path = Join-Path $dir 'thm.wxl'
                    $document = Read-WixUiXml $path
                    $node = $document.SelectSingleNode("/*[local-name()='WixLocalization']/*[local-name()='String' and @Id='FilesInUseDontCloseRadioButton']")
                    $node.ParentNode.RemoveChild($node) | Out-Null
                    Write-WixUiXml $document $path
                }
            },
            @{
                Name = 'unknown-localization-reference'
                Code = 'UI010'
                Change = {
                    param($dir)
                    $path = Join-Path $dir 'thm.xml'
                    $document = Read-WixUiXml $path
                    $progressPage = Get-WixUiPage $document 'Progress'
                    $label = @($progressPage.SelectNodes(".//*[local-name()='Label']") | Where-Object {
                        Test-WixUiNodeLocReference $_ 'ProgressActionLabel'
                    })[0]
                    $label.InnerText = '#(loc.NotDeclaredInstallerUiString)'
                    Write-WixUiXml $document $path
                }
            },
            @{
                Name = 'accept-checkbox-missing'
                Code = 'UI030'
                Change = {
                    param($dir)
                    $path = Join-Path $dir 'thm.xml'
                    $document = Read-WixUiXml $path
                    $node = $document.SelectSingleNode("//*[@Name='EulaAcceptCheckbox']")
                    $node.ParentNode.RemoveChild($node) | Out-Null
                    Write-WixUiXml $document $path
                }
            },
            @{
                Name = 'logo-control-missing'
                Code = 'UI031'
                Change = {
                    param($dir)
                    $path = Join-Path $dir 'thm.xml'
                    $document = Read-WixUiXml $path
                    $node = $document.SelectSingleNode("//*[local-name()='ImageControl' and @ImageFile='logo.png']")
                    $node.ParentNode.RemoveChild($node) | Out-Null
                    Write-WixUiXml $document $path
                }
            },
            @{
                Name = 'native-failure-message-missing'
                Code = 'UI032'
                Change = {
                    param($dir)
                    $path = Join-Path $dir 'thm.xml'
                    $document = Read-WixUiXml $path
                    $node = $document.SelectSingleNode("//*[@Name='FailureMessageText']")
                    $node.ParentNode.RemoveChild($node) | Out-Null
                    Write-WixUiXml $document $path
                }
            },
            @{
                Name = 'static-busy-guidance-missing'
                Code = 'UI035'
                Change = {
                    param($dir)
                    $path = Join-Path $dir 'thm.wxl'
                    $document = Read-WixUiXml $path
                    $node = $document.SelectSingleNode("/*[local-name()='WixLocalization']/*[local-name()='String' and @Id='FailureInstallerBusyText']")
                    $node.SetAttribute('Value', '安裝失敗')
                    Write-WixUiXml $document $path
                }
            }
        )
        foreach ($case in $cases) {
            Invoke-WixUiMutationCase -Name $case.Name -Source $source -Root $tempRoot -Mutation $case.Change -ExpectedCode $case.Code
            $mutationCount++
        }
    }
    finally {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }

    foreach ($name in $protectedHashes.Keys) {
        $actual = (Get-FileHash -LiteralPath (Join-Path $source $name) -Algorithm SHA256).Hash
        if ($actual -cne $protectedHashes[$name]) {
            throw "Self-test changed the original positive resource '$name'."
        }
    }

    [pscustomobject]@{
        mode               = 'self-test'
        positive_ba        = $source
        mutations_rejected = $mutationCount
        source_unchanged   = $true
    } | ConvertTo-Json -Compress
}

$bindingParameters = @('SourceTheme', 'SourceLocalization', 'BundleAuthoring')
$bindingParameterCount = @($bindingParameters | Where-Object { $PSBoundParameters.ContainsKey($_) }).Count
if ($bindingParameterCount -ne 0 -and $bindingParameterCount -ne $bindingParameters.Count) {
    throw '[UI050] Supply SourceTheme, SourceLocalization and BundleAuthoring together.'
}
if ($SelfTest) {
    if ($bindingParameterCount -ne 0) {
        throw '[UI050] SelfTest validates copied UI-resource mutations only; omit source/authoring binding parameters.'
    }
    if ($BaDir.Count -ne 1) {
        throw '[UI050] SelfTest requires exactly one positive extracted BA directory.'
    }
    Invoke-WixUiSelfTest $BaDir[0]
    return
}

$validatedDirectories = @()
foreach ($directory in $BaDir) {
    $fullPath = if (Test-Path -LiteralPath $directory -PathType Container) {
        (Resolve-Path -LiteralPath $directory).Path
    }
    else {
        $directory
    }
    Assert-WixUiValid $fullPath "Installer UI resource '$directory'"
    $validatedDirectories += $fullPath
}

if ($bindingParameterCount -eq $bindingParameters.Count) {
    $bindingIssues = @(Get-WixUiSharedBindingIssues `
        -Directories $validatedDirectories `
        -ThemeSource $SourceTheme `
        -LocalizationSource $SourceLocalization `
        -Authoring $BundleAuthoring)
    if ($bindingIssues.Count -gt 0) {
        $details = @($bindingIssues | ForEach-Object { "[$($_.Code)] $($_.Message)" }) -join [Environment]::NewLine
        throw "Shared bundle UI binding failed:$([Environment]::NewLine)$details"
    }
}

[pscustomobject]@{
    mode                 = 'validate'
    ba_directories       = @($validatedDirectories)
    localization         = 'zh-TW/1028'
    shared_binding_valid = ($bindingParameterCount -eq $bindingParameters.Count)
} | ConvertTo-Json -Depth 3 -Compress
