# Windows 安裝中斷恢復測試

此流程使用 `windows/native/tests/wix-installer-scenarios.ps1`，只可在本輪擁有的 Windows Sandbox 中執行。使用正式來源、雜湊已核對的 baseline／candidate EXE，以及包含沙盒 ID、非映射 guest-local sentinel、版本與 fixture 雜湊的 input manifest。輸出目錄必須是新目錄，不能與產品目錄重疊。原始日誌與完整快照留在本機，可能含安裝交易識別資訊。

## 中斷與安全範圍

`-Scenario Recovery -RecoveryAttempts 1` 啟動普通候選安裝介面，只額外指定 Burn 日誌路徑。操作同意說明與「安裝」，讓真正 MSI 提交後才中斷獨立 BA。工具核對 EXE／SHA-256、PID／建立時間、程序樹與持有的原生 handle；只中斷核對通過的獨立 BA，不中斷 Burn engine、msiexec 或 Windows 服務。

Windows Sandbox 預設帳戶有時沒有標準的分割權杖，獨立 `wixstdba.exe` 也會顯示 elevated。此環境需明確指定 `-AllowElevatedBaUi`；它仍要求可信的獨立 BA 雜湊及完整 ancestry，不允許 engine、setup image 或未知身分。預設仍拒絕 elevated 目標，沒有修改 UAC／安全政策。

預設模式仍要求「新 MSI 已提交、原 engine 已退出、Burn 登錄不一致」的現場，未形成則是 `NOT_COVERED`。使用者於 2026-11-28 明確接受新版也可自行保住完整登錄；依此驗收時明確指定 `-AcceptRetainedBundle`，輸出記錄 `retained_or_repaired` 模式。這個模式另要求已驗證 BA stop、原 engine 已退出，且同一 EXE 普通維護／卸載、完整讀回及所有重啟／保留條件仍須通過。

每輪最多兩次有目標的嘗試，不可用手改登錄、直接 MSI 構造或元件測試替代。`formed-state.json` 保存實際 `bundle_consistent` 值，完整登錄不能寫成已修復缺登錄；原嚴格模式的 `NOT_COVERED` 亦不得覆寫。原始日誌、中間收據及後續明確模式的驗收收據分別保留，不能只看 UI 成功或 MSI 3010 就判定完成。

## 遇到 3010 時續跑

不要在安裝介面選「立即重新啟動」而中斷測試器保存結果；選「稍後重啟」，等原程序結果與 receipt 寫完，再由操作端只重啟該 guest。原測試器會保留需要重啟的中間收據，不能將其當作最終 PASS。

後續由操作端逐階段保存原生結果與獨立 readback：重啟後普通重開同一 EXE，重新核對完整收斂；若維護回 3010，先重啟，再用同一 EXE 普通介面解除安裝；卸載回 3010 時，再重啟並實讀 absence／設定／Doctor 保留。每階段保留來源 SHA-256、空安裝參數、native exit、開始／結束時間、guest ID 與非映射 sentinel，不能改寫原始收據。

純函式 `Test-WixInstallerScenarioRecoveryRebootProof` 及對應契約驗證前後 boot time、程序結束時間、重啟後觀測時間與下一階段時序。它不執行安裝或重啟；219 個合成斷言通過也不等於缺登錄恢復已通過。此輪沒有交付未經真正情境驗證的自動續跑 runner。

卸載後使用已從驗證成功的安裝快取出的 `LiuKaiRegister.exe --status`，搭配 HKCU／HKLM 的 32／64 位 COM／profile registry views、MSI／ARP、產品目錄與產品相關 pending deletes。原生 observer 沒有提供全部 TSF category 的枚舉，因此不宣稱已枚舉所有 category。缺失、拒絕存取或 observer 雜湊不符維持未知；看到實際殘留則失敗。

## 回歸測試與證據界線

CI 在 PS5／PS7 執行程序樹、登錄狀態、重啟及 post-exit 證據的純契約；既有 scenario／native-capture 測試另涵蓋空日誌、MSI native 3010 與持有 handle 的身分核對。純合成資料永遠不算實際安裝 PASS。

`wix-installer-absence-snapshot-tests.ps1` 在擁有的 guest 正常卸載並完成檔案清理後執行，參數為 `-RegistrationStatusExe <verified-observer.exe> -WorkDir <new-dir>`。它使用實際唯讀 observer／registry／filesystem，並以私有副本測試失效雜湊及目錄殘留；不讀使用者設定、不修改產品註冊。

原歷史事故的 `0x800700e8` 唯一原因若未確認，必須維持未知。受控 BA 中斷能驗證恢復能力，不等於重現歷史事故的全部因果。
