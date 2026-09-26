# ChatGPT 設計討論紀錄（2026-09-26）

> 本檔保存 liu-kai 設計討論中 ChatGPT（GPT-5.6 Sol，Max 推理）的兩則實質回覆原文，作為 `docs/plan.md` 的來源依據。回覆屬外部建議，不是事實來源；其中的數字與行為描述以本機實測為準。
>
> 先前脈絡（2026-09-14～15，同一 session）：獨立 IME、不修改官方 APK；母表採「LIU FOR LINUX」附件 `liu_ibus_final.txt` 的繁中區段；只發佈引擎與轉換器。

## 我方送出的草案（R1，摘要）

底座 A（fork OhMyBias）／B（clean-room Kotlin IME）／C（Trime + Rime）／D（fcitx5-android table）；字表管線；引擎行為；B 的模組設計；驗收；里程碑。另附本機查證：OhMyBias Android 無 LICENSE（macOS 版為 MIT）、Trime 為 GPL-3.0、fcitx5-android 為 LGPL-2.1 且支援自訂 table（PR #255）、rime-liur 以字典輔碼與 lua 處理 VRSF。完整 prompt 見本檔附錄 A。

## ChatGPT 回覆一（R1d）

> 1) 直接結論
> 
> 選 C：Trime＋自行產生的 Rime 方案。但 rime-liur 無 LICENSE，因此：
> 
> 個人 spike 可直接安裝驗證。
> 公開專案只能把它當黑箱行為參考；不可複製其 schema、Lua 或 openxiami 字典。
> 用 liu_ibus_final.txt 重新產生字典，clean-room 重寫 schema／Lua。
> 若 Trime 實體鍵盤不合格，先改用 fcitx5-android＋Rime plugin，不要立即改成 fcitx table。
> A 排除；B 是最終退路。
> 2) Q1–Q5
> Q1
> 
> 若 rime-liur 取得授權，可直接作基礎；目前則只能產生「相容格式」，不能衍生其程式碼。
> 
> VRSF 生成流程：
> 
> 每個標準碼建立去重且保持順位的候選陣列。
> 由 LIU7／rime-liur 黑箱實測建立版本化 selector_map；不要先假定 V/R/S/F 對應第幾順位。
> 產生 code^selector → 對應候選 的內部輔碼。
> 只有當 code+selector 依官方優先規則應解讀為選字、而非合法字根或可延伸碼時才輸出。
> 對「可用／應排除」案例做 golden test。
> 
> Trime 通常能執行 librime-lua 的 processor／translator／filter，但不能保證完整相容。可能失效：
> 
> librime-lua API／Candidate 類型版本不同。
> Lua 依賴桌面路徑、外部命令或額外 native module。
> 軟鍵盤沒有送出腳本預期的實體 KeyEvent。
> 候選註解、preedit、翻頁與實體鍵盤 UI 行為不同。
> 
> 基本四碼、排序、簡碼可用標準 Rime；VRSF、萬用字元、官方式同音查詢才需要 clean-room Lua 或預編譯輔助字典。
> 
> Q2
> 
> **確定支援。**PR #255／TableManager.kt 已提供 .conf/.conf.in＋.txt/.dict 匯入。但「能匯入」不等於能表達完整無蝦米行為；VRSF、萬用及同音可能仍需 addon。
> 
> Q3
> 
> 上一輪以 lime_liu7.txt 作區段標記：
> 
> 第一個 %gen_inp 前的連續字碼資料為繁中。
> 後續以 %cname 開新區段。
> 跳過 %keyname begin…end，只收 %chardef begin 後資料。
> 四段串接後，與 IBus BEGIN_TABLE…END_TABLE 的 (code,text) 串流逐筆相等。
> 
> 所以繁中是對應第一段的 IBus 前綴，共 28,816 筆。只靠 IBus 檔本身無可靠標記；應保存區段雜湊與首尾錨點，或驗證 liu70_zh_tw.cin 等同該段。
> 
> Q4 行為規格
> 標準碼、簡碼、容錯碼及候選順位：高
> 字本體最多四碼：高
> 空白首選、點擊候選上屏：高
> VRSF 功能存在：高；衝突優先序：低
> [／] 萬用語意：中
> ' 同音與讀音查詢流程：中
> 四碼／唯一候選自動上屏：低
> 翻頁、Enter、Esc、Backspace 精確行為：低
> 中英、全半形、標點狀態：中
> 自訂詞／調頻是否改變固定順位：低
> Q5
> 
> 缺少：
> 
> LIU7 黑箱行為案例與差異測試。
> Lua API 最低版本及降級策略。
> 密碼欄禁止學習、EditorAction、旋轉／程序重啟狀態。
> 「使用上游 Trime」與「自行發布 APK」的授權／權限界線。
> 
> 過度設計：MVP 手機端轉換、SAF、自製二進位及完整 B 架構。另 allowBackup=false、無 INTERNET 權限只適用自製 APK，無法約束上游 Trime。
> 
> 前三大風險：
> 
> rime-liur／openxiami 無授權，不能作公開基底。
> VRSF 衝突規則與 Lua 跨平台差異。
> 實體鍵盤／Termux 等 App 可能繞過 IME。
> 3) 修正後里程碑
> M0：LIU7＋rime-liur 黑箱行為表、授權閘門。
> M1：官方表轉中性 TSV／Rime 字典及區段驗證。
> M2：clean-room 基本 schema，Weasel／Trime 共測。
> M3：VRSF、萬用、同音 Lua 與 differential tests。
> M4：Android 軟／實體鍵盤矩陣；必要時換 fcitx5＋Rime。
> M5：兩週 dogfood，之後才決定是否走 B。
> 4) 不確定處
> 
> VRSF 的完整順位映射、兩三碼加選字鍵的衝突規則，以及 rime-liur Lua 使用的 API 是否全受目前 Trime 支援，均需實測。

