# Installer UI TDD handoff

## TDD 第一輪範圍（歷史；已由 verified RED 解鎖）

本輪只新增：

- `windows/native/tests/wix-installer-ui-tests.ps1`
- `docs/superpowers/plans/installer-ui-tdd-handoff.md`

未修改 installer production、MSI/交易/鎖、bundle authoring、build/verify、`.github/workflows/` 或 `scripts/`；也未執行測試、build、install、commit、push 或任何網路/WSL/registry/process/user-app 操作。

資料來源是正式 `0.1.1` EXE（SHA-256 `ba32e8e62dd4eddad057ca44218ea96860f94afaec86d026a7471515baef43ae`）由固定 WiX 6.0.2 真實抽出的 BA：

```text
windows/build/installer-plan-baseline/BA/
```

該目錄不是 test stub。測試及 `-SelfTest` 都不得改寫、覆蓋或在原目錄內建 mutation。

## Root 先取得 RED

使用 PowerShell 7，從 repo root 執行：

```powershell
pwsh -NoProfile -File .\windows\native\tests\wix-installer-ui-tests.ps1 `
  -BaDir .\windows\build\installer-plan-baseline\BA
```

目前輸入預期應以非零狀態結束，首筆失敗必須是：

```text
[UI020] Progress page must contain exactly one visible Label named 'ExecuteProgressActionDataText'.
```

## Task I3 production handoff（已完成程式碼、待 Root 驗證）

Root 的 `ui-red-2.json`/`ui-red.log` 已確認正式 `0.1.1` 真實 BA 在 09:47:14 UTC 以 exit 1、`[UI020]` 因缺 `ExecuteProgressActionDataText` 失敗。這次 feature RED 通過後，實作只包含下列允許範圍：

- 由官方 RTF source 最小衍生 `windows/native/wix/ui/rtf-theme.xml`，保留 license accept、logo、ProgressCancel、native FilesInUse 字串、真 `FailureMessageText` 及 Success/Failure restart/close。
- `windows/native/wix/ui/zh-TW.wxl` 集中全部繁中 UI 字串；1618 指引是 static Label，不新增 error-code theme variable 或 BAFunctions。
- `bundle-migration.wxs` 與 `bundle.wxs` 共用同一 `ThemeFile`/`LocalizationFile`。有明確 build variable，未傳時以各自 source file directory 的共同 `ui\` 資源作 fallback。
- `build-installer.ps1` 將同一 pair 傳給 Burn 與 verifier；`verify-bundle.ps1` 先驗證 source/embedded SHA-256，再呼叫既有 `wix-installer-ui-tests.ps1 -BaDir`。沒有複製 parser 或新增測試框架。
- `windows/VALIDATION.md` 與 `changelog.d/16-18-installer-flow.md` 只記錄實作與待驗範圍。

測試契約與已核准的官方 WiX RTF schema 沒有發現需要修測試的衝突；沒有放寬任何 assertion。沒有修改 MSI/transaction/registration/locking、workflow、`RELEASE_FLOW.md`、資料或 UI test；也沒有執行 test/build/install/runtime 或修改 repo 版號。不得據此宣稱 GREEN、畫面、DPI、鍵盤導覽或交易/重入通過。

### Root：兩種 bundle 獨立 build/extract

以下輸出目錄必須尚不存在；替換角括號內路徑為本機固定 WiX 6.0.2、BAL extension、source bundle、R64 MSI 及工具鏈：

```powershell
pwsh -NoProfile -File .\windows\native\wix\build-installer.ps1 `
  -WixExe <wix.exe> `
  -BalExtension <WixToolset.Bal.wixext.dll> `
  -SourceBundle <release-source-bundle> `
  -R64Msi <R64.msi> `
  -OutputDir <new-migration-output> `
  -ToolchainBin <CI-matched-toolchain-bin> `
  -MsiVersion <Root-approved-private-test-version>

pwsh -NoProfile -File .\windows\native\wix\build-disabled-prototype.ps1 `
  -WixExe <wix.exe> `
  -BalExtension <WixToolset.Bal.wixext.dll> `
  -SourceBundle <same-release-source-bundle> `
  -OutputDir <new-development-output> `
  -ToolchainBin <same-CI-matched-toolchain-bin> `
  -MsiVersion <same-Root-approved-private-test-version>
```

