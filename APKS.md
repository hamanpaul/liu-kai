# test-apks — 測試 APK 專用分支

放 liu-kai（Android 嘸蝦米輸入法）的 debug 測試 APK，方便手機側載。
**此分支刻意不進 CI、不做 policy check、不開 PR**（與 main 的開發流程分離）；測試 APK 不建立 GitHub Releases。

- 最新：`liu-kai-debug-2026-09-26.apk`（TestPilot 全覆蓋＋依使用者確認對齊官方行為：候選列一定出現、0–9 選字、v/r/s/f、`*` 萬用字元、``` ` ``` 同音字、假名直接打「字碼＋,／.」、空碼按空白出空白、組字失敗不出字並以紅框提示）
- 手機直接下載（raw）：
  <https://github.com/hamanpaul/liu-kai/raw/test-apks/liu-kai-debug-2026-09-26.apk>
- SHA-256：`5ca8bcadf9ee692a87877c423a9f8c0e144b16d320c465e60a987201be265134`
- Source commit：`d42d9bdafaafcebc34b43ef8e0223a740a6d1efd`（`feature/testpilot-coverage`；模擬器端對端 54 個案例通過、app 覆蓋率行＋分支 100%）
- 需求：Android 11（API 30）以上。
- 重建：`./gradlew :app:assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk`

## 安裝與設定

1. 下載後安裝（需允許瀏覽器或檔案管理員「安裝不明應用程式」）。
2. 開啟 liu-kai 設定頁：按「1. 在系統設定啟用 liu-kai」，再按「2. 切換輸入法」。
3. 字表不內建：把自建字表 `liu_ibus_final.txt` 與 `lime_liu7.txt` 放到手機「下載」資料夾，在設定頁按「3. 匯入字表」一次選取兩個檔案。
4. 這是 debug 版：內含 JaCoCo 覆蓋率 instrumentation 與供 adb 使用的除錯匯入元件，僅供測試。

## 歷史

| 日期 | 檔案 | source commit | 內容 |
|---|---|---|---|
| 2026-09-26 | `liu-kai-debug-2026-09-26.apk` | `d42d9bd` | 首次發佈：v0.1 MVP＋官方行為對齊（見上） |
