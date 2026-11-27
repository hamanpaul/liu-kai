# 版本紀錄

## 雙平台版本

整合後兩端各自從 **0.1.0** 起算，首版來自同一個合併提交。後續獨立遞增，例如 Android 可到 `0.1.1`，Windows 可到 `0.1.12`。下載時以平台前綴辨認新發行線。歷史 Android `v0.1.0` 和 Windows `r70` 等資產保留在原網址。

| 平台 | 正式版、標籤與來源 | 下載檔 | 系統升級身分 |
| --- | --- | --- | --- |
| Android | [android-v0.1.1](https://github.com/hamanpaul/liu-kai/releases/tag/android-v0.1.1) | `liu-kai-android-0.1.1.apk` | `com.hamanpaul.liukai`，versionName 0.1.1、versionCode 3；保留相容簽章 |
| Windows x64 | [windows-v0.1.1](https://github.com/hamanpaul/liu-kai/releases/tag/windows-v0.1.1) | `liu-kai-windows-0.1.1-x64-setup.exe` | MSI／Burn 70.1.3；沿用原 MSI 與 Bundle 的 UpgradeCode |

本表隨發行 PR 更新為目前來源的目標版本，成品是否已公開以對應 GitHub Release 為準。Android 公開版號在 [app/VERSION](app/VERSION)，Android 內部安裝序號在 [app/VERSION_CODE](app/VERSION_CODE)，每個正式版須大於所有已分發 APK 的安裝序號。Windows 公開版號在 [windows/VERSION](windows/VERSION)，安裝版號的換算與驗證見 [Windows 發行說明](windows/RELEASE_FLOW.md)。根層 [VERSION](VERSION) 和無平台前綴的 annotated tag 是 paulsha-conventions 的整合批次，例如這次 `v0.1.3`；兩個產品各自使用自己的 lightweight tag 和 GitHub Release。

### Android 0.1.1

批次 `0.1.3` 先交付 Android Doctor 與標點輸入修正；該批次沒有升級 Windows。後續 Windows Doctor 的交付見下節，雙端驗收結果共用[同一議題](https://github.com/hamanpaul/liu-kai/issues/11)。

### Windows 0.1.1

批次 `0.1.4` 已正式發布 [Windows Doctor 0.1.1](https://github.com/hamanpaul/liu-kai/releases/tag/windows-v0.1.1)，來源為 `a3f2c65cea6db99e1a4cf3e7e468d6296621296f`；Android 維持已發布的 `0.1.1`，沒有重新封裝或移動其標籤。Windows 同一成品已完成沙盒安裝／卸載、本機 R70 升級、Search／Store、Terminal／WSL 與實體 Shift 驗證，範圍及限制見 [Windows 驗證紀錄](windows/VALIDATION.md)。三個公開附件已下載核對；後續文件提交不改變已驗收安裝檔的來源及標籤。

### 發行流程

1. 更新要發行的平台版號、安裝序號與本表。整合 PR 通過雙端 CI 後，以正式提交建立批次 annotated tag 及該平台的 lightweight tag。首版的兩個平台標籤指向同一提交。只有一端升版時，只新增該端的產品標籤。
2. 從同一提交製作及實測成品。Android 在有核准字表與原簽署身分的環境執行 `python3 scripts/android-release.py build <輸出目錄>`；Windows 使用 [Windows 發行入口](windows/RELEASE_FLOW.md)。各端均會輸出安裝檔、`SOURCE.txt`（來源 SHA 與版號）及 `SHA256SUMS.txt`。以成品做離線輸入、升級與資料保留驗收，將結果附在對應 issue。
3. 在該提交執行 `python3 scripts/publish-platform-release.py --android <APK輸出目錄> --windows <EXE發布目錄>`。程式把兩端已測成品放入各自的草稿，並下載核對。只有一端發版時只給該平台參數。
4. 實測通過後以相同參數加 `--publish`，另從安裝驗收紀錄傳入 `--accepted-android-sha256` 及／或 `--accepted-windows-sha256`。首版須同時提供兩端；後續只需提供本次發布的平台。所有草稿均通過後才開始公開，GitHub 對每份 Release 各執行一次公開操作。中斷時重跑同一命令，它會讀回原草稿／已公開狀態，核對既有檔案與雜湊，再補完剩餘步驟。修正已發出的程式時增加版本，保留原下載。

## 歷史 Android 版本

| 版本 | tag | commit | APK | 摘要 |
|------|-----|--------|-----|------|
| 0.1.1 | `v0.1.1` | `355856dd743dd996508cd42aaf3417a6123f5301` | `liu-kai-0.1.1.apk`（SHA-256 `cb2218c6c007771314c88465a056337c8b4c3cf62e94370131786e76f74df824`） | 修正邊緣／鍵間隙命中與空白鍵順序，增加觸控取證；使用者實機試用順手後決定發布。沿用已試用 APK，內部 versionName／versionCode 仍為 0.1.0／1；完整模擬器 107/108、app 覆蓋率 99.9%／98.2%，保留未修 race，依使用者決定接受本次例外，遇到再開單。 |
| 0.1.0 | `v0.1.0` | `3c0b836de49a9aacfd75b8e56169d69e8be7a5f8` | `liu-kai-0.1.0.apk`（SHA-256 `59763afc31bfad6a24548f97cc4f9c2b275598751eeb94cabfd653adc3e4dc4f`） | 首個正式版：照使用者手機上的官方非 PRO「嘸蝦米輸入法」2.6.8 照刻的螢幕鍵盤與設定頁、內建使用者自建字表、候選列一定出現（直式與橫式）、語言模式（嘸／无／台／日）、同音查詢、加字加詞、快打連鍵不漏字、使用者設計的 App 圖示；core／cli／plugin／app 行＋分支覆蓋率 100%，模擬器端對端 100 個案例 |