兩個 build script 的 `verify-bundle.ps1` 都會建立 `<output>\verification-extracted` 與 `-oba` 目錄 `<output>\verification-extracted-ba`，並先分別執行同一 committed UI validator。若 theme/wxl 抽出後與 source hash 不同、參照不可解、任一控制項或條件錯誤，build 入口必須失敗。

### Root：雙 authoring/source/embedded 共同驗證

```powershell
pwsh -NoProfile -File .\windows\native\tests\wix-installer-ui-tests.ps1 `
  -BaDir @('<new-migration-output>\verification-extracted-ba',
           '<new-development-output>\verification-extracted-ba') `
  -SourceTheme .\windows\native\wix\ui\rtf-theme.xml `
  -SourceLocalization .\windows\native\wix\ui\zh-TW.wxl `
  -BundleAuthoring @('.\windows\native\wix\bundle-migration.wxs',
                     '.\windows\native\wix\bundle.wxs')
```

### Root：GREEN 後 mutation self-test

```powershell
pwsh -NoProfile -File .\windows\native\tests\wix-installer-ui-tests.ps1 `
  -BaDir <new-migration-output>\verification-extracted-ba `
  -SelfTest
```

先取得兩種實際抽出資源 GREEN，才可跑 self-test。它只在 temp 複本 mutation；任何負例未以預期錯誤碼失敗、或原始資源 hash 改變，均視為 validator 失敗。

最後仍需在同一 Sandbox 以 100% 與 150% DPI，保存 Progress action/等待、FilesInUse、Failure/Success/restart/close、安裝說明同意與 logo，以及鍵盤導覽；這些 runtime 畫面本輪沒有執行。

（RED 階段歷史門檻）當時的真實 `thm.xml` 在 `Progress` 頁缺少官方 action data Label；缺工具、權限、檔案、網路或 EXE hash 錯誤都不能取代該 RED。Root 後已取得 `UI020`，因此進入 production implementation。

## 最小命令列契約

### 單一 extracted BA 驗證

```powershell
pwsh -NoProfile -File .\windows\native\tests\wix-installer-ui-tests.ps1 `
  -BaDir <burn-extract-oba-directory>
```

`-BaDir` 直接接受 `wix burn extract -oba` 的輸出目錄。腳本只讀該目錄的 `manifest.xml`、`thm.xml`、`thm.wxl`、`logo.png`、`license.rtf`，不執行 installer、進程、registry 或網路操作。

### 最終兩種 bundle 的共同來源與 embedded UI 驗證

production 已 GREEN 後，正式與開發 bundle 都必須實際 build/extract；每個 `-oba` 目錄各傳一次：

```powershell
pwsh -NoProfile -File .\windows\native\tests\wix-installer-ui-tests.ps1 `
  -BaDir .\build\ui\migration-ba,.\build\ui\development-ba `
  -SourceTheme .\windows\native\wix\ui\rtf-theme.xml `
  -SourceLocalization .\windows\native\wix\ui\zh-TW.wxl `
  -BundleAuthoring .\windows\native\wix\bundle-migration.wxs,.\windows\native\wix\bundle.wxs
```

三個 source/bundle 參數必須同時提供。此模式強制：

- 恰好兩個不同 extracted BA；
- 恰好兩份不同 authoring；
- 兩份 authoring 各恰有一個 `WixStandardBootstrapperApplication`；
- `ThemeFile` 在兩份 authoring 中完全相同；
- `LocalizationFile` 在兩份 authoring 中完全相同；
- 兩個 embedded `thm.xml` 的 SHA-256 都等於同一 `SourceTheme`；
- 兩個 embedded `thm.wxl` 的 SHA-256 都等於同一 `SourceLocalization`。

只改 source XML、只 grep WXS、只檢查一個 bundle 或只驗 fixture 都不成立。

### GREEN 後的 mutation self-test

只拿一個已通過的真實 extracted BA：

```powershell
pwsh -NoProfile -File .\windows\native\tests\wix-installer-ui-tests.ps1 `
  -BaDir <one-green-burn-extract-oba-directory> `
  -SelfTest
```

腳本先要求正向資源全部通過，再把整個 BA 複製到唯一 temp 子目錄。負例只修改複本，最後刪除該 temp 子目錄並比對原始五個資源的 hash。self-test 明確 mutation：

- action 移出 `Progress`；
- action 設為隱藏；
- `WixStdBARestartRequired` 改錯；
- 刪除 FilesInUse 原生字串；
- 插入未知 `#(loc.*)`；
- 刪除 EULA accept 控制項；
- 刪除 logo 控制項；
- 刪除真 `FailureMessageText`；
- 改壞固定 1618／0x80070652 說明。

