# 螢幕鍵盤觸控診斷

## 結論與範圍

調查確認兩個已修正原因，並保留一個尚未修正的快速中文重疊觸控 race。固定螢幕邊緣與按鍵間隙原本會送達 IME window，但沒有命中原按鍵 View；將比例槽內事件副本轉送至原按鍵 View 後，相同座標可輸入預期字元，視覺按鍵和無障礙 bounds 不變。另一個獨立 race 是普通空白鍵在 ACTION_UP 同步執行 `onKey`，可能超過較早放開但仍排隊的字母 `performClick`；現在普通空白鍵 click 也排入 View queue，保留 ACTION_UP 順序。第三個 race 的 trace 顯示 Android `UnsetPressedState.run` 在另一個指標仍按住時清除字母鍵 pressed 狀態；嘗試的 production guard 無法安全判斷 IME window focus，已撤回，未宣稱這項已修正。

英文控制與 252 字元壓力案例在先前 13-case run 中完整輸入，但這不能證明快速中文組字可靠。嚴格真實字表中文控制的 40 ms／50 ms hold 組在 25 字讀回中，索引 2 的預期「中」變成「後」。80 ms／100 ms hold 組的 50 字讀回是累積欄位：前 25 字沿用同一個 40 ms 組的錯字，後 25 字全部是預期「中」，因此這些報告沒有顯示 80 ms 組獨立漏字。Trace 在活動指標期間記錄了框架延後的 pressed reset。Guest `MotionEvent.eventTime` 間隔與 host 注入間隔分開記錄。先前英文壓力組 guest down 間隔為 37–40 ms（40 ms 組）及 74–80 ms（80 ms 組）；`dispatchMs` 的最大值為 2.441 ms。最終 full-suite 結果以根代理在最新 APK 上的執行為準；不得以平均 dispatch 時間推論此 race 已消失。`dispatchMs` 含 debug 幾何整理和 log 成本，不能視為未受記錄影響的純觸控延遲。

此模擬器測試驗證 Android IME View、事件生命週期與 emulator 注入路徑；它不驗證實體觸控面板、廠商韌體、邊緣防誤觸或手掌拒絕策略。IME logger 只能觀察已送達 `ImeView.dispatchTouchEvent` 的事件。

## 命中區 RED / GREEN 證據

- 修正前固定點 `(2,1681)`、`(104,1681)`、`(112,1681)`、`(1077,1681)` 均位於 IME window `[0,1467,1080,2400]` 內。原視覺按鍵 bounds 分別為 Q `[6,1596,102,1767]`、W `[114,1596,210,1767]`、P `[978,1596,1074,1767]`。這四點分別落在 Q 左側螢幕邊界、Q/W 內側間隙兩側、P 右側螢幕邊界；事件記錄為 `handled=false` 且沒有 UP。四點後欄位仍空白。這是修正前的實際 emulator RED，不是模擬的測試結果。
- 修正後以相同四個座標得到 `qqwp`。另外 Q 上方 `(54,1592)` 輸入 Q、E 下方 `(300,1769)` 輸入 E；候選列外 `(500,1550)` 不輸入字。視覺 key bounds 沒有改變。完整 before/after raw 座標、slot/key bounds 和代表 trace 在 [input-touch-baseline.json](input-touch-baseline.json)。
- `soft-touch-gap-and-side-margins` 與 `soft-touch-qazplm-edges` 通過；中央鍵和 QAZ/PLM 外側鍵都能輸入，槽內起始的空白鍵滑動、取消和長按案例也通過。

## 快速中文輸入 RED / GREEN 證據

