# Doctor Android 先行發版

使用者於 2026-11-27 決定：「補上真正尚未完整測到的執行條件，清掉確定無用程式後 android 就可以先發版了」，Windows 另與 Thala 協調測試。

本計畫延續 [Doctor 計畫](2026-11-25-doctor-mode.md)，只調整交付順序。共用格式與 Android 實作、測試在本 PR 交付；Windows 程式及其未完成驗證留在另一工作樹。共用 issue 使用完整網址 https://github.com/hamanpaul/liu-kai/issues/11，雙端均驗收並發版後才關閉。

## 已核對的起點

- 分支 `feature/11-doctor-android-first`，基底 `e045f978eb52001abc8b632482a6f4536cbdf2ab`；移入 Android／共用變更，Windows 原始碼與 installer workflow 沒有移入。
- 新增測試目前只有本機來源，尚未 commit／push；本次須隨功能一併納入 GitHub。
- 原報告為行 3196/3291、分支 1095/1213。95 個未覆蓋行分為 87 個 inline 保留本體／編譯器產物、3 個未使用 accessor、5 個真正未完整觸發的行；118 個未覆蓋分支中仍有 80 個執行條件須逐項核對。
- app 已用 JaCoCo 0.8.15、Kotlin 2.0.20。官方 [filtering 說明](https://github.com/jacoco/jacoco/wiki/FilteringOptions) 記錄 Kotlin 2.0 的 inline 行覆蓋限制與尚未完成的 inline 分支處理；這不能代替實際 bytecode／呼叫端證據。

## 工作與驗收

- [ ] 對照目前 XML、bytecode 與既有案例，建立真正缺測的逐項對帳；不得把未觸發條件概括稱為不可達。
- [ ] 在 `app/src/test`、`app/src/androidTest` 或既有 TestPilot 來源補案例，涵蓋可達的 lifecycle、訂閱者投遞、staging／關閉、計數邊界、writer 失敗恢復及 SAF 預覽失效。
- [ ] 新增測試驗證契約與失敗結果；若發現產品缺陷，取得對應 RED 再做最小修正。單純補覆蓋、原行為即正確時如實記為補測，不捏造缺陷 RED。
- [ ] 確認引用後刪除三個未使用的 controller wrapper；保留 producer 仍使用的欄位及正常契約。
- [ ] 不加私有測試 hook、不用反射修改狀態、不偽造 provider；不移除必要防衛、不為數字改 inline 或升級整套工具鏈。
- [ ] 執行單元／ART／framework 針對性驗證及 TestPilot 回歸，核對實際退出碼、來源 SHA 與覆蓋 class ID；真正可達缺口須完成，才符合使用者的先行發版條件。
- [ ] 剩餘編譯器保留碼逐項列方法、來源、bytecode 與呼叫端證據，保留原始覆蓋數字及 100% 驗證結果。此次條件式接受不改全 repo 的門檻，不排除整個 class，也不將展開後的真分支自動豁免。
- [ ] 保留既有 https://github.com/hamanpaul/liu-kai/issues/22 快打基準的原始失敗與來源比較，確認本次沒有新增輸入退化；不將已知失敗改成 PASS。
- [ ] 以目前來源完成 Doctor 啟停、標記、預覽／匯出、清除、隱私與標點回歸。先前一小時與效能證據保留其原成品範圍；若新修正改變實際執行邏輯，對修正後來源重新驗證一小時及成對效能。僅刪除無引用 accessor 時須記錄等價性依據，仍以新成品做針對性回歸。
- [ ] Terra 獨立檢查新增差異，修掉實質問題。既有 Gemini review 八輪結果保留，不另外擴大 review 輪數。
- [ ] 同步說明及 changelog，依遠端實際版本遞增最小版號；本次目標 Android `0.1.1`／versionCode `3`、repo 批次 `0.1.3`，Windows 保持 `0.1.0`。
- [ ] 對最終來源跑 CI 對等 preflight，通過後 commit、push／PR，待 CI 綠及 GitHub review 修正完成後 merge。
- [ ] 從正式提交用既有 Android release 腳本建置，核對原簽署身分、離線權限、升級／資料保留、`SOURCE.txt` 與 SHA-256，以實測的同一 APK 發布 `android-v0.1.1` 並下載核對。

## 測試資源

測試單線執行，每次最多一個 Android 模擬器或 Windows 沙盒，Android 可寫目錄在 E 槽。D 槽維持使用者本輪指定的 Windows 顯示 80 GiB 以上。

記憶體依使用者的 Windows＋WSL 可用量語意觀察，分別保留 Windows API 與 Linux `MemAvailable` 原始讀值，不把兩者直接相加，也不把 WSL 快取說成已歸還 Windows。護欄是操作性估算，不能宣稱嚴格實體聯集證明；另保護 Windows 即時配置餘裕。Windows 實測前由 Root 審查最小 guard 修正並與 Thala 確認窗口；不得因此停止 WSL、改系統設定或中斷其他工作。
