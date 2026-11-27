# Android Doctor 驗證

測試來源隨功能提交；本機診斷紀錄、模擬器資料、金鑰及個人字表不進 repo。Doctor 的使用方式見 [操作說明](doctor.md)。Windows 另行驗收，進度共用 [Doctor 議題](https://github.com/hamanpaul/liu-kai/issues/11)。

## CI 與本機單元測試

```sh
./gradlew --no-daemon :core:check :cli:check :app:testDebugUnitTest assembleDebug
python3 -m unittest discover -s scripts/tests
(cd testpilot && python3 -m pytest)
```

TestPilot 需先安裝 `testpilot[test]` 及 workflow 指定的 TestPilot core。上述命令與 `.github/workflows/tests.yml`、`.project-policy.yml` 的測試步驟對齊；不會自行開啟模擬器。

| 契約 | 測試來源 |
| --- | --- |
| 格式、隱私投影、容量、保留、真實檔案故障、清除及匯出恢復 | `core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/` |
| 單一 producer、佇列、操作關聯、close／staging、寫入失敗與清除重試 | `app/src/test/kotlin/com/hamanpaul/liukai/doctor/` |
| 普通／敏感／未知欄位、真實 InputConnection、標記、預覽與 SAF | `DoctorProjectionDeviceProbe.kt`、`DoctorActionsDeviceProbe.kt` |
| 訂閱解除、回呼例外隔離、弱引用回收、真實 View 觸控與中斷恢復 | `DoctorStatusDeviceProbe.kt` |
| 符號頁英數直出、回字母頁恢復模式、組字中長按標點 | `testpilot/liu_kai_testpilot/cases/soft-symbols-default-direct.yaml`、`soft-root-popup-while-composing.yaml`、`soft-root-popup-composing-direct.yaml` |

## Android 執行條件

使用單一 Android 11 以上模擬器，安裝同一次建置的 debug app、androidTest APK 及 testhost。不要把不同來源的 `.ec` 混用。framework probe 的入口如下，`SERIAL` 必須指定實際測試裝置：

```sh
adb -s "$SERIAL" shell am instrument -w -e mode doctor-unit \
  com.hamanpaul.liukai.test/com.hamanpaul.liukai.ime.TouchDiagnosticsFrameworkTest
adb -s "$SERIAL" shell am instrument -w -e mode doctor-status \
  com.hamanpaul.liukai.test/com.hamanpaul.liukai.ime.TouchDiagnosticsFrameworkTest
adb -s "$SERIAL" shell am instrument -w -e mode doctor-actions \
  com.hamanpaul.liukai.test/com.hamanpaul.liukai.ime.TouchDiagnosticsFrameworkTest
adb -s "$SERIAL" shell am instrument -w -e mode doctor-saf-lifetime \
  com.hamanpaul.liukai.test/com.hamanpaul.liukai.ime.TouchDiagnosticsFrameworkTest
```

必須核對實際 instrumentation 結果為 `INSTRUMENTATION_CODE: -1` 及對應完成訊息，不能只看 adb 的退出碼。`doctor-unit` 用 JUnit 執行相同單元測試並要求執行數大於零、沒有 ignore。一般 framework 入口也會執行 Doctor 單元、操作與狀態測試；`doctor-saf-lifetime` 另測選檔期間 Activity 銷毀。

`doctor-saf-lifetime` 必須在專用模擬器先記錄「Don't keep activities」原值、透過開發人員選項啟用，再執行測試並核對選檔期間真正發生 Activity 銷毀；結束後還原並讀回原值。沒有找到選項或未實際銷毀屬於未完成執行條件，不能記為通過。

弱引用案例先以強引用作正向投遞控制，釋放後使用真正的 `WeakReference`／`ReferenceQueue` 等待回收，再驗證不再投遞。兩秒內未觀察到回收會失敗，沒有略過或假造回收結果。此案例證明該訂閱的生命週期契約，不等於整個 app 沒有記憶體洩漏。測試最後解除訂閱並恢復原 Doctor 開關。

完整輸入回歸及 app 覆蓋報告沿用 `scripts/emulator-e2e.sh`。覆蓋資料須核對實際編譯 class ID；100% 原始驗證結果保留，不降低全專案門檻。Kotlin inline 保留本體、編譯器產物、無正常入口的防衛條件與真正缺測分開對帳；不能把它們全部稱為編譯器問題。既有 [高速連鍵問題](https://github.com/hamanpaul/liu-kai/issues/22) 的失敗持續揭露。

本次 app 為 3196／3287 行（97.2%）、1098／1211 分支（90.7%），原 100% 驗證實際失敗。[具名覆蓋對帳](doctor-coverage.md) 與來源層方法／計數摘要一併進 repo，不公開原始私有 EC 或裝置資料，也不把未強制的競態、極值或 provider 防衛說成已測。

## 長測與效能範圍

發版要求一小時加針對性及防回歸驗證；八小時屬後續記憶體等長跑觀測，不阻擋發版。一小時成功不代表八小時或無洩漏。效能比較使用同一成品交替關閉／啟用 Doctor，核對提交數、drop、持久事件及操作關聯；dispatch 計時不代表延後 click 到文字輸出的端到端延遲。

## 2026-11-27 驗證紀錄

- 本機 CI 對等檢查的原始 XML：core 257 項、CLI 9 項，以及後續補測後的 app 68 項，failure／error／skip 均為零；core／CLI 100% verification、TestPilot 覆蓋門檻及發版完整性檢查亦成功。
- 同一 Android 成品在 ART 執行 68 項並通過真正弱引用回收、正／負向投遞控制及狀態測試；單元測試通過不代替 framework 的 GC 結果。另以真 View 長按與選單選取驗證設定／語音各自的操作關聯，這是 component callback 驗證，不等同跨 Activity／IME 導覽。
- 匯出清理案例在輸出完整 ZIP 關閉後，僅移除本次測試自行建立暫存子目錄的寫入權限，真實觸發刪除失敗；驗證已交付輸出不被 discard、狀態及歷史保留，還原權限後能取消預覽並繼續落盤。此 fixture 需要 Linux／Android POSIX 檔案權限，沒有宣稱 Windows native JVM 已驗證。
- 新成品的六項標點／輸入定向回歸全部通過：組字中長按標點、選取 `@` 直接輸出、符號頁英數預設、組字切符號、萬用字元／同音查字與英文重音。
- Doctor 開啟時，另以三個既有 TestPilot 案例真正開啟設定、由子頁回主頁，以及切到系統語音再回嘸．改，均通過。逐案保存的 KEY_ACTION／FOCUS 增量分別為 2／3、2／4、6／3；只以各案時間範圍關聯，不聲稱非同步焦點事件與按鍵共用 operation ID。結束後以正式 UI 還原原停用狀態，保留紀錄及 epoch。
- 設定頁操作回歸第一次在 BACK 注入時遇到系統 `InputDispatcher` 無焦點視窗，預覽仍開啟且佇列為空。測試改為先確認預覽視窗取得焦點，保留單次 BACK 與原取消／清除斷言後通過；產品 APK 未改動。回歸另涵蓋真實 SAF 儲存、取消、權限錯誤恢復、清除競態及舊預覽拒絕。
- 本輪額外重跑 `doctor-saf-lifetime` 時，外層導覽在有界搜尋內找不到開發人員選項，實際 instrumentation 未啟動；不算本輪通過。該次尚未切換開關，獨立讀回原值仍為 0。Activity 銷毀契約保留先前真正實測的證據，並依未變更的設定頁／SAF 方法及非空 build 呼叫契約核對範圍。
- 發版整理移除三個無引用的 controller wrapper，以及 writer 無呼叫者使用的 nullable／預設 build 參數；全部三個建構呼叫原本就明確傳入非空 metadata，保留編碼拒絕與寫入故障路徑。先前一小時及效能證據保留原成品範圍，本輪以新成品補驗清除重試、實際落盤、普通輸入及公開操作。
- Git 文字換行正規化後，本機 CI 與實機成品的 91 個 app、155 個 core production class 逐檔 SHA-256 完全相同；沒有把來源表示差異當成產品改動，也沒有以此宣稱不同版本的 binary 完全相同。