每個負例必須以預期錯誤代碼被擋；不接受「任何失敗」。baseline 尚未 GREEN，不可對 baseline 跑 `-SelfTest`。

## 實際 WiX BA schema 契約

測試用 .NET XML parser，以 DTD 禁止與 `XmlResolver = null` 解析；不把 source 字串出現視為有效 UI。

### Embedded resource binding

`manifest.xml` 的 `UX/Payload/@FilePath` 必須各恰好嵌入：

- `thm.xml`
- `thm.wxl`
- `logo.png`
- `license.rtf`

embedded BA 不得含 `BAFunctions*.dll`。

### Localization reference table

只在 `thm.wxl` 的 `WixLocalization/String[@Id]/@Value` 建立字典，拒絕重複或空 ID。解析器逐一檢查 theme 與 wxl 的文字/屬性：

- 未知 `#(loc.*)` 或 `!(loc.*)` 失敗；
- 格式不完整的 reference 失敗；
- 巢狀 reference 失敗時可定位未知或循環；
- 合法 `#(loc.*)` 可以保留在 extracted `thm.xml`，只要字典可正常解析，**不得**錯誤要求原始 theme 清除所有 reference。

單一 embedded wxl 必須是 `Culture="zh-TW"`、`Language="1028"`。

### 必要控制項

| 位置 | 必要節點 |
| --- | --- |
| `Install` | `Richedit[@Name='EulaRichedit']`、可顯示 `Checkbox[@Name='EulaAcceptCheckbox']` |
| Window | 一個可顯示 `ImageControl[@ImageFile='logo.png']`，且 `logo.png` 有 PNG signature |
| `Progress` | 可顯示 `Label[@Name='ExecuteProgressActionDataText']`、原生 `Button[@Name='ProgressCancelButton']` |
| `Progress` | 可顯示 static action heading 與等待說明；action Label 有無資料時使用一般階段 fallback |
| `Failure` | 空的原生 `Hypertext[@Name='FailureMessageText']`，由 BA 填真實 HRESULT/system text |
| `Success`、`Failure` | restart text 的唯一 `VisibleCondition='WixStdBARestartRequired'`，以及原生 Restart/Close 控制項 |

Action control 只能在 `Progress`，不得使用 `Visible="no"`、`Hidden="yes"`、零尺寸或 `VisibleCondition`。它必須保留一般階段文案 `ProgressActionFallback`，以免 MSI 暫無 action data 時捏造阻塞原因。

### 固定字串契約

以下 ID 必須由同一 `zh-TW.wxl` 提供：

| ID | Value |
| --- | --- |
| `ProgressActionLabel` | `Windows Installer 目前作業` |
| `ProgressActionFallback` | `正在準備或執行安裝作業` |
| `ProgressWaitText` | `驗證或移除舊版可能需等待，請勿重複啟動安裝` |
| `FailureInstallerBusyText` | `若顯示 1618／0x80070652，請等待其他安裝完成，再重新開啟本程式` |
| `SuccessRestartText` | `安裝完成，需要重新啟動以完成舊檔清理` |

另外必須保留既有 `SuccessUninstallRestartText`、`FailureRestartText`、`SuccessRestartButton`、`SuccessCloseButton`、`FailureRestartButton`、`FailureCloseButton` 及 `InstallAcceptCheckbox`、`ProgressCancelButton` 字串。`SuccessCloseButton` 必須明確包含「稍後」，表示稍後自行重啟。

原生 FilesInUse 字串必須全部保留：

- `FilesInUseTitle`
- `FilesInUseLabel`
- `FilesInUseNetfxCloseRadioButton`
- `FilesInUseCloseRadioButton`
- `FilesInUseDontCloseRadioButton`
- `FilesInUseRetryButton`
- `FilesInUseIgnoreButton`
- `FilesInUseExitButton`

合併後的繁中 FilesInUse 文案必須包含自行儲存、重試與稍後重新啟動選擇。`DisableShutdown` 仍由 MSI/chain 既有契約驗證；本 UI test 不改動或重造鎖、交易或程序操作。

`FailureMessageText` 必須保持空的 native Hypertext，才能顯示真實錯誤；固定 1618 句只能是不帶錯誤碼條件的 static Label。禁止 theme/wxl 的 `[...ErrorCode...]`／`[...HRESULT...]` 變數，以及為此新增 BAFunctions。