## 我方挑戰（R2）

範圍過重（提出自用 P／公開 R 兩路線）、VRSF 對應與衝突規則、何時自製 APK 的觸發條件；要求輸出最終版 plan。完整 prompt 見附錄 B。

## ChatGPT 回覆二（R2，最終版 plan）

> 1) 目標與範圍
> 路線 P：個人自用，現在執行
> 安裝上游 Trime，不製作 APK。
> 本機使用 rime-liur 的 schema／Lua，但不公開、不分享。
> 主字典取代而非混合為正版表繁中區段；同步重建「字→碼」反查檔，避免 VRSF 仍依賴 openxiami 資料。
> 手機即為行為 oracle；實測結果記成簡短測試表即可，不拆 LIU7 RAR。
> 
> 無 LICENSE 代表沒有明確授予改作、散布權。本機下載、執行及修改且完全不散布，實務暴露較低，但不是取得授權、風險也非零。邊界是：不 push、不分享設定包／APK／字典、不把其 Lua 或資料帶入路線 R。這不是法律意見。
> 
> 路線 R：未來可能公開
> 
> 只有決定公開才啟動。先做授權與來源盤點，再依 P 的黑箱測試結果 clean-room 重寫。公開物限自行開發的轉換器、schema／Lua 與假資料，不附官方表、openxiami 或 rime-liur 程式碼。
> 
> 2) 架構決策與理由
> 
> 主線採 Trime＋Rime：最快解決問題，也能與 Weasel 共用方案。兩個 Android 前端都無 INTERNET 權限。
> 
> 備援順序：
> 
> Trime＋Rime
> fcitx5-android＋官方 Rime plugin
> 自製 Kotlin IME（B）
> 
> 不先採 fcitx table，因它會增加第二套格式，而且 VRSF、萬用、同音行為仍可能需要 addon。
> 
> 3) 字表管線
> 
> liu_ibus_final.txt → 繁中原始列 → 去重 → TSV → Rime 主字典＋字→碼反查檔
> 
> 切分規則：
> 
> 解析 lime_liu7.txt；第一個 %gen_inp 前的資料為繁中。
> 跳過 %keyname begin…end，其餘區段依 %cname／%chardef begin 解析。
> 四段 (code,text) 串流須與 IBus BEGIN_TABLE…END_TABLE 逐筆相同。
> 將第一段對應回 IBus 頻率欄。
> 
> 驗證基準：原始 28,816 列、去重後 28,755 組、21,199 個碼、17,792 個輸出字；保留首次順位／最高頻率並產生 SHA-256 manifest。
> 
> VRSF：每碼建立固定候選序列，位置 2／3／4／5 對應 V／R／S／F（高信心）。為目標字生成 字 → code^selector。順位以靜態母表為準，不隨學習調頻；自訂詞另設 translator。
> 
> 選字判定修正為：
> 
> 若 code+鍵 是任一正常碼的前綴（完整碼也算），當字根。
> 否則，僅當 code 是完整碼且存在該 VRSF 反查項目時選字。
> 否則不應從「部分符合候選」誤選；實際清碼／忽略方式待實測。
> 
> 你的原規則缺少第 2 項，反例是：目前輸入並非完整碼、加鍵也無後續碼時，不應選取前綴候選。
> 
> 4) 行為規格表
> 行為	信心	需實測
> 一至四碼、簡碼、固定候選序	高	抽測
> 空白上屏首選、點擊候選	高	是
> V/R/S/F＝第 2/3/4/5 候選	高	是
> VRSF 與正常字碼的優先規則	中	必測
> [ 任意數量、] 單一字根	中	必測
> ' 同音字、讀音查詢	中	必測
> 四碼／唯一候選自動上屏	低	必測
> Enter、Esc、Backspace、翻頁	低	必測
> 中英、全半形、標點	中	是
> 自訂詞與固定順位隔離	中	是
> 5) 測試矩陣
> App	軟鍵盤	藍牙／實體鍵盤
> Chrome	組字、點選、旋轉	直打、VRSF、候選列
> Gmail	多行、Enter	組字、退格、送出鍵
> LINE／Messenger	點選與連續輸入	候選列、切換中英
> Termux	組字／commit	診斷項；宿主繞過 IME 不算單獨切 B
> 密碼欄	英數直出、不學習	同左
> 
> 每格至少測：首選、第二候選、四碼、萬用、同音、退格及切 App 後狀態。
> 
> 6) 里程碑與完成條件
> P0 基線：原版 rime-liur 在 Trime 可部署，普通輸入及三項特殊功能各成功一次。
> P1 轉換：產出主字典與反查檔；統計、順序、hash 全通過。
> P2 整合：完全換成正版表後，Chrome 軟鍵盤、候選點擊、VRSF 通過。
> P3 相容性：完成矩陣；失敗均能分類為設定、Lua、前端或宿主問題。
> P4 Dogfood：連續兩週無資料遺失、卡死或高頻輸入阻斷。
> R0 公開閘門：只有決定公開才建立獨立 repo、來源清單與 clean-room 測試規格。
> 7) C → B 觸發條件
> 
> 以下任一問題在 Trime 與 fcitx5＋Rime 都存在，且無法由設定／Lua 修正，才切 B：
> 
> 兩個以上一般 App 無法點擊候選或正確 commit。
> 兩個以上一般 App 的實體鍵盤無法組字、顯示候選或選字；Termux 單獨失敗不算。
> VRSF、萬用或同音因 librime-lua/API 限制無法實現。
> 前端狀態／隱私需求必須取得 InputMethodService 控制權。
> 
> B 原模組已足夠；但 :engine 必須是明確狀態機，:ime 要補 onKeyDown/onKeyUp、重複鍵／修飾鍵、EditorAction、候選列隱藏鍵盤模式；測試需增加 instrumentation，無須先加新模組。
> 
> 8) 前三大風險與對策
> P 的無授權內容流入 R：P/R 分目錄，R 僅接收黑箱測試案例與自行撰寫程式。
> 換字典後 VRSF／順位漂移：主字典與反查檔必須由同一 TSV 同次生成，固定單字順位。
> Android App／實體鍵盤差異：先測兩個 Rime 前端；區分 IME 缺陷與宿主繞過，避免為 Termux 誤啟動 B。

