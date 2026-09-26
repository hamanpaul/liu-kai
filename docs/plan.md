# liu-kai 設計與實作計畫

> 狀態：2026-09-26 定案（路線 B：自製 Kotlin IME）。M0–M5 已完成；M6（真實字表驗收）待使用者提供正版字表檔。
> 來源：ChatGPT 設計討論（2026-09-14～26，見 `docs/research/2026-09-26-chatgpt-discussion.md`）＋ 使用者裁決。

## 1. 背景與決策

- 官方 Android 版嘸蝦米 PRO 有「點候選字卻無法選字」的問題；不修改官方 APK，改做一支獨立的 Android 輸入法。
- 字碼來源採使用者合法持有的正版表格（「LIU FOR LINUX」附件的 `liu_ibus_final.txt`，以 `lime_liu7.txt` 驗證區段）。**字表不進 repo、不進 APK**，只在手機端由使用者匯入。
- 討論中比較過四種底座：
  - A：fork OhMyBias Android——沒有 LICENSE，排除。
  - B：自製 Kotlin IME。
  - C：Trime + Rime。
  - D：fcitx5-android。
- ChatGPT 建議自用先走 C。使用者於 2026-09-26 裁決改走 **B**，理由是要交付一支自己的 APK。C／D 保留作為行為比對的參考。
- clean-room 原則：rime-liur、openxiami 沒有授權，只能參考其公開文件描述的行為，不得複製程式碼或資料。

## 2. 範圍

### MVP（v0.1）

| 類別 | 功能 |
|---|---|
| 核心 | 四碼查字、空白上屏首選、點選候選上屏、數字鍵選字、VRSF 選字、候選翻頁、Backspace／Esc／Enter、中英切換 |
| 鍵盤 | 軟鍵盤（字根鍵 + 基本數字符號層）、藍牙／外接實體鍵盤直打 |
| 字表 | 手機端匯入正版表（SAF 選檔）、編譯成私有二進位格式、匯入報告（筆數／雜湊） |
| 進階 | 萬用字元查字、同音字／讀音查詢、日文平假名／片假名輸入 |

### 不在 v0.1

自訂詞、全形／半形切換與中文標點、學習調頻、雲端或任何網路功能、上架。

## 3. 授權與資料邊界

- repo 與 CI 只能放**合成字表**：`core/src/test/resources/fixtures/`，以及 debug 版內建的 demo 表。
- 真實字表放在 repo 外（建議 `~/prj_pri/liu-kai-data/`）。轉換產物、含真實字碼的測試輸出與截圖都不得 commit。
- 讀音資料採 Unicode Unihan `kMandarin`（Unicode License v3，可隨 APK 散布，授權聲明見 `THIRD_PARTY_NOTICES.md`），由 `liu-kai-cli gen-readings` 轉成注音後 commit 產物 `core/src/main/resources/readings.tsv`。
- APK 不宣告 `INTERNET` 權限；`android:allowBackup="false"`，並以 `dataExtractionRules` 排除字表。

## 4. 架構

```
liu-kai/
├─ core/     純 Kotlin/JVM：字表解析、區段切分、編譯格式、輸入引擎狀態機、萬用字元、讀音索引、假名轉換
├─ cli/      JVM CLI：在桌機把正版表轉成中性 TSV + manifest，驗證切分與統計（不產生可散布物）
├─ app/      Android IME（InputMethodService、鍵盤、候選列、設定與匯入）
└─ testhost/ E2E 用的宿主 App（EditText 畫面 + UiAutomator instrumentation）
```

- `core` 不依賴 Android，所有輸入行為都在 JVM 上單元測試。`app` 只把 KeyEvent／觸控轉成引擎事件，再把引擎動作轉成 `InputConnection` 呼叫。
- 引擎是明確的狀態機：輸入 `EngineEvent`，輸出 `EngineAction` 列表（`Commit`、`SetComposing`、`ShowCandidates`、`PassThrough`…），不直接碰 Android API。

## 5. 字表管線

```
liu_ibus_final.txt ─┐
lime_liu7.txt ──────┼─► parse ─► section split ─► normalize ─► CompiledTable (binary) ─► app 私有目錄
liu70_jp.* ─────────┘                         └─► neutral TSV + manifest（cli，僅本機驗證）
```

