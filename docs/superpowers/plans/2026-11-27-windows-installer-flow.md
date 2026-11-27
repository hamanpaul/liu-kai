# Windows 安裝流程與提示 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 沿用官方 WiX，驗證重入與中斷恢復，補齊真實作業、占用處理及待重啟提示，解決安裝 issue 16/18 的可驗收缺口。

**Architecture:** MSI 仍是檔案與註冊的唯一交易權威；Burn/WixStdBA 仍處理偵測、升級與 UI。原先認定 Burn 6.0.2 已有有效跨程序互斥是錯誤假設：`ApplyLock` 的 mutex 程式在 `#if 0` 中停用。已保留第二客戶端破壞第一筆 Burn 登錄的控制組 FAIL，並以官方 BAFunctions Create 前置 guard 修正；新候選 Reentry 與 3010 後的完整狀態續驗已通過。真 BA 中斷恢復仍不算完成。

**Tech Stack:** WiX/Bal 6.0.2、Windows PowerShell 5.1/7、原生 C++、單一 Windows Sandbox。

---

狀態：三輪 Neon 對抗後，使用者已於 2026-11-27 核准執行；以下 checklist 按真實證據更新。設計裁決見[共用設計](../specs/2026-11-27-installer-readings-design.md)，審查結果見[對抗摘要](2026-11-27-installer-readings-review.md)。關聯：[issue 16](https://github.com/hamanpaul/liu-kai/issues/16)、[issue 18](https://github.com/hamanpaul/liu-kai/issues/18)。

2026-11-28 使用者新增授權：剩餘作業只在沙盒自主完成，不做本機安裝；通過後 push／PR、處理 CI 與 GitHub review，再 merge／發版。此指示取代本計畫原先「只做本機提交」的交付限制。各階段私有候選與正式成品不混用，最新實測見 [Windows 驗證紀錄](../../../windows/VALIDATION.md)。

## 預期變更範圍

| 檔案 | 用途 |
| --- | --- |
| Create `windows/native/wix/ui/rtf-theme.xml` | 固定官方 theme 的最小衍生，露出 MSI action 與等待說明；保留原授權標示及必要控制項 |
| Create `windows/native/wix/ui/zh-TW.wxl` | 同意、作業、占用選項、失敗與待重啟文案，集中一次定義 |
| Modify `windows/native/wix/bundle-migration.wxs` | 正式 Burn 綁定共同 ThemeFile/LocalizationFile |
| Modify `windows/native/wix/bundle.wxs` | 開發 fixture 沿用相同資源，防止兩套 UI 漂移 |
| Modify `windows/native/wix/build-installer.ps1` | 傳入同一對 UI 資源，不改 MSI 交易順序 |
| Modify `windows/native/wix/verify-bundle.ps1` | 驗證抽出的真實 theme/wxl、控制項、logo/notice/MSI |
| Create `windows/native/tests/wix-installer-ui-tests.ps1` | 資源缺漏、錯條件及真封裝內容的回歸，不只搜尋 source 字串 |
| Create `windows/native/tests/wix-installer-scenarios.ps1` | 在已指定的拋棄式 guest 內執行有界情境、保存程序身分及版本結果 |
| Modify `.github/workflows/windows-installer.yml` | 建置後執行封裝資源檢查，保留失敗 artifact |
| Modify `windows/RELEASE_FLOW.md`, `windows/VALIDATION.md` | 目前行為、重入/恢復步驟及有界實測 |
| Create `changelog.d/16-18-installer-flow.md` | 實作完成時記錄實際變更，不先聲稱已修 |

`package.wxs`、registration action 及交易邏輯預設不改。若下述 Gate I2 失敗，先保存具體原因與最小修正設計，不偷偷將這份 UI 計畫擴成安裝器重寫。

## Task I1：建立跨版本及重入基準

- [x] 核對官方 baseline `windows-v0.1.1` 的 EXE/hash/source；候選必須是產品 `0.1.2`、MSI/Burn `70.1.4`、`versions/0.1.2/` 與 manifest 相符的**私有測試快照**。不只改內部數字而重用 0.1.1 目錄。本輪 plan 不修改任何 VERSION。
- [x] 使用一個 4 GiB Sandbox，關閉網路與剪貼簿、E 槽輸入輸出；主機可用 RAM/D 槽低於門檻便停止自己的 guest，不操作 WSL。
- [x] 先核對 guest 為 Windows 11 22H2 以上。在 guest 自身 C 槽寫入隨機 sentinel，記下 hash、Sandbox ID 與 guest boot time；只在 guest 內重新啟動，再核對 sentinel/登錄仍在且 boot time 已改變。host-mapped 檔案不能作持久性證據。若 sentinel 不保留，標記環境不符合、停止重啟後驗收；不自動另建 Hyper-V VM。
- [x] 測試入口參數固定為 `-BaselineSetup <EXE> -CandidateSetup <EXE> -OutputDir <new-dir> -Scenario Upgrade|Reentry|Recovery`。拒絕非 guest、既有輸出目錄、版本不遞增或雜湊不符。fixture 不讀使用者文件。
- [x] 基線安裝後，以既有 mapped-module fixture 類型的獨立程序載入舊 DLL。核對 PID、建立時間、實際 DLL 路徑與雜湊；不使用真實使用者 App 製造占用。
- [x] 執行第一次升級，保留 Burn/MSI 日誌、真實 UI 畫面、每個 action 時間、MSI/Burn/COM/owner/manifest 前後狀態。若自然發生 3010，保留未重啟現場；不以固定 sleep 假裝六分鐘卡住。
- [x] 第一次確實進入 Apply 後只再啟動一次相同候選。驗證第二次被原生鎖拒絕、沒有第二筆 MSI，第一筆仍完成。第二視窗能出現不是失敗；提示必須可讀且告知等待原安裝。
- [x] 在測試收據中區分 Burn 互斥與 MSI 執行鎖、第二次 HRESULT 與第一次結果；不將同時發生推論成 pipe 斷線的因果證據。

預期：`Reentry` 必須同時證明第二次與第一筆 Apply 時間重疊、第二次 Burn log 明確記錄原生鎖拒絕（`0x80070652`／`ERROR_INSTALL_ALREADY_RUNNING`／1618），以及 `second_msi_transactions = 0` 且第一筆成功。未撞鎖屬無效試驗，不得判 PASS；只允許一次有目標的時機修正重試，仍未命中便記 `NOT_COVERED`。若第二筆真的進 MSI，保存完整證據，不能加一個新鎖掩蓋原因。

## Task I2：有界驗證中斷恢復

- [x] 先卸載候選、guest 內重啟及獨立 readback，再裝同一 baseline。這是串行清理，不是 Sandbox 快照還原；guest 無法恢復時，先關閉舊 instance 才啟動乾淨 instance，仍只開一個。保留正常升級對照。
- [ ] 將「防重入」與「中斷後恢復」分開。恢復試驗最多兩次**有目標**的 BA 故障注入；在真 MSI `InstallFinalize` 成功/提交後，觀察仍存活的非提權 BA 窗口，只停止核對 EXE/hash/PID/creation time 的 BA/UI。若 BA 已結束或只剩提權 Engine，不注入；不結束 msiexec、Windows Installer service、提權 Engine 或 host 程序。
- [ ] 只有實際觀察到「新 MSI 成功、bundle 登錄不一致」才能進恢復驗收。用 MSI API/產品版本及完整原始日誌證明 MSI 已完成，不靠寫 Registry 製造成功。
- [ ] 對此真實現場普通雙擊同一候選，不帶 `/passive`、repair property 或手動註冊修復參數。分別核對未重啟狀態；必要時只重啟 guest，再普通雙擊核對。
- [ ] 成功條件：新 MSI/Burn 與實際 payload/COM 版本一致、舊 bundle/MSI 無孤兒登記、可透過同一 EXE 卸載；使用者設定/Doctor 控制與歷史依現有契約保留。
- [ ] 若兩次都沒形成所需現場，記錄 `NOT_COVERED` 與具體時序，停止重複殺程序。直接 MSI 構造的等價狀態只能當補充 oracle，不能代替 BA 中斷，也不得關閉 issue 16。
- [ ] 普通雙擊恢復失敗時，issue 16 保持開啟；先固定 Detect/Plan/Registration 的真實缺口，再提交最小補丁設計。不得刪舊登錄、修改 Installed 標記或將 MSI 3010 說成完整 Bundle 成功。

Gate I2：只有原生拒絕重入、真實中斷後恢復及可卸載均有證據，才能聲稱 issue 16 完成。原始 `0x800700e8` 根因若未定位，仍明示未知，不以測試注入等同原始根因。

### 2026-11-27 控制組實測與設計更正

- 官方 baseline 0.1.1 升級至原始控制組 0.1.2 時，第一筆在 22:16:28（UTC+8）開始執行 MSI；第二客戶端已看到同一 bundle 的 `InProgress`，仍進入 Apply，之後 MSI 才回 1618。
- 第二筆在 22:16:53 以 `SessionEnd None` 移除共同 Burn 登錄。第一筆 MSI 在 22:18:35 提交並要求重啟，但下一秒 `SessionEnd Full` 因登錄不存在而失敗 `0x80070002`。第一筆仍回 Apply 0／3010，重啟 guest 後只留下 MSI 登錄。**控制組 Reentry = FAIL**；MSI 的 1618 不能保證 Burn 交易未互相干擾。
- 已完成普通雙擊同一 EXE、空參數的恢復：Burn 登錄重新建立，未重跑 MSI；12 個 payload、COM、owner、MSI/Burn 版本與設定／Doctor 保留核對通過。這是上述競態事故的恢復，**不是 BA 故障注入**，不能替代原定 I2。
- 已從同一 EXE 的維護畫面正常卸載。選擇保留占用程式、稍後重啟；guest 內重啟後 MSI/Burn/COM/owner、產品目錄及產品待刪項目均不存在，設定與 Doctor 歷史保留。原生狀態工具的 `GetProfile` 在 profile 不存在時回 E_FAIL；未聲稱全部查詢都回 0，也未要求空 TIP 根鍵必須消失。
- 補丁目標是「第二個外層客戶端在任何登錄／MSI 交易之前退出」，同產品不同版本共用保護，背景交易尚在時不能因 UI 關閉而解鎖，也不能阻擋合法相關 bundle 的內部卸載／rollback 還原。已核實 BAFunctions Create 失敗會走 Burn skip-cleanup 分支；[最小補丁設計](installer-reentry-minimal-design.md)與真正多程序 TDD 仍須落地及實測，沒有新增自製交易引擎、常駐服務或鎖檔修復流程。
- 先前停止於 UAC 的試驗仍保留原收據，不列為通過。上述新證據不等同修正版已通過，亦未證明歷史 `0x800700e8` 的原因；#16 保持開啟。UI 字形修正版仍需完成實際進度／失敗／卸載頁面驗證。

原始日誌、程序及獨立 readback 均保留於本機測試證據包；正式 repo 僅記錄去識別的時序、判定及可重跑測試。官方來源：[停用的 ApplyLock](https://github.com/wixtoolset/wix/blob/v6.0.2/src/burn/engine/apply.cpp#L393)。

## Task I3：先 RED，再加入官方支援的最小 UI

- [x] 對當前 EXE 的抽出資源執行新 UI 測試：因 Progress 頁缺少 `ExecuteProgressActionDataText` 而失敗；保存原始 RED，不能以缺工具/權限失敗代替。
- [x] 新增最小 theme/wxl。保留 `EulaAcceptCheckbox`、logo、ProgressCancel、FilesInUse、FailureMessage、Success/Failure 重啟與關閉按鈕的官方名稱和條件，不改 BA 的選項結果。
- [ ] Progress 頁顯示「Windows Installer 目前作業」與原生 action message；靜態說明「驗證或移除舊版可能需等待，請勿重複啟動安裝」。沒有 action 時顯示誠實的一般階段，不捏造當前阻塞原因。
- [ ] FilesInUse 保留真實程序資訊及原生選擇；文案說明自行儲存/關閉、重試或保留程式稍後重啟。仍保持 `DisableShutdown`，不自動強制關閉 App。
- [ ] Success 頁在 `WixStdBARestartRequired` 時顯示「安裝完成，需要重新啟動以完成舊檔清理」，提供明確「稍後自行重啟」選擇。失敗頁保留真實 HRESULT 與系統說明，另以靜態條件句提示「若顯示 1618／0x80070652，請等待其他安裝完成，再重新開啟本程式」。不發明未存在的 error-code theme variable 或為此新增 BAFunctions；其他錯誤不可被改寫成重入原因。
- [x] 正式與開發 bundle 只引用同一份 UI。擴充 verifier 驗證抽出資源與期望來源 hash、必要節點及條件；缺 action/誤移除 restart 條件/錯 logo 均要失敗。
- [ ] 兩種 authoring 都要至少建置一次並抽出檢查；wxl/localization 參照全可解，沒有殘留 `!(loc.*)`/`#(loc.*)`，Progress/Failure/Success 文字與原生 FilesInUse 選項都不截斷。（兩種真封裝與字形檢查已通過；其他頁面實畫面仍待驗。）
- [x] 重跑 UI 測試取得 GREEN；保留安裝說明同意、logo、DPI 100%/150%、繁中文字串不截斷及鍵盤導覽的真實畫面。兩比例皆實際確認同意勾選啟用安裝、Tab 至選項及正常取消；150% 已以桌面與 BA 視窗各 144 DPI 核對，保存原始畫面，主機雙螢幕已還原 96 DPI。

官方有 action 控制項但原本 theme 沒放，故不需要 BAFunctions DLL、單字翻譯 dispatch 或新 C++ launcher。原生 action 文案可能由 MSI 語言決定，不承諾所有 action 名稱翻成中文。

## Task I4：回歸與交付

- [x] 先執行 `windows/native/test-release.ps1 -ToolchainBin <CI同版工具鏈> -WorkDir <new-dir>`，再沿 `.github/workflows/windows-installer.yml` 執行 TLS、retired-files、legacy safety、MSI validation 與封裝驗證。原生完整 suite、TLS、retired-files、六項 legacy 安全測試及新 MSI 的 early collector／retired MSI 回復均已在同一沙盒通過；新 UI/data 私有包也完成實際 MSI／payload／雙 authoring 封裝驗證。
- [ ] 使用新封裝成品，在同一 guest 串行重跑跨版本/占用/重入/需要重啟、取消與卸載；不開第二個 Sandbox。
- [ ] 對每案保存 source SHA、EXE/MSI/hash、實際 UI、退出碼與 readback。模擬錯誤、等價構造與真正 BA 中斷分開，沒測到不能寫 PASS。
- [ ] 更新操作說明和兩張 issue 的剩餘驗收。issue 18 的 UI 可獨立完成；issue 16 若恢復還未覆蓋則保留開啟，不把它轉移成另一張單而宣稱完成。
- [ ] 完成固定引擎 canonical preflight 與最終獨立 review，將測試與實作提交、push／開 PR；CI 全綠且 GitHub review 修正後合併，再由正式來源成品於沙盒驗收並發布。Neon 八輪上限已用完，後續差異不冒稱由 Neon 審過。

## 關單條件

| 議題 | 必要證據 |
| --- | --- |
| 16 | 無第二筆 MSI、第一筆不受影響、普通雙擊修復真實不一致、前後版本一致且可卸載 |
| 18 | 可見真實作業、等待/占用下一步可讀、3010 對應的成功與待重啟提示明確 |

本計畫不承諾將所有升級時間壓成固定秒數，沒有證據也不把原先五分鐘全歸因於 DLL 占用。

Sandbox 的 guest 內重啟持久性由 [Microsoft 官方文件](https://learn.microsoft.com/en-us/windows/security/threat-protection/windows-sandbox/windows-sandbox-overview) 說明，適用 Windows 11 22H2 以上。**關閉 Sandbox instance 再開會清空；guest 內 Restart 不同於關閉。** 每輪仍須以非映射 sentinel 及登錄實際核對，不能只依文件假定已驗。
