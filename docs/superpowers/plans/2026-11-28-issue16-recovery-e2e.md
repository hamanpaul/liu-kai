# Issue 16：MSI／Burn 中斷恢復驗收

使用者要求完成真正恢復測試、修正發現的問題，驗收通過後補 PR 並關閉 issue。延續原安裝計畫 I2，測試碼進 repo；本輪不以元件測試或人工寫入安裝登錄替代實際 MSI／Burn 現場。

## 驗收鏈

1. 單一 4 GiB Windows Sandbox，核對本機可用 RAM ≥30 GiB、D 槽 ≥82 GiB；不啟動或查詢 WSL，不在主機安裝。
2. 官方 0.1.1 baseline 與正式 0.1.2 candidate 都先核對來源與 SHA-256。初始化合成設定及 Doctor 歷史，獨立核對安裝基線。
3. 真正 MSI 提交後，只中斷已驗證的獨立候選 BA/UI。核對其 EXE、SHA-256、PID、建立時間、root ancestry 與持有的 process handle。Burn 引擎、msiexec、服務及主機程序均不得中斷。
4. 沙盒預設可能令獨立 `wixstdba.exe` 也為 elevated；新增明確 opt-in 只接受可信獨立 BA，default 仍拒絕。不得以 elevated=true 判斷它就是引擎，也不得放寬 engine/setup hash、未知角色或身分不符的拒絕。
5. 原始引擎結束後，獨立證明新 MSI 已提交但 Burn 登錄不一致，再普通重開同一 EXE（零參數）恢復。保留正常安裝介面與操作結果；需要重啟時只重啟同一 guest，以非映射 sentinel 與 boot time 核對。
6. 驗證新 MSI／Burn、payload／manifest、COM／TSF、owner、舊登錄清理與合成設定／Doctor 保留；同一 EXE 正常解除安裝，再核對重開後不存在產品登錄、檔案與待刪除項目。

## 執行與交付

先補可觀察的負向回歸（MAI Code 2 Max），Root 執行語意 RED，再做最小修正與 GREEN。真正故障注入保留每次身分／時序／原始日誌，沿用最多兩次有目標的嘗試；沒形成現場不算通過。不可在引擎仍工作時把暫時登錄狀態當作永久損壞。

新發現的測試器缺陷與產品缺陷分開記錄；必要的修正保持最小。獨立 review、canonical preflight、CI 與 GitHub review 均通過後合併，以 PR 與實際恢復證據更新並關閉 issue 16。歷史 `0x800700e8` 的唯一根因若仍未知，明確記錄，不因測試注入就宣稱已找到。

## 本輪已取得的證據與剩餘條件

正式 0.1.2 已實際在 MSI native 3010 後中斷獨立 BA，取得 `0x800700e8`；原 engine 仍自行完成 Full／ARP 登錄，故沒有形成第 5 步要求的缺登錄前提。原 Recovery 收據維持 NOT_COVERED。相同 EXE 的普通維護／解除安裝與逐階段 guest 重啟已實跑，最終 MSI／ARP／COM／profile、目錄、同層退休 DLL 及產品待刪均清除，設定／Doctor 指紋保留。

測試器的空日誌、整數 PID 查詢、held handle、native 3010、post-exit 完整日誌及卸載 absence／退休檔查核均有 RED／GREEN；獨立審查的重要缺陷已修。未經完整真缺登錄情境驗證的自動續跑 runner 不納入 PR。真正 BA stop 計數為 1／2，沒有為製造故障改寫產品登錄或終止引擎。

使用者於 2026-11-28 明確接受以「真實中斷後自行維持完整狀態，且可正常維護／卸載」作為新版驗收，要求完成 PR 並關閉 issue 16。依此增加明確的 `AcceptRetainedBundle` 驗收選項，預設仍保留原缺登錄恢復模式；原始 NOT_COVERED 不改寫。PR／合併仍須 canonical preflight、CI 及 GitHub review。實測與合成／日誌重放的範圍分別記在 `windows/VALIDATION.md`。