## 後續裁決

2026-09-26 使用者裁決改走路線 B（自製 Kotlin IME），MVP 加入萬用字元、同音字／讀音查詢、日文平假名／片假名。見 `docs/plan.md`。

## 附錄 A：R1 prompt

```text
【APK plan R1】接續上面的結論（獨立 IME、不改官方 APK、母表用 liu_ibus_final.txt 繁中區段、只發佈引擎與轉換器），我請本機的 Claude Code 一起整理了一版 Android APK 設計 plan 草案。請你以挑戰者角度審查，不必再搜尋 Gmail / Drive。

=== 草案 ===
0. 範圍
- MVP = 個人自用 sideload APK，不上架；主要痛點是官方 PRO「點候選字無法選字」。
- 必須支援：軟鍵盤 + 藍牙/外接實體鍵盤直打。

1. 底座（最關鍵的決策）
- A. fork OhMyBias：功能最接近，但 repo 沒有 LICENSE，連私人 fork 後長期維護都卡住。
- B. clean-room 自寫 Kotlin IME（InputMethodService + 自己的候選列）。
- C. Trime（Rime 的 Android 前端，GPL-3.0）+ 自製無蝦米 Rime schema／字典。上一輪 Windows 端已建議走 Weasel/Rime，若 Android 也用 Rime，同一份 .dict.yaml + schema 可兩平台共用。
- D. fcitx5-android（LGPL）+ table 輸入法匯入自訂字表。
- 目前傾向：先用 1–2 天 spike C 或 D（「設定」優先於「開發」），確認無蝦米行為能否表達；做不到才走 B。

2. 字表管線
- liu_ibus_final.txt → 取繁中區段 → 正規化（字碼字元集、去重、保留候選順序／頻率）→ 中性 TSV（code, text, rank）→ 各平台編譯（Rime .dict.yaml／fcitx5 table／自製二進位）。
- 區段切分要有可重現、可驗證的規則，不靠行號。
- 字表只在本機或手機內轉換；手機端用 SAF 讓使用者選檔匯入，放 app 私有目錄，allowBackup=false。repo／CI 只放合成的假字表。

3. 引擎行為（需要一份規格）
- 最多 4 碼、空白上屏首選、字碼後加 v 選第二候選等選字規則、候選翻頁、英數切換、全半形、標點、萬用字元、同音字查詢。

4. 若走 B 的架構
- 模組：:engine（純 Kotlin/JVM，可單元測試）、:table（轉換器與編譯格式）、:ime（Service、鍵盤、候選列）、:settings。
- 組字用 setComposingText，上屏用 commitText，並處理不支援 composing 的 App；實體鍵盤在 service 的 onKeyDown 處理，軟鍵盤隱藏時候選列仍要顯示。
- 繁中約 2.9 萬筆，記憶體內排序陣列加二分搜尋就夠，萬用字元用前綴範圍查詢；先不做 mmap／trie。
- 不宣告 INTERNET 權限、不做任何遙測。

5. 驗收
- 候選字點擊在 Chrome、Gmail、LINE、Messenger、Termux、密碼欄、橫向模式、實體鍵盤都能正確上屏（針對官方 PRO 的 bug 做回歸測試）。
- 轉換器有 golden test（用合成字表）；引擎有單元測試。

6. 里程碑
- M0 spike C/D → M1 轉換器與中性格式 → M2 引擎＋最小 IME（若走 B）→ M3 實體鍵盤、設定、自訂詞 → M4 自用兩週 dogfood。

=== 請回答 ===
Q1. A/B/C/D 你選哪個？理由是什麼？C 或 D 能否表達無蝦米特有行為（例如 v 選第二字、4 碼、萬用字元、同音字查詢）？需要 Rime Lua 或 fcitx5 addon 的地方請點名。
Q2. fcitx5-android 目前是否支援使用者匯入自訂 table 字表？不確定就明說不確定。
Q3. 你上一輪如何判定 liu_ibus_final.txt 的四個區段邊界？請給一條可重現的切分規則。
Q4. 列出無蝦米必須對齊的標準行為規格，每條標示信心（高／中／低），低信心的我會自己實測確認。
Q5. 這份 plan 缺了什麼、哪裡過度設計？前三大風險是什麼？

輸出格式：1) 直接結論 2) Q1–Q5 逐題回答 3) 修正後的里程碑 4) 不確定處。請精簡，控制在 1500 字內。
```

