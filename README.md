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

支援的匯入組合：`liu_ibus_final.txt`＋`lime_liu7.txt`（以後者切分區段、沿用前者頻率）、單一 CIN／LIME 多區段檔、或 `liu-kai-cli convert` 產生的中性 TSV。只有 IBus 檔時無法切分區段，會拒絕匯入。

輸入操作（實體鍵盤）：字碼後按空白上屏首選；數字鍵選目前頁候選；字碼後加 `v`／`r`／`s`／`f` 選第 2～5 候選；`?`（恰一字根）與 `*`（任意字根）為萬用字元；組字後按 `` ` `` 查首選的讀音與同音字（觸控時長按候選）；單按 Shift 切換中英；Ctrl+J 切換日文。

開發者工具：

```bash
./gradlew test                                   # JVM 單元測試（合成字表）
scripts/emulator-e2e.sh                          # 本機模擬器端對端測試（demo 合成表）
scripts/emulator-e2e.sh --real ~/prj_pri/liu-kai-data   # 匯入正版字表到模擬器，供驗收
./gradlew :cli:run --args="stats <liu_ibus_final.txt> <lime_liu7.txt>"   # 桌機核對正版表區段統計
```

`liu-kai-cli` 指令說明：

```text
liu-kai-cli <command> [options]

commands:
  stats   <file>...                         匯入字表並印出各區段統計（不寫檔）
  convert <file>... --out <tsv>             匯入字表並輸出中性 TSV（請寫到 repo 外）
  gen-readings --unihan <Unihan_Readings.txt> --out <readings.tsv>
                                            由 Unihan kMandarin 產生注音讀音表
```

## Version

`VERSION` 是本專案版本的唯一來源；變更記錄以 `changelog.d/` fragment 累積，release 時彙整進 `CHANGELOG.md`。
