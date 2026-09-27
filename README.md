# liu-kai

自製的 Android 嘸蝦米輸入法（Kotlin IME）。字碼來源為使用者自建的字表（`liu_ibus_final.txt`＋`lime_liu7.txt`），於手機端匯入；本 repo 不附字表檔。

設計與里程碑見 [`docs/plan.md`](docs/plan.md)。

## Install

開發環境需求：JDK 17、Android SDK（platform 35、build-tools 35.0.0）。

```bash
export ANDROID_HOME=~/Android/Sdk
./gradlew assembleDebug
```

產出的 APK 位於 `app/build/outputs/apk/debug/`。安裝到裝置或模擬器後，於系統「螢幕鍵盤」設定中啟用 liu-kai。

內建字表：建置時若本機有使用者自建字表（預設 `~/prj_pri/liu-kai-data/`，可用 `-Pliukai.tableDir=<目錄>` 或環境變數 `LIU_KAI_DATA` 指定），會以 `liu-kai-cli bundle` 編譯後內建進 APK，裝好第一次使用就能打中文；沒有字表時（例如 CI）不內建，需在設定頁匯入。設定頁可匯入其他字表覆蓋，「清除字表」會改回內建字表。

手機側載用的測試 APK 放在 test-apks 分支（不建立 GitHub Releases），下載連結、SHA-256 與對應的 source commit 見該分支的 APKS.md；需要 Android 11 以上。

## Usage

1. 開啟 liu-kai 設定頁，依引導啟用並切換輸入法。
2. 在設定頁「匯入字表」同時選擇 `liu_ibus_final.txt` 與 `lime_liu7.txt`；匯入後會顯示各區段筆數與來源雜湊。
3. 在任何輸入欄位以嘸蝦米字碼輸入；空白上屏首選，點選候選列可直接上屏。

支援的匯入組合：`liu_ibus_final.txt`＋`lime_liu7.txt`（以後者切分區段、沿用前者頻率）、單一 CIN／LIME 多區段檔、或 `liu-kai-cli convert` 產生的中性 TSV。只有 IBus 檔時無法切分區段，會拒絕匯入。

輸入操作（實體鍵盤）：字碼後按空白上屏首選（空碼時直接出空白）；數字鍵選目前頁候選（0 為預設字，1–9 為其後候選）；字碼後加 `v`／`r`／`s`／`f` 選標籤 1～4 的候選（「字碼＋鍵」本身是字碼時當字根）；`=`／`-` 翻頁；Enter 送出字碼字母、Esc 清除組字；組字中按到標點等非字根鍵、數字超出候選數、打滿四碼後再打字根，都是組字失敗：清除組字、不出字，輸入畫面以紅框提示；`*` 為萬用字元（零到多個字根）；組字後按 `` ` `` 查首選的讀音與同音字；單按 Shift 切換中英，Shift＋字母直接輸出大寫；假名不需切換模式，直接打羅馬拼音加 `,` 為平假名、加 `.` 為片假名（例如 `ka,` → か、`ka.` → カ）。

螢幕鍵盤照官方嘸蝦米 PRO 的配置、外觀與行為（預設為使用者手機的「經典灰＋顯示按鍵＋直式按鍵高」，其他主題與尺寸在設定頁選）：
- 中文模式：鍵帽大寫；Shift 位置是「同音」鍵（按同音、打字碼、選字後列出該字的同音字；從同音字上屏後候選列顯示該字的字碼）；長按「同音」選語言模式 嘸（繁中）／无（簡體）／台（打繁出簡）／日（日文漢字與假名），會記住上次的選擇；中英鍵顯示 En；空白鍵標「嘸蝦米」；「.'[]」鍵點按為字根「.」、長按彈出字根 ' [ ]；沒有組字時候選列顯示常用標點 ! ? , " : ( ) -，點選直接上屏。
- 英文模式：鍵帽小寫；Shift 按一次單次大寫、再按一次大寫鎖定；a／s／c／n 長按彈出重音字母；長按「.」彈出常用標點；欄位開頭與句號後自動大寫。
- 組字中「智慧鍵盤」淡化不能接的字根鍵，「,」「.'[]」不能接時隱藏、空白鍵加寬。
- 按住按鍵顯示放大預覽；沒有組字時按住空白鍵左右滑動移動游標；在 ?123／ALT 層打符號後按空白或 Enter 自動回字母層。
- 第一排字母長按輸入數字（組字中即選字）；?123 層、ALT 層另有字根 `'` `[` `]`、萬用字元 `*` 與反引號 `` ` ``；長按「,」彈出 ⚙ 開啟設定；候選列右端的麥克風切換到系統的語音輸入（需先在系統設定啟用，例如 Google 語音輸入）。