## 附錄 B：R2 prompt

```text
【APK plan R2】同意選 C，也同意 A 排除、B 當退路。但有幾點要挑戰，然後請收斂成最終版 plan。同樣不要上網搜尋、不要使用工具。

補充我本機查證的事實：
- Trime 和 fcitx5-android 的 AndroidManifest 都沒有宣告 INTERNET 權限。
- fcitx5-android 有官方的 rime plugin，你提的「fcitx5 + Rime」備援可行。
- rime-liur 的 VRSF 資料放在另一份「字 → 碼」反查資料檔，不在主字典裡。

挑戰 1：範圍過重
MVP 是「個人自用」，你的 M0 卻一開始就要做 clean-room 重寫加 LIU7 黑箱行為表，對自用來說太重。我提議分兩條路線：
- 路線 P（自用，現在做）：手機裝上游 Trime，在本機直接使用 rime-liur（不散布、不公開），只把字典換成或補上我的正版表繁中區段 28,816 筆。這台手機同時就是「行為 oracle」，不必另外去拆 LIU7 的加密 RAR。
- 路線 R（公開，之後才做，可能不做）：clean-room schema／Lua，只發佈轉換器。只有決定公開時才啟動，並先過授權閘門。
請問你同意嗎？自用情境下，在本機修改並使用沒有 LICENSE 的 repo，風險邊界在哪裡？

挑戰 2：VRSF 對應
我的認知是字碼後加 v／r／s／f 分別選第 2／3／4／5 個候選。請標示你對這點的信心。
衝突規則我提議：若「字碼 + 該鍵」本身是字表中的合法碼，或是更長合法碼的前綴，就當字根處理；否則當選字。請確認，或給出反例。我會在手機上實測。

挑戰 3：什麼時候才自己做 APK
走路線 P 就沒有自製 APK。請定義從 C 切到 B 的明確觸發條件，例如：Trime 也重現「點候選無法上屏」、實體鍵盤問題無法靠設定解決、VRSF／萬用字元的 Lua 在 Trime 上跑不起來。若真的觸發 B，請用幾行檢查 R1 草案第 4 節的模組設計是否足夠。

最後請輸出最終版 plan，固定結構如下：
1) 目標與範圍（P／R 兩路線）
2) 架構決策與理由
3) 字表管線（含區段切分與驗證）
4) 行為規格表（每條附信心，並標出需要我實測的項目）
5) 測試矩陣（App × 軟鍵盤／實體鍵盤）
6) 里程碑，每個都要有完成條件
7) C → B 觸發條件
8) 前三大風險與對策
2000 字內。
```