- `soft-chinese-fast-order-control` 在 space callback 修正前為 RED（報告 `20261018-015423`）：實際 click 順序中，第二、三個及後續重複區塊的 space/callback 次序顛倒；改成 `v.post { onKey(def) }` 後，報告 `20261018-015935` 的合成字表序列通過，空白鍵手勢案例也通過。這項修正只統一普通 click 的 UP 後佇列順序，沒有改成按 DOWN 順序提交；嚴格 120 ms 反向 UP 注入仍留在診斷探針外，不改寫其順序語意。
- 使用者真實字表 `ci + SPACE → 中` 的正式控制保留 25 字／40 ms 與 50 字／80 ms 的精確輸出預期。報告 `20261018-021247` 是具體 RED：`read-40` 的 25 字在零起算索引 2 出現「後」而非「中」；`read-80` 是累積的 50 字，其前 25 字與 `read-40` 相同，後 25 字全部正確為「中」。因此此報告證明 40 ms 組的漏字，不證明 80 ms 組另有一次漏字。對應 logger 行為：`phase=key_pressed key=c from=true to=false action=ACTION_MOVE eventTimeMs=6273216 pointerIds=[1] source=android.view.View$UnsetPressedState.run uptimeMs=6273236`。當時另一指仍在鍵盤上；Android 15 `android-15.0.0_r1` 的 [`UnsetPressedState.run`](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-15.0.0_r1/core/java/android/view/View.java) 會清除該 View 的 pressed 狀態。這不是以 host command 間隔推算的漏字。
- 一次限定 guard 試驗的 debug APK SHA-256 為 `d9239e9671b950e373fd9f2a7f27cf09e9a756160051a48d1384004c2ccafe23`，報告 `20261018-022038` 也在 `read-40` 索引 2 讀到「後」；累積 `read-80` 的後 25 字全部正確。Trace 顯示 pressed reset 沒有被 guard 抑制；`dumpsys window` 同時顯示 IME window 設有 `NOT_FOCUSABLE`，目前 focus 屬於 testhost，故 `hasWindowFocus()` 不是此 IME view 可用的生命週期判準。為避免弱化 detach/focus/disable reset 保護，已撤回該 guard；正式 case 不要求 stack 私有類名，也不放寬字串預期。
- 舊 APK 的偶發通過（`20261018-020159`）不推翻 RED；多次真實字表執行結果不一致，所以此項仍是未解 race。新 debug trace 會記錄每個鍵的 `ACTION_DOWN/UP/CANCEL`、pointer/local coordinates、pressed 前後狀態與在活動觸控外發生的 pressed reset caller，且不記錄組字文字。APK 不會攔截作業系統在 IME window 外處理的觸控。

## 節奏與延遲觀察

`soft-touch-cadence-sweep` 以同一組中央與 QAZ/PLM 字串各測 40、80、120、160、241 ms，每個測試組單獨清空 log buffer。結果如下，範圍是該組相鄰 guest down event 的毫秒間隔：

| Host 請求間隔 | 實際 host 中央 / 外側 | Guest 中央 | Guest QAZ/PLM 外側 |
|---:|---:|---:|---:|
| 40 ms | 40 / 40 ms | 38–39 ms | 38–40 ms |
| 80 ms | 80 / 79–81 ms | 76–78 ms | 76–78 ms |
| 120 ms | 119–123 / 119–125 ms | 115–119 ms | 115–120 ms |
| 160 ms | 160 / 160 ms | 152–157 ms | 154–155 ms |
| 241 ms | 240–242 / 240–242 ms | 231–233 ms | 231–233 ms |

13-case focused run 的 cadence 欄位為 `eurjdyqpalzm` 重複五次，最終模式恢復為 CHINESE。Host command 執行時間和 lateness 都保存在 TestPilot 步驟資料；不能用要求的 interval 取代 guest event cadence。

新增的 `soft-touch-stress-40-80` 包含 6 字單指序列控制 `edcedc`、`qazqaz`，以及 4 組各 60 字的 40/80 ms 中央與 QAZ/PLM 雙指重疊序列；總欄位長度為 252 字。四組各觀察到 60 個 guest down event、59 個相鄰間隔及多指事件。Guest 範圍分別為中央 40 ms 37–39、外側 40 ms 37–40、中央 80 ms 74–80、外側 80 ms 75–78。各組最高 `eventAgeMs` 為 15、15、2、3 ms；最高 `dispatchMs` 為 0.513、0.522、2.441、0.509 ms。這些 outlier 保留在報告中，沒有從摘要中排除。

第一次 104-case 全套執行為 96 pass、8 fail。失敗包含 TestPilot adb 遠端寫入尚未完成、匯入 FAIL 後快取舊表、候選列幾何尚未穩定、手勢測試遺留 Simplified 語系，以及 cadence 預期／host 與 guest 間隔混淆。TestPilot 現在等待遠端檔案位元組一致、匯入失敗時 fail closed、候選幾何穩定後才點擊；手勢測試復原語系並斷言 Traditional，cadence 仍保留精確輸出與 guest event 檢查。