設定頁（照官方）：
- 「加字加詞」：自訂拆碼與字詞（拆碼可用英文字母、數字、「,」「.」，第一碼不可為數字），可新增、編輯、刪除、上移下移、匯入與匯出 TSV；按「儲存」後，下一次開始輸入時生效，自訂字詞排在同碼候選前面。
- 「嘸蝦米鍵盤設定」：按鍵時震動、按鍵時播放音效、按鍵時顯示彈出式視窗、自動返回字元輸入模式、顯示退出鍵盤鍵、英文自動大寫、智慧鍵盤。
- 「設定主題及配置」：直式／橫式的按鍵高度與字體（各五段）、顯示數字鍵、主題樣式（質感白、沉勁黑、經典灰、魅力紅、奇想綠、星空藍、甜蜜紫、櫻花粉、靜謐黃、濃可可）、顯示按鍵。顏色與尺寸取自官方 PRO 3.0.9 的實測。
- 官方的英文拼字建議、自動完成、輕觸修正與關聯字詞目前不提供（使用者 2026-09-27 裁決）。

開發者工具：

```bash
./gradlew :core:check :cli:check                 # JVM 單元測試＋JaCoCo 100%（行＋分支）門檻
(cd testpilot && .venv/bin/python -m pytest)     # TestPilot plugin 單元測試＋100% 門檻
scripts/emulator-e2e.sh                          # 模擬器端對端：TestPilot 案例、報告、app 覆蓋率 100% 門檻
scripts/emulator-e2e.sh --case hw-wildcard       # 只跑指定案例
testpilot/.venv/bin/liu-kai-diff --official <IME id>   # 與官方嘸蝦米 PRO 對照（模擬器需先安裝官方 PRO；報告逐項標示是否符合使用者確認的行為）
./gradlew :cli:run --args="stats <liu_ibus_final.txt> <lime_liu7.txt>"   # 桌機核對字表區段統計
```

端對端測試由 `testpilot/` 內的 TestPilot plugin（`liu_kai`）執行：案例為 `testpilot/liu_kai_testpilot/cases/*.yaml`（環境前置條件、步驟、通過條件），以 adb 驅動模擬器上的 liu-kai 與 testhost，報告輸出到 `testpilot/reports/<時間>/report.{md,json}`。測試環境設定見 `testpilot/liu_kai_testpilot/testbed.yaml.example`（`scripts/emulator-e2e.sh` 會依此範本自動產生實際使用的設定檔）。`real-*` 案例需要 `~/prj_pri/liu-kai-data/` 內的真實字表。

`liu-kai-cli` 指令說明：

```text
liu-kai-cli <command> [options]

commands:
  stats   <file>...                         匯入字表並印出各區段統計（不寫檔）
  convert <file>... --out <tsv>             匯入字表並輸出中性 TSV（請寫到 repo 外）
  bundle  <file>... --out <liutable>        匯入字表並輸出 app 直接載入的二進位字表（建置時內建進 APK）
  gen-readings --unihan <Unihan_Readings.txt> [--grades <Unihan_DictionaryLikeData.txt>] --out <readings.tsv>
                                            由 Unihan kMandarin 產生注音讀音表（可附 kGradeLevel 常用度）
```

## Version

`VERSION` 是本專案版本的唯一來源；變更記錄以 `changelog.d/` fragment 累積，release 時彙整進 `CHANGELOG.md`。
