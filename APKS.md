# test-apks — 測試 APK 專用分支

放 liu-kai（Android 嘸蝦米輸入法）的 debug 測試 APK，方便手機側載。
**此分支刻意不進 CI、不做 policy check、不開 PR**（與 main 的開發流程分離）；測試 APK 不建立 GitHub Releases。

- 最新：`liu-kai-debug-2026-10-18.apk`（**觸控漏字診斷版，尚未完全修復**；內建使用者字表）
- [手機下載 APK](https://github.com/hamanpaul/liu-kai/raw/test-apks/liu-kai-debug-2026-10-18.apk)
- SHA-256：`cb2218c6c007771314c88465a056337c8b4c3cf62e94370131786e76f74df824`
- Source commit：[`785eeb11d94f9a07b3d1cf9ac7eb917d5f2f273b`](https://github.com/hamanpaul/liu-kai/commit/785eeb11d94f9a07b3d1cf9ac7eb917d5f2f273b)（`feature/input-touch-diagnosis`，未合併）
- 已修正：QAZ／PLM 螢幕邊緣與按鍵間隙命中死區；普通空白鍵超過排隊中字母 click 的順序問題。
- **仍未修復**：快速中文重疊輸入時，Android 延遲 `UnsetPressedState` 可清除下一次按壓而漏字；本版加入取證 log，不宣稱完全解決。
- 驗證：core／CLI checks PASS；TestPilot 179 項 PASS、行／分支 100%；canonical preflight PASS。完整 emulator 108 項為 **107 PASS／1 FAIL**（語音鍵 Next／Done 標籤差異），app 行 99.9%、分支 98.2%，**未達 100% 門檻，尚不可合併**。單獨語音補測失敗於視窗未穩定，尚不能確認標籤差異原因。
- [Issue #6](https://github.com/hamanpaul/liu-kai/issues/6)；[完整診斷與證據](https://github.com/hamanpaul/liu-kai/blob/785eeb11d94f9a07b3d1cf9ac7eb917d5f2f273b/docs/input-touch-diagnosis.md)。實體手機觸控面板尚未驗證。
- 需求：Android 11（API 30）以上。Debug log 不含組字／候選文字；可執行 `adb logcat -v threadtime -s LiuKaiTouch:D > liu-kai-touch.log` 收集，重現後按 Ctrl-C。
- 重建：`./gradlew :app:assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk`

## 安裝與設定

1. 下載後安裝（需允許瀏覽器或檔案管理員「安裝不明應用程式」）。
2. 開啟 liu-kai 設定頁：按「1. 在系統設定啟用 liu-kai」，再按「2. 切換輸入法」。
3. 設定頁的「嘸蝦米鍵盤設定」照官方非 PRO 的項目（語音輸入、鍵盤高度等），「其他設定」可選主題；「加字加詞」可自訂拆碼與字詞。語音輸入需先在系統設定啟用 Google 語音輸入。
4. 字表已內建（使用者自建字表），第一次使用自動套用，設定頁顯示「字表：內建」。要改用其他字表時，把 `liu_ibus_final.txt` 與 `lime_liu7.txt` 放到手機「下載」資料夾，在設定頁按「3. 匯入字表」一次選取兩個檔案；「清除字表」會改回內建字表。
5. 這是 debug 版：內含 JaCoCo 覆蓋率 instrumentation 與供 adb 使用的除錯匯入元件，僅供測試。

## 歷史

| 日期 | 檔案 | source commit | 內容 |
|---|---|---|---|
| 2026-10-18 | `liu-kai-debug-2026-10-18.apk` | `785eeb1` | 觸控死區與空白鍵順序修正、debug 取證；仍有未解中文快打 race；全套 107/108，app coverage 未達門檻 |
| 2026-09-28 | `liu-kai-debug-2026-09-28.apk` | `0308aa4` | SHA-256 `bc964000f0903613…`；App 圖示改用「嘸＋改」logo、修正快打連鍵漏字、合入 PR #2 review 修正（取代同日 `b2aae76` 版） |
| 2026-09-28 | `liu-kai-debug-2026-09-28.apk` | `b2aae76` | SHA-256 `2afd9cffc5c02660…`；修正鍵盤較高時設定頁試打區被蓋住（取代同日 `ab88901` 版） |
| 2026-09-28 | `liu-kai-debug-2026-09-28.apk` | `ab88901` | SHA-256 `6e4b507b2e6762f7…`；改照官方非 PRO 2.6.8：大寫字碼、空白鍵滑動切換語言與底線、🎤 鍵與語音輸入設定、橘色候選列、智慧鍵盤留白、設定頁、橫式白色編輯區；PRO 主題與閒置標點改選用，移除數字列與空白滑動移游標（取代同日 `4da5df3` 版） |
| 2026-09-28 | `liu-kai-debug-2026-09-28.apk` | `4da5df3` | SHA-256 `d75e6a0d1d5d51d1…`；照官方 PRO 補齊：語音鍵、中文大寫鍵帽、同音鍵與嘸无台日語言模式、Shift 三態、重音字母、按鍵預覽、空白滑動、自動返回、智慧鍵盤、十種主題與五段高度字體、加字加詞 |
| 2026-09-27（第 2 版） | `liu-kai-debug-2026-09-27.apk` | `a467aef` | SHA-256 `e95e93f1d64ee037…`；外觀改為官方「經典灰＋按鍵高」（灰色按鍵獨立分格、按鍵加大）、修正 BACK 收起後立即打字組字被定案成字母 |
| 2026-09-27（第 1 版，已被覆蓋） | `liu-kai-debug-2026-09-27.apk` | `c9c6548` | SHA-256 `5d81f17e2d5d8495…`；內建字表、螢幕鍵盤照官方配置、長按數字與彈出鍵、修正鍵盤跳動 |
| 2026-09-26（第 2 版） | `liu-kai-debug-2026-09-26.apk` | `9cee47d` | SHA-256 `feb06d7bcd744c11…`；超出候選數／超過四碼為組字失敗、放行字元改以上屏 |
| 2026-09-26（第 1 版，已被覆蓋） | `liu-kai-debug-2026-09-26.apk` | `d42d9bd` | SHA-256 `5ca8bcadf9ee692a…`；首次發佈：v0.1 MVP＋官方行為對齊 |
