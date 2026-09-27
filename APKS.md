# test-apks — 測試 APK 專用分支

放 liu-kai（Android 嘸蝦米輸入法）的 debug 測試 APK，方便手機側載。
**此分支刻意不進 CI、不做 policy check、不開 PR**（與 main 的開發流程分離）；測試 APK 不建立 GitHub Releases。

- 最新：`liu-kai-debug-2026-09-27.apk`（**內建字表，裝好即可打中文，不需匯入**；螢幕鍵盤照官方嘸蝦米配置：字母層 q–p 長按輸入數字、?123 層、ALT 層、長按「.」彈出標點、長按「,」彈出設定；其餘同前版：候選列一定出現、0–9 選字、v/r/s/f、`*` 萬用字元（?123 層）、``` ` ``` 同音字（ALT 層）、假名直接打「字碼＋,／.」、空碼按空白出空白、組字失敗不出字並以紅框提示）
- 手機直接下載（raw）：
  <https://github.com/hamanpaul/liu-kai/raw/test-apks/liu-kai-debug-2026-09-27.apk>
- SHA-256：`5d81f17e2d5d849577997be36e13f74167a50fa74c06cfa909be0151b7887a3f`
- Source commit：`c9c6548e245ed24f166c8b5794143bc888084091`（`feature/testpilot-coverage`；模擬器端對端 60 個案例全數通過、app 覆蓋率行＋分支 100%）
- 需求：Android 11（API 30）以上。
- 重建：`./gradlew :app:assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk`

## 安裝與設定

1. 下載後安裝（需允許瀏覽器或檔案管理員「安裝不明應用程式」）。
2. 開啟 liu-kai 設定頁：按「1. 在系統設定啟用 liu-kai」，再按「2. 切換輸入法」。
3. 字表已內建（使用者自建字表），第一次使用自動套用，設定頁顯示「字表：內建」。要改用其他字表時，把 `liu_ibus_final.txt` 與 `lime_liu7.txt` 放到手機「下載」資料夾，在設定頁按「3. 匯入字表」一次選取兩個檔案；「清除字表」會改回內建字表。
4. 這是 debug 版：內含 JaCoCo 覆蓋率 instrumentation 與供 adb 使用的除錯匯入元件，僅供測試。

## 歷史

| 日期 | 檔案 | source commit | 內容 |
|---|---|---|---|
| 2026-09-27 | `liu-kai-debug-2026-09-27.apk` | `c9c6548` | SHA-256 `5d81f17e2d5d8495…`；內建字表、螢幕鍵盤照官方配置、長按數字與彈出鍵、修正鍵盤跳動 |
| 2026-09-26（第 2 版） | `liu-kai-debug-2026-09-26.apk` | `9cee47d` | SHA-256 `feb06d7bcd744c11…`；超出候選數／超過四碼為組字失敗、放行字元改以上屏 |
| 2026-09-26（第 1 版，已被覆蓋） | `liu-kai-debug-2026-09-26.apk` | `d42d9bd` | SHA-256 `5ca8bcadf9ee692a…`；首次發佈：v0.1 MVP＋官方行為對齊 |
