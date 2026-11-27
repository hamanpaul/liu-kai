# 安裝與字表計畫對抗摘要

日期：2026-11-27。模型：透過 Windows 原生 agy CLI 使用 `gemini-4-neon-high`。前三輪為規劃對抗，後續實作審查累計至第八輪，已用完上限。審查模型只讀取允許的文件與程式碼；Root 的實際建置與沙盒驗證另列，不將文字審查當作產品測試。

使用者已裁決兩個入口維持原有主要音規則，先核對資料；該裁決納入第二輪與最終文件。

## 2026-11-28 後續實測與獨立覆核

新 BAFunctions guard 的私有候選已通過真正 Reentry、3010 後重啟 readback，並完成 guarded 舊版 fixture 的正常 embedded 卸載及跨版本升級；兩份封裝與七項多程序核心亦通過。完整 native test-release、22 項 scripts 測試及資料門禁已通過。獨立 adversarial review 的兩項 P2（卸載查詢缺漏、Recovery 漏算原生 3010）已依負向 TDD 修正並覆核，未發現剩餘 Important。PS5／PS7 各 749 項契約通過；不是 749 次安裝。

這些後續 guard／harness 差異沒有交給 Neon 第九輪。真正非提權 BA 故障注入仍為 0，Recovery 缺 COM／TSF absence 證據時只能 NOT_COVERED；歷史錯誤根因與未裁定讀音不因此算完成。以下各輪與較早實測描述保留當時狀態，最新範圍以 [Windows 驗證紀錄](../../../windows/VALIDATION.md) 為準。

## 逐輪採納與反駁

| 輪次 | Neon 主張 | 最終處理 |
| --- | --- | --- |
| 1 | 必須測真正跨版本與 3010 未重啟 | 採納；baseline 0.1.1、私有候選 0.1.2/70.1.4，manifest/目錄一致，分別驗證重啟前後 |
| 1 | 兩份表不能互相證明正音真值 | 採納；集合差與權威正音分開，未判定關聯不自動修正 |
| 1 | 先不要預設整份客製 ThemeFile | 以固定 WiX 原始碼核實後收斂：官方 RtfTheme 缺 action 控制項，最小 theme 有必要；不寫新 BA |
| 1 | 用直接 MSI 升級構造災後狀態即可 | 部分駁回；產品刻意有 bundle 升級 guard，直接 MSI 僅可作額外 oracle，不代替真 BA 中斷 |
| 1 | 長尾未判定可移到新單後結案 | 駁回；保留原 issue 16/19 的未完成驗收，不移走未知來關單 |
| 2 | Sandbox Restart 就清空，必須新建可重啟 VM | 駁回並由第三輪接受：Windows 11 22H2+ guest 內 Restart 保留狀態；關閉 instance 才清空。仍加本地 sentinel/登錄/boot time 預檢 |
| 2 | BA 故障注入不能太早且須核對目標 | 採納；只在 InstallFinalize 成功後的非提權 BA 窗口注入，最多兩次，未形成目標現場記未覆蓋 |
| 2 | 1618 需程式化 error-code theme 條件 | 駁回新變數/BAFunctions；保留原始錯誤文字，加靜態條件句操作引導 |
| 2 | 雙 authoring/UI 與 JDK 預檢需明確 | 採納；兩份 bundle 都建置抽取檢查；已找到可執行的原生 JDK 17，但未冒稱 Gradle 已通過 |
| 3 | 沒有第二筆 MSI 不代表撞過鎖 | 採納；加入 Apply 時間重疊、明確拒鎖 log、零第二筆交易三條件，未命中不能算 PASS |
| 3 | 未預期缺字與裁決表需要嚴格門禁 | 採納；缺字回非零、封閉 status、source 非空、禁止孤兒/重複裁決，不容許裁決掩蓋結構錯誤 |

第三輪結論：無剩餘 Critical、無過度設計；補入兩條具體 Important 門檻後即可定稿，不需要第四輪。上述門檻已寫入兩份 plan，仍未執行。

## 實作審查與目前驗證