## RED-stage 完成界線（歷史）

此為 verified RED 前的停止條件；Root 已取得 `UI020` 後由 production handoff 取代。GREEN 與兩 bundle 的實際 build/extract、mutation self-test、畫面/DPI/鍵盤人工驗收仍是 Root 後續驗證。

## Root 回報的 harness 修正

Root 原先實際執行時，`Read-WixUiXml` 的 `Write-Output -NoEnumerate $document` 仍把 `XmlDocument` 以 generic collection 傳給 `Get-WixUiPage`，在 feature validation 前失敗；這不是核准的 `UI020` RED。

已將該 helper 改為 `[OutputType([System.Xml.XmlDocument])]` 加 unary-comma return（`return ,$document`），同一測試檔其餘 `Read-WixUiXml` 呼叫點不需逐點轉型。此 follow-up 只修 harness 的回傳形狀，未執行測試；Root 需獨立重跑 baseline 命令，確認首筆失敗才是：

```text
[UI020] Progress page must contain exactly one visible Label named 'ExecuteProgressActionDataText'.
```

## Honest restart guidance 測試先行（2026-11-27）

Root 已在 09:58 UTC 讓先前 production 以舊契約通過結構驗證及 9 個 mutation，但獨立確認兩個真實缺陷：

- `SuccessCloseButton` 在沒有 `WixStdBARestartRequired` 時仍提示稍後重新啟動；
- `SuccessUninstallRestartText` 只覆蓋 action 3，漏掉正常 uninstall action 4。

本節取代先前固定字串契約中「`SuccessCloseButton` 必須包含稍後」的說法。本次只更新 test contract，未修改 production。`UI037` 現在要求：

- `SuccessCloseButton` 有無條件的 `SuccessCloseButton` default，顯示為 `關閉`；
- 同一原生 Button 有且只有一個 `Text`，其 `Condition='WixStdBARestartRequired'` 引用 `SuccessCloseRestartLaterButton`，顯示為短字串 `稍後重啟`；
- 兩個字串均保留 accelerator；原生 Button、唯一 `CloseWindowAction` 與 control name 不變；
- `SuccessUninstallRestartText` 必須同時覆蓋 action 3 與 4；可用 `WixBundleAction = 3 OR WixBundleAction = 4`（雙方向同義），或兩個各自條件的 `Text` 節點。

Root 應先以舊契約已通過的 `ui-source-positive` fixture 取得新 feature RED：

```powershell
pwsh -NoProfile -File .\windows\native\tests\wix-installer-ui-tests.ps1 `
  -BaDir <existing-ui-source-positive-fixture>
```

預期為非零狀態，且輸出必須包含 `UI037`，例如：

```text
[UI037] SuccessCloseButton must have an unconditional generic '關閉' default and a Text with Condition='WixStdBARestartRequired' pointing to SuccessCloseRestartLaterButton ('稍後重啟').
```

不得以 XML parse error、缺檔或其他 harness failure 代替。該 fixture 在 production 尚未修正前，至少會因 unconditional close guidance、缺 `SuccessCloseRestartLaterButton` 條件節點及漏 action 4 而被擋。Root 未取得此 RED 前不得修 production。

production 修正後，`-SelfTest` 會再加入兩個固定 negative mutation，原 9 個保持不變：

- 移除 `SuccessCloseButton` 的 `WixStdBARestartRequired` 條件，必須被 `UI037` 擋下；
- 把 `SuccessUninstallRestartText` 的 coverage 限回 action 3，必須被 `UI037` 擋下。

總計 11 個 mutation；它們只在 temp 複本執行，不會改原 fixture 或 production。

## Root：UI037 修正與實際 bundle gate handoff

`UI037` 已以 production 的現有 schema 修正，未改 test contract：

- `SuccessCloseButton` 預設為 `SuccessCloseButton` 的 `關閉(&C)`。
- 同一原生 button 增加 `Condition="WixStdBARestartRequired"` 的 `SuccessCloseRestartLaterButton`，短字串為 `稍後重啟(&L)`；保留唯一 `CloseWindowAction`。
- `SuccessUninstallRestartText` 使用固定 `WixBundleAction = 3 OR WixBundleAction = 4`。

兩份 bundle 同時隨附 adjacent `ui/LICENSE.TXT`（MS-RL）為 embedded BA payload `wix-theme-license.txt`，而 `verify-bundle.ps1` 與 build test 都以 SHA-256 比對同一來源。來源與 license URL 記在 `windows/VALIDATION.md`；沒有把 repo Apache license 當成 upstream theme 授權。

新 `wix-installer-ui-build-tests.ps1` 參數只接受 `-WixExe`、`-BalExtension`、`-Msi`、`-MigrationBundle`、新 `-OutputDir`，可選 `-RuntimeVersion`。它以 WindowsInstaller COM 讀 MSI `ProductVersion`、不安裝任何 MSI/EXE，對既有 MSI 實建 development bundle，抽出兩個真 BA 後執行共同 UI validator 與一個 BA 的 11 mutation。

Root 在完成 `build-platform-release.ps1` 後可獨立執行：

```powershell
pwsh -NoProfile -File .\windows\native\tests\wix-installer-ui-build-tests.ps1 `
  -WixExe <fixed-wix-6.0.2.exe> `
  -BalExtension <fixed-bal-extension.dll> `
  -Msi <platform-output>\wix\LiuKai.msi `
  -MigrationBundle <platform-output>\wix\LiuKai-win-setup.exe `
  -OutputDir <new-installer-ui-build-test-dir> `
  -RuntimeVersion <approved-source-runtime-version>
