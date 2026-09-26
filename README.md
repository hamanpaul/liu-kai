# liu-kai

自製的 Android 嘸蝦米輸入法（Kotlin IME）。字碼來源為使用者自行持有的正版表格，於手機端匯入；本專案不附任何官方字表。

設計與里程碑見 [`docs/plan.md`](docs/plan.md)。

## Install

開發環境需求：JDK 17、Android SDK（platform 35、build-tools 35.0.0）。

```bash
export ANDROID_HOME=~/Android/Sdk
./gradlew assembleDebug
```

產出的 APK 位於 `app/build/outputs/apk/debug/`。安裝到裝置或模擬器後，於系統「螢幕鍵盤」設定中啟用 liu-kai。

## Usage

1. 開啟 liu-kai 設定頁，依引導啟用並切換輸入法。
2. 在設定頁「匯入字表」選擇自己持有的正版表格檔（例如 `liu_ibus_final.txt`）；匯入後會顯示筆數與雜湊。
3. 在任何輸入欄位以嘸蝦米字碼輸入；空白上屏首選，點選候選列可直接上屏。

開發者測試：

```bash
./gradlew test               # JVM 單元測試（合成字表）
scripts/emulator-e2e.sh      # 本機模擬器端對端測試
```

## Version

`VERSION` 是本專案版本的唯一來源；變更記錄以 `changelog.d/` fragment 累積，release 時彙整進 `CHANGELOG.md`。