- **格式**：解析 SCIM／IBus（`BEGIN_TABLE…END_TABLE`，欄位為 code、text、freq）與 CIN／LIME（`%chardef begin…end`、`%gen_inp`、`%cname`、`%keyname`）。
- **區段切分**（ChatGPT 提出，需以真實檔驗證）：
  - 解析 `lime_liu7.txt`，第一個 `%gen_inp` 之前的資料為繁中，其餘區段以 `%cname`／`%chardef begin` 切開，並跳過 `%keyname begin…end`。
  - 四段的 `(code,text)` 串流須與 IBus 的 `BEGIN_TABLE…END_TABLE` 逐筆相同，再把第一段對應回 IBus 的頻率欄。
  - 只有 IBus 檔時：以 manifest 記錄的區段雜湊與首尾錨點切分，或改用 `liu70_zh_tw.cin`。
- **驗證基準**（ChatGPT 自報，待本機核對）：
  - 繁中原始 28,816 列、去重後 28,755 組、21,199 個碼、17,792 個輸出字。
  - 日文段 13,760 列。
- **正規化**：字碼轉小寫；同一 (code,text) 去重，保留首次順位；候選順序以來源順位為準，不隨使用而調整。
- **編譯格式**：版本化二進位檔，內容有排序後的字碼陣列、各碼候選清單，以及字→碼反查表（供萬用字元與同音清單標示字碼）。整份載入記憶體，繁中約 2 MB 以內。

## 6. 引擎行為規格

信心欄引自討論結論；「需實測」由使用者對照官方行為確認，實測後更新本表。