```

預期成功收據為該 OutputDir 下的 `ui-build-test-result.json`，包含實際 build/extract 命令、兩個 bundle hash、source/embedded license hash、共同 validation 與 `mutations_rejected: 11`。本輪未執行該命令；也未宣稱 actual bundle、DPI 100%/150%、runtime action、keyboard 或 install/uninstall GREEN。

CI 已只在 platform build 後新增同一 build-test step，並在失敗時保存其輸出目錄；資料 audit、HEAD blob guard 及其他 workflow step 原樣保留。

### Root：MS-RL byte identity

本工具的外部 diagnostics 路徑讀取被拒絕；repo 已加入完整 MS-RL 文本，但本 worker 無法直接宣稱它與 Root 下載檔逐位元相同。Root 應在實際 build 前只做一次 hash 比較，並在不符時以該 supplied file 覆寫 `windows/native/wix/ui/LICENSE.TXT`，不得改用 repo 根授權：

```powershell
$sourceLicense = 'E:\agent-work\Liu-Kai\diagnostics\installer-readings-implementation-20261127\wix-6.0.2-LICENSE.TXT'
$repoLicense = '.\windows\native\wix\ui\LICENSE.TXT'
$sourceHash = (Get-FileHash -LiteralPath $sourceLicense -Algorithm SHA256).Hash
$repoHash = (Get-FileHash -LiteralPath $repoLicense -Algorithm SHA256).Hash
if ($sourceHash -cne $repoHash) { throw "WiX MS-RL byte mismatch: source=$sourceHash repo=$repoHash" }
```

該比較不屬 test/build/install，僅確認這次法律副本的必要前置條件。

## Root：actual build-test ProductVersion stream fix

Root 於 10:20 UTC 對 clean private `39af3117` 的實際 migration candidate 先確認 MSI/UI/theme/wxl/license/logo verification 通過。其後執行 `wix-installer-ui-build-tests.ps1`，WiX 以 exit 44 顯示 `BundleVersion` 夾帶空白並級聯缺 `BootstrapperApplication` Name/SourceFile；原始 receipts 為 `ui-two-authoring.json`、`ui-two-authoring.log` 與 `.err`。

根因是 `Get-WixUiMsiProductVersion` 的 WindowsInstaller COM view 呼叫將 `Execute()` 與 `Close()` 的 method return 也寫入 PowerShell success stream，使 function 產出陣列；沒有改 MSI version formula。修正只做：

- `[void]$view.Execute()`
- `[void]$view.Close()`
- `([string]$record.StringData(1)).Trim()` 後返回單一已驗證字串。

未加 argument quoting workaround、hardcoded `70.1.4` 或 authoring 變更。Root 應以同一 MSI/candidate 與新 OutputDir 重跑既有命令；若後續仍出現 BA `Name`/`SourceFile` 錯誤，另由 Root 依實際後續證據調查，不在本修正內預先改 authoring。本 worker 未重新執行 build/test，也不宣稱該 second-authoring error 已消失。

## Theme glyph coverage 測試先行

Root 在 owned Sandbox 對 actual prototype `4faf` 只 preview/確認後取消；未 Apply。Title、button/EULA label 的繁中字元顯示方框，但 RichEdit notice 與 OS dialog 正常。Root 的 safe headless GDI probe 顯示 Segoe UI 在樣本「嘸改安裝取消」漏全部 6 個 glyph；Microsoft JhengHei UI、Microsoft YaHei UI 與 PMingLiU 漏 0。此為 UI 真實缺陷證據，不是把某個字型名稱寫進 assertion。

本輪只新增 `wix-installer-ui-font-tests.ps1`；它：

- 只接受 `-ThemeFile`；
- 用禁止 DTD/外部 resolver 的 XML parser 讀 WiX v4 thmutil `Font` 節點；
- 使用 `CreateCompatibleDC`、`CreateFontW`、`SelectObject`、`GetTextFaceW` 與 `GetGlyphIndicesW(..., GGI_MARK_NONEXISTING_GLYPHS)`；
- 以 Unicode code point 顯式取 U+5638/U+6539/U+5B89/U+88DD/U+53D6/U+6D88，輸出每個 `Font` 的 requested face、actual face、missing count；
- finally 中還原 font 並釋放 font/DC；不建立視窗、不做 input/UI Automation/desktop capture、registry、system settings、process、network 或 install。

先在 Root 已確認的 owned Sandbox 執行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\windows\native\tests\wix-installer-ui-font-tests.ps1 `
  -ThemeFile .\windows\native\wix\ui\rtf-theme.xml
