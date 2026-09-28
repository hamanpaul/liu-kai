# 版本紀錄

liu-kai 正式版本 ↔ tag ↔ commit ↔ APK 的對照表。發版流程：release PR（`VERSION`、`CHANGELOG.md` 由 `changelog.d/` 彙整、本表）merge 後，在 merge commit 打 `vX.Y.Z` tag、建立 GitHub Release 並附上 APK，再回填本表的 commit SHA。

| 版本 | tag | commit | APK | 摘要 |
|------|-----|--------|-----|------|
| 0.1.0 | `v0.1.0` | `3c0b836de49a9aacfd75b8e56169d69e8be7a5f8` | `liu-kai-0.1.0.apk`（SHA-256 `59763afc31bfad6a24548f97cc4f9c2b275598751eeb94cabfd653adc3e4dc4f`） | 首個正式版：照使用者手機上的官方非 PRO「嘸蝦米輸入法」2.6.8 照刻的螢幕鍵盤與設定頁、內建使用者自建字表、候選列一定出現（直式與橫式）、語言模式（嘸／无／台／日）、同音查詢、加字加詞、快打連鍵不漏字、使用者設計的 App 圖示；core／cli／plugin／app 行＋分支覆蓋率 100%，模擬器端對端 100 個案例 |