| # | 行為 | v0.1 規則 | 信心 | 需實測 |
|---|---|---|---|---|
| 1 | 字根輸入 | 字表字碼用到的字元都是字根鍵；組字長度上限＝字表最長碼 | 高 | 抽測 |
| 2 | 空白 | 有候選時上屏首選；無組字時送出空白；有組字但無候選時保留組字 | 高 | 是 |
| 3 | 點選候選 | 觸控候選列任一項立即 `commitText` 並清空組字 | 高 | 是 |
| 4 | 數字鍵 | 組字中，1–9、0 選目前頁第 1–10 個候選 | 中 | 是 |
| 5 | VRSF | v／r／s／f＝第 2／3／4／5 候選 | 高 | 是 |
| 6 | VRSF 衝突 | 「碼＋鍵」是任一合法碼的前綴（含完整碼）時當字根；否則只有在「碼」是完整碼且候選數足夠時才選字；兩者都不成立時當字根附加 | 中 | 必測 |
| 7 | 翻頁 | PageDown／PageUp、`=`／`-`（不在字根集內時）；觸控候選列可橫向捲動 | 低 | 必測 |
| 8 | Backspace | 組字中刪最後一個字根；無組字時交給 App | 高 | 是 |
| 9 | Esc | 清空組字 | 中 | 是 |
| 10 | Enter | 組字中送出原始字碼字母；無組字時交給 App | 低 | 必測 |
| 11 | 中英切換 | 實體鍵盤單按 Shift；軟鍵盤「中／英」鍵 | 中 | 是 |
| 12 | 自動上屏 | v0.1 不做（預設關閉） | 低 | 必測 |
| 13 | 萬用字元 | `?`＝恰一個字根，`*`＝零到多個字根；結果最多 200 筆，每筆標示字碼 | 中 | 必測 |
| 14 | 同音字／讀音 | 觸控長按候選，或實體鍵盤在組字後按 `` ` ``，列出首選字（或長按字）的注音與同音字；同音字依字表頻率排序 | 中 | 必測 |
| 15 | 日文模式 | 以字表日文段查字；候選提供平假名與對應片假名兩種形式；軟鍵盤「日」鍵、實體鍵盤 Ctrl+J 切換 | 低 | 必測（待看日文段資料格式後細化） |

## 7. IME 前端

- `LiuKaiImeService : InputMethodService`：
  - 輸入畫面固定為「候選列＋鍵盤」；偵測到實體鍵盤時隱藏鍵盤區，只保留候選列。這樣兩種模式共用同一條候選列，不另用 `onCreateCandidatesView`。
  - 實體鍵盤在 `onKeyDown`／`onKeyUp` 處理：修飾鍵、長按重複、單按 Shift 判定。
  - 組字用 `setComposingText`，上屏用 `commitText`。App 不支援 composing 時，改在候選列顯示組字。
  - 密碼欄（`TYPE_TEXT_VARIATION_PASSWORD` 等）強制英數直出。
  - 處理 `EditorInfo.imeOptions` 的 action（搜尋／送出）。
- 候選列：每個候選是獨立的標準 TextView（`contentDescription` 為 `cand:<索引>:<字>`），可點選、長按，也可被 UiAutomator 定位；候選列置於可橫向捲動的容器內。
- 實體鍵盤：`onShowInputRequested` 一律接受（預設實作在有實體鍵盤時會拒絕 App 的隱含顯示請求，導致候選列不出現），開始組字而畫面未顯示時呼叫 `requestShowSelf`。
- 組字區：只有畫面上確實有 liu-kai 的組字區時，組字清空才以 `setComposingText("")` 移除，避免誤刪使用者選取的文字；游標被移出組字區時結束組字。
- targetSdk 35 的 edge-to-edge：輸入畫面以導覽列 inset 補底部 padding。
- 鍵盤：自繪 View，字根鍵依字表字元集產生；另有基本數字符號層。
- 設定頁：
  - 啟用輸入法引導、匯入字表（SAF）、顯示匯入報告與目前字表 manifest、清除字表。
  - debug 版另提供「從 app 專屬外部目錄匯入」，供 adb 自動化。

## 8. 讀音資料

- `liu-kai-cli gen-readings --unihan Unihan_Readings.txt --out core/src/main/resources/readings.tsv`：取 `kMandarin`（有兩個值時第二個為台灣讀音，列為主要讀音），拼音轉注音（`core` 的 `PinyinZhuyin`）後輸出。目前資料為 Unihan 18.0.0，共 44,353 字。
- 同音字索引在執行期建立：以字表中存在的繁中單字為範圍，按主要讀音分組。
- 已知限制：`kMandarin` 只收主要讀音，多音字不完整；需要時改用其他授權相容的資料源。

## 9. 測試策略

| 層級 | 內容 | 執行 |
|---|---|---|
| core 單元測試 | 引擎狀態機、VRSF 衝突、萬用字元、翻頁、讀音、假名、解析與切分（合成字表） | `./gradlew test`（CI） |
| cli golden | 合成的 IBus／LIME 檔 → TSV／manifest 逐位元比對 | `./gradlew test`（CI） |
| 真實字表驗證 | `cli` 對正版檔輸出統計並與第 5 節基準比對 | 本機手動，產物不入 git |
| 模擬器 E2E | testhost + UiAutomator：實體鍵盤打字、VRSF、點候選、萬用字元、同音、日文、密碼欄 | `scripts/emulator-e2e.sh`（本機 Windows 模擬器 AVD `LiuKai35`） |

模擬器限制：沒有 LINE／Messenger／Gmail 帳號，App 矩陣只能在模擬器上測 Chrome 與 testhost，其他 App 留待實機驗收。

## 10. 里程碑

| # | 內容 | 完成條件 |
|---|---|---|
| M0 ✅ | repo 骨架、conventions 1.0.17、本計畫 | policy_check 0 FAIL，PR merge |
| M1 ✅ | Gradle 多模組骨架、Linux SDK、CI | `./gradlew test assembleDebug` 本機與 CI 皆綠 |
| M2 ✅ | core：解析、切分、編譯格式、引擎（第 6 節 #1–#12） | 單元測試涵蓋每條規則，全綠 |
| M3 ✅ | core：萬用字元、讀音／同音、日文模式（#13–#15） | 單元測試全綠；讀音資料可由 `liu-kai-cli gen-readings` 重現 |
| M4 ✅ | app：IME、候選列、鍵盤、設定匯入；testhost | 模擬器上以合成字表可打字、點選上屏 |
| M5 ✅ | 模擬器 E2E 自動化 | `scripts/emulator-e2e.sh` 全綠（15 項） |
| M6 | 真實字表驗收 | 正版表於模擬器匯入成功；統計符合基準；E2E 以真實字表抽測通過；需實測項目交使用者確認 |

## 11. 風險與對策

| 風險 | 對策 |
|---|---|
| 真實字表格式與 ChatGPT 描述不符 | 解析器先對真實檔做統計與逐筆比對，再固定規則；不符時以真實檔為準修正計畫 |
| VRSF／萬用字元／同音與官方行為不同 | 行為規格表標信心與需實測項，實測後更新；規則集中在 core，改動成本低 |
| 字表外流 | `.gitignore`、CLAUDE.md 規範、R-21 掃描；轉換產物只寫 repo 外或 app 私有目錄 |
| 實體鍵盤事件差異（各 App、各鍵盤） | 模擬器以 `hw.keyboard=yes` 測；實機再補 |
