# Windows 安裝提示與字表核對設計

狀態：原生 agy Gemini 4 Neon 三輪對抗已收斂，第三輪兩條明確門檻已補齊。使用者於 2026-11-27 核准依計畫實作：Copilot MAI Code 2 Max、TDD、agy Neon 審查及 Windows Sandbox 驗證；測試程式納入 repo。尚未完成實作或重新發版。[對抗摘要](../plans/2026-11-27-installer-readings-review.md)。

基準：`57cc3a79c3f1d2bd67ec599aaa9c223c3988add2`。相關議題：[安裝重入與登錄恢復](https://github.com/hamanpaul/liu-kai/issues/16)、[安裝進度與提示](https://github.com/hamanpaul/liu-kai/issues/18)、[字表讀音與候選對帳](https://github.com/hamanpaul/liu-kai/issues/19)。

## 使用者已定案

- 規劃階段已完成；現已授權本機實作、測試及 Sandbox 部署，尚未授權 push/PR/正式發版或主機安裝。
- 所有工作使用 Windows 原生工具；WSL 正在匯出，連唯讀命令與 UNC 路徑也不使用。
- 對抗使用已登入的 `agy.exe`、`gemini-4-neon-high`，本次獨立計算最多八輪，沒有問題即可提前收斂。
- **直接注音與字碼同音均維持現有主要音規則，先只核對資料。** 不擴成查所有有效次讀音，也不改傳統同音排序。
- DRY：沿用官方 WiX Burn/MSI；讀音的 canonical 資料仍是 core 的 `readings.tsv`，Windows 複本必須一致。
- KISS/YAGNI：兩個獨立 plan；不新建安裝框架、常駐服務、資料庫或雙平台查字引擎。原先排除新增互斥是基於「Burn 鎖已啟用」的錯誤假設；實測反證後，僅評估官方擴充中的最小產品互斥，不擴成鎖管理框架。

## 選項與決定

| 選項 | 代價 | 決定 |
| --- | --- | --- |
| 沿用 WixStdBA 的步驟事件與重啟狀態，加最小 theme/wxl；資料先只讀對帳 | UI／資料部分可限於資源、測試及一個小腳本；原生鎖假設已被實測與來源推翻 | UI／資料部分採用；防重入另需最小補丁 |
| 新 launcher/自製 BA/BAFunctions 全面接管交易 | 增加登入、UAC、鎖生命週期及維護面，沒有現成缺陷證據支持 | 不採用 |
| 自動合併全部候選與所有次讀音，重建統一資料庫 | 改變查詢語意，且兩份互有差異的表不是彼此的真值 | 使用者已排除 |

## 已確認的安裝邊界

`#16` 有兩個不同命題：第二次 Apply 不應啟動第二筆 MSI，以及新 MSI 已成功但 BA 中斷後普通雙擊是否能恢復登錄。沒有證據顯示第二次 setup 就是 `0x800700e8` 的原因。

更正：WiX 6.0.2 `ApplyLock` 中 `Global/Local\\WixBurnExecutionLock` 的程式實際在 `#if 0` 停用，不能依賴它防止重入。控制組已重現第二筆雖被 MSI 1618 拒絕，仍移除第一筆共用 Burn 登錄，造成第一筆 MSI 3010 之後 Burn 登錄失敗。完整時序及普通重開恢復／卸載證據見[安裝計畫更正](../plans/2026-11-27-windows-installer-flow.md#2026-11-27-控制組實測與設計更正)。

目前產品防重入補丁尚未實作。最小修正須在登錄與 MSI 交易前排除第二個外層客戶端，保護持續到背景交易結束，且允許合法相關 bundle 的內部呼叫；正在核對官方擴充介面與生命週期。只禁止第二筆交易，不把第二個視窗能否開啟當成鎖失效。

WixStdBA 已將 MSI `ACTIONSTART` 寫到名為 `ExecuteProgressActionDataText` 的控制項；目前採用的官方 `RtfTheme.xml` **沒有該控制項**。最小 theme 是為了露出既有事件，不是重做 BA。現有 success 頁已有 `WixStdBARestartRequired` 顯示條件。

保留 `DisableShutdown` 與現有 MSI/Burn UpgradeCode。UI 只能說明已收到的作業與可能的等待原因，不得從畫面靜止就斷言檔案占用或死鎖，不製造百分比與剩餘時間。

## 已確認的資料邊界

兩份讀音表皆為 44,353 字。「勒」已是 `ㄌㄜˋ ㄌㄟ`，既有雙端驗收不是本輪新修正。後續以 Git 原始 blob 核對發現，基準 Windows 表是 CRLF、core 表是 LF，原先「byte-identical」的敘述不成立；字列、順序與欄位沒有差異。已機械統一 LF 並以 `.gitattributes` 固定，目前兩份皆為 614,553 bytes，SHA-256 `f2f5d289ccec051d2f7185b49ed4e9c7ce885f179bf4b781a5dba3a69a7c9da8`，沒有修改讀音內容。

逐讀音候選表有 5,975 組，17,972 個非空候選關聯及 4,644 個明示空組。與讀音表精確比較：

| 分類 | 關聯數 | 解讀 |
| --- | ---: | --- |
| 主要音相符 | 14,172 | 一致性事實，不保證字典內容全面正確 |
| 已列出的次讀音 | 25 | 資料有效性與是否參與主要音查詢分開；本輪不改查詢 |
| 候選文字無讀音列 | 37 | 已逐筆確認全是 U+3105–U+3129 注音符號本身，不是 37 個漏音漢字 |
| 候選讀音未列於該字 | 3,738 | 是待分類的關聯，不是 3,738 個已確認錯字 |
| 明示空組 | 4,644 | 現有資產契約，不能刪除或改當缺漏 |

`readings.tsv` 的來源是既有 `gen-readings` 的 Unihan 管線與已註明來源的「勒」修正；`phonetic-candidates.tsv` 有獨立來源 metadata，保留傳統候選觀察資料及過濾規則。兩表不要求集合相等。

先交付完整可重跑對帳、分類、同步門禁與具名裁決；不靠重新排序、刪除不匹配資料或大白名單讓報告變成零。涉及正音真值的修改必須有官方來源證據；未判定的關聯保持未判定。

## 驗收與交接

- 安裝：一個 Windows Sandbox 串行，使用一致版本的 baseline/candidate，包含真 DLL 占用、跨版本、3010 未重啟、重入與實際 BA 中斷後恢復。主機不重啟。
- Windows 11 22H2 起可在 guest 內 Restart 保留 Sandbox 狀態，和關閉 instance 不同；用 guest C 槽 sentinel/登錄/boot time 作前置驗證，失敗則明列重啟後未覆蓋，不擅自另建 VM。
- 字表：Windows Python 標準庫、原生 C++；如果後續確有 generator 變更，再跑 Windows JDK 17/Kotlin JVM。已找到原生 JDK 17.0.18，但尚未執行 Gradle 或聲稱依賴已齊。
- 兩個 plan 分別交付、分別驗收，不互相拖住。`#16`/`#19` 不因有報告就視為修好，也不將剩餘問題搬家後關單。
- 不增加 Android 模擬器、不宣稱 Android 實機驗收；本輪沒有 Android 行為變更。
- 實作階段仍遵守可用記憶體至少 30 GiB、D 槽 Windows 顯示至少 80 GiB（守護程序以 82 GiB 提前停止）。最多一個 Sandbox，工作檔放 E 槽；目前控制組驗證完成後已關閉該 guest 釋放資源，後續有待驗成品才重新啟動。

## 計畫與官方依據

- [安裝計畫](../plans/2026-11-27-windows-installer-flow.md)
- [資料核對計畫](../plans/2026-11-27-readings-consistency.md)
- [WiX v6.0.2 ApplyLock](https://github.com/wixtoolset/wix/blob/v6.0.2/src/burn/engine/apply.cpp#L411)
- [WiX v6.0.2 WixStdBA](https://github.com/wixtoolset/wix/blob/v6.0.2/src/ext/Bal/stdbas/WixStandardBootstrapperApplication.cpp#L1074)
- [固定版 RTF theme](https://github.com/wixtoolset/wix/blob/v6.0.2/src/ext/Bal/stdbas/Resources/RtfTheme.xml#L57)
- [WixStdBA 自訂 theme 與變數](https://docs.firegiant.com/wix/tools/burn/wixstdba/)