- 第四輪讀檔遭拒且沒有有效回覆，保留該次紀錄，不算成一次完成的程式碼審查，也沒有重置八輪上限。
- 第五輪資料審查的五項發現經 TDD 修正；第六輪確認資料部分通過。完整資料核對仍有 3,738 筆未裁定關聯，沒有修改主要音規則、候選順序或 generator，也不以兩份相同資料互相證明正音真值。
- 第六、七輪針對實作補足候選 MSI commit 判讀、版本域分離、原始 log 來源、BA 身分／提權／祖先鏈核對、3010 狀態保留，以及狀態 fixture 的有界等待與失敗處理。相關純測試及原生 fixture 留在 repo。
- 第七輪聲稱 owner `Token` 不存在，經 Root 在實際客體唯讀核對後駁回：欄位存在且非空，早期報告只是未投影該欄位；沒有匯出 Token 內容。第八輪接受此證據，結論為 `SAFE_TO_RUN_RUNTIME`，UI／資料審查通過。
- 第八輪後，Root 的實際 PS5／沙盒執行找出原生輸出、COM 讀取、含空白路徑、原子 JSON 及 live-log 共享讀取問題。各項回歸與修正記於 [情境 TDD 交接](installer-scenarios-tdd-handoff.md)；沒有第九輪審查，也不冒稱 Neon 審過這些後續差異。產品 MSI 與 BA 終止授權規則未改。
- 已完成兩份 bundle 真實建置／抽取、字形檢查，以及 100% 和 150% 的實際畫面驗證；主機縮放已恢復原值。現有完整原生回歸、診斷 ZIP／rollback 補充測試、資料測試與目前私有快照的 canonical policy gate 通過；此 gate 使用 `skip-tests`，原生／Python 測試另行執行，未宣稱重跑 JVM。
- 後續控制組已實際完成跨版本 MSI 升級並重現重入失敗：第二客戶端雖被 MSI 1618 拒絕，卻在結束時移除第一筆共用的 Burn 登錄；第一筆 MSI 提交 3010，Burn Full 登錄失敗，重啟後僅剩 MSI。普通空參數重開同一 EXE 已恢復登錄，同 EXE 卸載及 guest 重啟後的完整核對也通過。完整時序見[安裝計畫更正](2026-11-27-windows-installer-flow.md#2026-11-27-控制組實測與設計更正)。這些是原始控制組的測試結果，防重入產品補丁與真 BA 中斷驗收仍未完成，不得關閉 #16；更早的 UAC 取消收據也未改列 PASS。
- 原先「Burn 自帶執行鎖」的規劃與審查假設錯誤。固定來源顯示 `ApplyLock` 的 mutex 程式被 `#if 0` 停用。Root 已更正設計，新增第一筆最終狀態損壞的負向 TDD：PS5／PS7 各 505 項純情境檢查通過；被動測試入口補上 `/norestart`，普通空參數入口仍保持空參數。這些後續測試／設計差異沒有再交 Neon 審查，也沒有第九輪。防重入產品程式尚未實作，正式交付 commit 及最終快照 gate 仍未完成，沒有 push／PR／release。

## 可開始實作的範圍與保留限制

- [安裝計畫](2026-11-27-windows-installer-flow.md)：先收集真跨版本/重入/恢復證據，再做最小官方 UI 資源修改。#16 的恢復驗收沒命中就不能關單。
- [資料計畫](2026-11-27-readings-consistency.md)：先交付完整只讀核對、同步門禁與裁決報告，不改 query/generator/候選資料；未知正音仍保留在 #19。
- [共用設計](../specs/2026-11-27-installer-readings-design.md)：使用者規則、資料計數、來源及不做事項。
- 最初規劃對抗沒有產品操作；後續原生實作與一個 Windows Sandbox 的建置／安裝證據如上。未操作 WSL 或 Android 模擬器，未在本機安裝或重啟；尚無正式交付 commit／push／PR／release。文件檢查不代表產品測試。

## 可核對的官方依據

- [WiX 6.0.2 ApplyLock](https://github.com/wixtoolset/wix/blob/v6.0.2/src/burn/engine/apply.cpp#L393)：mutex 程式被 `#if 0` 停用，不能依賴此函式防重入；原規劃引述已更正。仍不新增安裝框架或常駐服務。
- [WiX 6.0.2 ACTIONSTART 更新](https://github.com/wixtoolset/wix/blob/v6.0.2/src/ext/Bal/stdbas/WixStandardBootstrapperApplication.cpp#L1101)：使用既有 action 控制項。
- [WiX 6.0.2 RTF Progress 頁](https://github.com/wixtoolset/wix/blob/v6.0.2/src/ext/Bal/stdbas/Resources/RtfTheme.xml#L57)：預設沒有 action 文字控制項。
- [Microsoft Sandbox](https://learn.microsoft.com/en-us/windows/security/threat-protection/windows-sandbox/windows-sandbox-overview)：Windows 11 22H2 起 guest 內重新啟動與關閉的狀態保留不同。