上述 harness 修正後，13-case emulator 執行曾全部通過，報告為 `testpilot/reports/20261018-012600/report.md`（本機 ignored artifact）。最新 debug APK `cb2218c6c007771314c88465a056337c8b4c3cf62e94370131786e76f74df824` 的正式 108-case suite（報告 `20261018-022507`）為 107 pass、1 fail；唯一失敗是 `soft-voice-key` 預期最後一列標籤 `Next`，實際為 `Done`。App 覆蓋門檻也未通過：LINE 99.9%（1 missed）、BRANCH 98.2%（10 missed）；debug `TouchDiagnostics` 的 pressed-reset caller 擷取分支未觸發，另有 null-safe 分支未覆蓋。完整驗收未達標，這不是 merge-ready 結果；沒有豁免或掩蓋覆蓋率缺口。較早原始 APK 的 `soft-rollover-typing` 曾出現尾端 `R` 而非「雨」；後續既有案例得到正確字串，但這不能代替新增真實字表控制中觀察到的間歇性 pressed-reset race。

## Debug 記錄與手機收集

`app/src/debug` 的 `LiuKaiTouch` logger 記錄 action/action index、event time、uptime、event age、pointer ID/count、local/raw 座標、IME window／keyboard／視覺 key bounds、槽轉送目標、處理結果，以及鍵盤 `DOWN/UP/CANCEL`、pressed 狀態和長按、空白鍵、背退、候選點擊及 rebuild 時間。活動觸控外的 pressed 改變會附 caller 名稱。記錄不包含組字欄文字或候選文字。最新 debug APK SHA-256 為 `cb2218c6c007771314c88465a056337c8b4c3cf62e94370131786e76f74df824`；release unsigned APK SHA-256 為 `fbdc5ef34a72c6229b46f02f4e9a703c962771a4e12ea7e53060abdc9373a382`。Release APK DEX 不含 `LiuKaiTouch`、`phase=key_pressed`、`dispatchMs=` 或 `UnsetPressedState` 字串；診斷 wrapper 維持 release inline no-op。

在已安裝 debug APK 的 Android 手機或模擬器上，可於重現前開始收集：

```sh
adb logcat -v threadtime -s LiuKaiTouch:D > liu-kai-touch.log
```

停止時按 Ctrl-C。也可用 `adb logcat -d -v threadtime -s LiuKaiTouch:D > liu-kai-touch.log` 擷取仍在 logcat buffer 的記錄。`visualHit` 只描述座標相對於內縮視覺鍵的位置；`slot_forward target=key:*` 和按鍵／手勢記錄描述實際事件處理目標。不要把 `dispatchMs` 解讀成沒有 instrumentation 成本的純 dispatch latency。

若事件被系統／視窗管理器攔截、落在 IME window 外，或沒有派送至 `ImeView`，此 logger 看不到它；這種情況需同時收集 Android 系統輸入派送或實體裝置工具記錄。`adb emu event send` 可以測試 emulator 對 IME View 的派送及多指生命週期，不能代表實體面板的採樣、韌體和邊緣防誤觸行為。

## 追蹤檔與重現命令

完整固定點證據和 APK 雜湊在 [input-touch-baseline.json](input-touch-baseline.json)。Focused runtime 命令：

```sh
LINUX_SDK="$HOME/Android/Sdk" AVD_NAME=LiuKaiTouchLinux EMU_PORT=5584 \
  scripts/emulator-e2e.sh --no-build \
  --case soft-chinese-fast-order-control --case real-chinese-fast-order-control \
  --case debug-import-errors --case real-basic --case real-wildcard-homophone \
  --case hw-wildcard --case soft-touch-cadence-sweep \
  --case soft-touch-qazplm-edges --case soft-touch-slot-gestures \
  --case soft-touch-stress-40-80 --case soft-smart-symbols-while-composing \
  --case soft-type-tap-space --case soft-wildcard-homophone \
  --case soft-rollover-typing --case soft-touch-gap-and-side-margins
```

## 根代理針對性補測

`soft-voice-key` 單獨補測報告 `20261018-025055` 仍為 FAIL，但失敗在第一個長按步驟：輸入法視窗位置尚未穩定，沒有執行到 `Next`／`Done` 斷言。因此本次補測不能證明全套中的標籤差異已穩定重現，也不推翻完整套件的失敗；此項原因尚未定位。補測覆蓋率只涵蓋單一案例，不取代完整 108-case 的 99.9%／98.2% 結果。

## 使用者發版決定（2026-10-18）

使用者回報本版實際試用順手，並明確要求先關閉 issue、發行目前版本，日後遇到問題再開單。因此 v0.1.1 沿用本文件已核對的同一份 APK；上述未修 race、107/108 的完整案例結果與覆蓋率缺口仍有效，本次接受例外發布，不表示測試已全綠或 race 已修復。APK 內部版號維持 0.1.0／1；發行版本與檔案雜湊見 RELEASES.md。