```

目前 `rtf-theme.xml` 的四個 Font 節點仍是 Segoe UI；預期非零且包含：

```text
[FONT001] Theme fonts do not cover the required CJK sample
```

stdout 的 JSON 必須顯示 actual face 與 `missing_count: 6`，才是核准的 feature RED。`[FONT002]` 只代表 GDI failure，`[FONT003]` 只代表檔案/XML failure，兩者均不可冒充 glyph RED。測試未 hardcode Segoe UI、JhengHei、YaHei 或 PMingLiU；任何實際已安裝且 six-glyph complete 的 face 都可通過。

本節尚未修改任何 production font、bundle、workflow 或 build script。Root 取得 `[FONT001]` 後，後續 GREEN 只應將四個 Theme Font faces 改為 `Microsoft JhengHei UI`，再由 build CI 呼叫此 test 並保留 actual UI evidence。本 worker 未執行測試。

### Root：font harness case-collision fix

Root 在同一 owned Sandbox 於 10:34 UTC 首跑時尚未進入 feature RED：typed parameter `$Font` 與 native `IntPtr` local `$font` 在 PowerShell 大小寫不敏感規則下是同一變數，`0` handle 嘗試轉回 `System.Xml.XmlElement` 而失敗。`font-red.json/log` 因此是 harness failure，不是 glyph RED。

修正僅將 XML parameter 改為 `$FontNode`、native handle 改為 `$fontHandle`，呼叫端改用 `-FontNode`；CreateFont/GetGlyph/DC cleanup 及任何 assertion 均未改。Root 應重新執行相同 `-ThemeFile` 命令，正確 feature RED 必須是 `[FONT001]` 與六個 missing glyph，而不是型別轉換錯誤。本 worker 未執行修正後測試。

## Root：FONT001 production fix handoff

Root 的第三次 glyph test（10:41 UTC，`font-red-3.json/log`）已驗證真正 feature RED：四個 Segoe UI Theme Font 節點各有 `missing_count: 6`，樣本為 U+5638/U+6539/U+5B89/U+88DD/U+53D6/U+6D88。Root 原始 GDI probe 已證明同一 owned Sandbox 的 `Microsoft JhengHei UI` 覆蓋全部 6 個 glyph。

已只將 `rtf-theme.xml` 的四個 face 由 Segoe UI 改為 `Microsoft JhengHei UI`；四個 `Height`、`Weight`、color、layout、controls、localization 與 transaction 均未動。`wix-installer-ui-build-tests.ps1` 現在在 development build 前執行 committed font test，並將 font command、完整 JSON 及其 `glyphs_valid` 一併放進 `ui-build-test-result.json`。

既有 COM ProductVersion fix、兩個 REAL authoring 的 build/extract、共同 UI validator、license hash 與 11 mutation 均保持原樣。Root 需以新 theme 重建 private candidate，再重跑 actual build test、DPI 100%/150%、繁中不截斷與鍵盤導覽。此 worker 未執行新 build/test/screenshot，沒有宣稱 FONT001 GREEN。
