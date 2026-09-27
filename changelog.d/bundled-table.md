---
type: feat
---
內建使用者自建字表：本機建置時以 `liu-kai-cli bundle` 將 `liu_ibus_final.txt`＋`lime_liu7.txt` 編譯為 `assets/bundled.liutable` 內建進 APK，第一次使用自動套用（裝好就能打中文）；設定頁顯示「字表：內建／已匯入／尚未匯入」，清除匯入的字表後改回內建；沒有字表的建置（CI）不內建。
