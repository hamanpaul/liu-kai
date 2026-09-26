# TestPilot plugin 與全面 100% 覆蓋率計畫

> 2026-09-26。使用者要求：以 TDD 建立 liu-kai 的 TestPilot plugin，覆蓋率目標為 liu-kai 全部程式碼 100%（行與分支），不接受排除清單；觸發不到的程式碼重構或刪除。plugin 本身也須 100%。

## 範圍與量測

| 對象 | 量測方式 | 門檻 |
|---|---|---|
| `core`（Kotlin/JVM） | JaCoCo（`:core:jacocoTestCoverageVerification`） | LINE、BRANCH 100% |
| `cli`（Kotlin/JVM） | JaCoCo（`:cli:jacocoTestCoverageVerification`） | LINE、BRANCH 100% |
| `app`（Android） | debug 版 JaCoCo offline instrumentation：TestPilot 案例在模擬器上執行後，由 debug 專用 receiver 匯出 `.ec`，與 app 的 JVM 單元測試 exec 合併（`:app:jacocoE2eCoverageVerification`） | LINE、BRANCH 100% |
| `testpilot/`（Python plugin） | pytest-cov `--cov-branch --cov-fail-under=100` | 100% |

`testhost` 是端對端測試的宿主 App，屬測試基礎設施，不列入產品覆蓋率。

## 設計決策

1. **Android glue 變薄**：把判斷邏輯從 `LiuKaiImeService`／`ImeView`／`SettingsActivity` 抽到 `core`（純 Kotlin），例如實體鍵轉引擎事件、單按 Shift 偵測、Enter 行為、密碼欄判定、組字區同步。這些都以 TDD 在 JVM 上測到 100%；Android 端只剩呼叫，由案例覆蓋。
2. **minSdk 30**：刪除 API 30 以下的版本分支。原本這些分支在 API 35 模擬器上觸發不到。
3. **IME 狀態可觀測**：覆寫 Android 標準診斷介面 `InputMethodService.dump()`，輸出模式、組字、候選與按鍵的螢幕座標（`LIUKAI_STATE <base64 JSON>`）。plugin 以 `dumpsys activity service` 讀取後點選。
4. **plugin 以 adb 黑箱驅動**：
   - 按鍵用 `input keyevent`／`input text`／`input keycombination`；
   - 點選用 `input tap`，長按用 `input swipe`；
   - 欄位文字用 `uiautomator dump` 讀取；
   - 設定切換用 `settings put`。
   case YAML 描述環境、步驟、通過條件與證據，符合 TestPilot「case 契約＝YAML base＋test report 必要」。
5. **舊 androidTest E2E 淘汰**：由 TestPilot 案例完整取代，`ImeE2eTest`／`RealTableSpotTest` 與 uiautomator 相依移除。

## 階段

| # | 內容 | 完成條件 |
|---|---|---|
| T1 | JVM 覆蓋率工具（JaCoCo＋`scripts/coverage-gaps.py`） | 報表可產生 |
| T2 | core 新增 IME 決策類別（TDD） | 先紅後綠，100% |
| T3 | core／cli 既有程式補測試到 100%；補上的測試以故意破壞程式抽驗能轉紅；死碼刪除 | `:core:check`、`:cli:check` 綠 |
| T4 | app 重構：使用 core 決策類別、minSdk 30、`dump()` 狀態、debug 覆蓋率匯出、JaCoCo E2E 報表任務 | 建置成功 |
| T5 | TestPilot plugin（TDD）：adb 包裝、狀態／UI 解析、step 動作、Plugin、runner＋報表、覆蓋率匯出 | pytest 100% |
| T6 | 案例 YAML：plan 第 6 節的全部行為＋app 所有路徑 | 案例全綠，app 覆蓋率 100% |
| T7 | 移除舊 E2E、更新腳本／CI／文件 | policy_check 0 FAIL，CI 綠 |
