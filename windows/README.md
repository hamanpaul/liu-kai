# 嘸．改 Windows x64

發行建置、測試、診斷與驗收流程見 [RELEASE_FLOW.md](RELEASE_FLOW.md)。

原生 Windows TSF 輸入法，使用 Android 手 key 字表與無改圖示。字碼以原字根表為準；品牌保持彩色，另有獨立中／英狀態。不包含自訂字詞、關聯詞或品牌灰階切換。

Windows 新發行線從 **0.1.0** 開始，本來源準備 **0.1.1**；正式包是否已公開以 Release 頁為準。請使用 EXE 安裝或原地升級；再次開啟同一支安裝檔可選擇解除安裝。[下載 Windows 0.1.1 安裝檔](https://github.com/hamanpaul/liu-kai/releases/download/windows-v0.1.1/liu-kai-windows-0.1.1-x64-setup.exe) · [版本與 SHA-256](https://github.com/hamanpaul/liu-kai/releases/tag/windows-v0.1.1) · [先前 R70 發行紀錄](https://github.com/hamanpaul/liu-kai_win/releases/tag/r70)

## R60 歷史驗收紀錄

R60（`0.0.0-dev.20261026.r60.machine-com-x64`）已於 2026-10-26 完成本輪使用者實機驗收：Windows Search、Microsoft Store 的 `a + Space`／`oek + Space` 中文輸入，以及獨立中／英模式圖示，使用者確認「三項都正常」。安裝後 12 個套件檔案雜湊與 HKCU64／HKLM64 雙 scope 的獨立驗後均通過。

[下載 R60 安裝程式](https://github.com/hamanpaul/liu-kai_win/releases/download/r60/WuGai-R60-x64-Setup.exe) · [SHA256](https://github.com/hamanpaul/liu-kai_win/releases/download/r60/SHA256SUMS.txt) · [發行說明](https://github.com/hamanpaul/liu-kai_win/releases/tag/r60)

已知安裝問題：上述 R60 的已安裝狀態驗收不等於新電腦安裝驗收。R60 首次註冊的圖示讀回缺陷已修正；另一台 R60.1 的 `0x8007000D` 仍待完整交易紀錄確認。此分支另加入正式卸載入口及診斷，驗證範圍見 [VALIDATION.md](VALIDATION.md)。

Terminal 沿用先前正常結果，本輪沒有重新測試。不宣稱完整 77 項 backlog 或官版功能 1:1 全部完成。驗收範圍見 [VALIDATION.md](VALIDATION.md)。

R60 新增明確的 x64 機器層級 COM 註冊，與使用者層級一起處理升級、修復、回復與解除。相同 SID、安裝目錄與所有權才允許寫入。保留舊交易格式與記錄，不改寫歷史。

當時的 **Windows x64 prerelease** tag 為 `r60`；安裝檔為 `WuGai-R60-x64-Setup.exe`。安裝必須以同一 Windows 帳號完成管理員授權。此來源不含官版程式、逆向輸出、本機部署記錄、登入憑證或私人來源路徑。

## 同音字查詢

在中文輸入模式先按 `'`，接著輸入已知同音字的字碼，按空白選擇該字，顯示同音候選字；以空白鍵翻頁、數字鍵選字。例：`'`、`mnfo`、空白、`0` 可由「瞎」查找並輸出「蝦」。不同字的讀音和候選字以隨附字典資料為準。

**也可以直接用注音查字碼。** 在中文模式依序按 `'`、`;`，再用標準注音鍵盤組音。例如按 `v`、`u`、`8` 輸入「ㄒㄧㄚ」，按空白可列出「蝦」等同音字。候選取自目前字表，依主要讀音查詢，每個候選旁的拆碼依你的顯示設定呈現。

一聲按空白列出候選，二、三、四聲按聲調鍵即可列出，輕聲按 `7` 後再按空白。候選出現後，空白翻頁，上下鍵移動，Enter 選字，也能依畫面上的數字直接選字。選完只輸出漢字，下一鍵就能打一般嘸蝦米。要修正讀音，可在候選清單按 Backspace 返回組音，再按 Backspace 刪除最後一個注音符號或聲調。按 Esc 可取消整次查詢。

## 原始碼建置

正式安裝程式使用 [WiX 發行流程](RELEASE_FLOW.md#從指定-commit-建置) 的乾淨來源建置與驗證。以下保留手動編譯原生工具的範例，從 `windows/` 目錄執行，需要 Windows x64、w64devkit x64 C++/windres、Python 3.10 以上及 PowerShell 7；產物仍須經完整發行檢查與封裝。

```powershell
New-Item -ItemType Directory -Force build/x64 | Out-Null
# 改為本機的 w64devkit 安裝目錄。
$toolchain = 'C:\tools\w64devkit\bin'
$compiler = Join-Path $toolchain 'g++.exe'
$windres = Join-Path $toolchain 'windres.exe'
$env:PATH = $toolchain + ';' + $env:PATH
python native/toolchain.py --architecture x64 --compiler $compiler --check
& $windres -I assets native/tsf/resources.rc -O coff -F pe-x86-64 -o build/x64/resources.o
python native/toolchain.py --architecture x64 --compiler $compiler -- -std=c++17 -shared -static -municode native/tsf/service.cpp native/tsf/service.def build/x64/resources.o -lole32 -loleaut32 -luuid -lgdi32 -ladvapi32 -lbcrypt -o build/x64/BoshiamyTsf.dll
python native/toolchain.py --architecture x64 --compiler $compiler -- -std=c++17 -static -municode -mwindows native/tools/lookup_main.cpp build/x64/resources.o -lcomdlg32 -lcomctl32 -lshell32 -lgdi32 -o build/x64/LiuKaiLookup.exe
python native/toolchain.py --architecture x64 --compiler $compiler -- -std=c++17 -static -municode -mwindows native/tools/settings_main.cpp build/x64/resources.o -lcomdlg32 -lcomctl32 -lshell32 -lgdi32 -ladvapi32 -lbcrypt -o build/x64/LiuKaiSettings.exe
python native/toolchain.py --architecture x64 --compiler $compiler -- -std=c++17 -static -municode native/tools/register_main.cpp -lole32 -loleaut32 -luuid -ladvapi32 -o build/x64/LiuKaiRegister.exe
python native/toolchain.py --architecture x64 --compiler $compiler -- -std=c++17 -static -municode native/installer/lifecycle_main.cpp -lole32 -loleaut32 -luuid -ladvapi32 -lbcrypt -lshlwapi -o build/x64/LiuKaiInstaller.exe
Get-ChildItem assets -File | Where-Object { $_.Extension -in @('.tsv','.json','.md') } | Copy-Item -Destination build/x64
pwsh -NoProfile -File native/build-branding.ps1 -Architecture x64 -Compiler $compiler -ResourceCompiler $windres -OutputDll build/x64/branding.dll
```

R60 歷史封裝入口為 `native/package-windows.ps1` 與 .NET Framework 外殼 `native/package/Setup.cs`；`install-release.ps1` 依賴完整 release manifest 與註冊工具。這些檔案保留供舊版維護，現在的正式安裝包依上述 WiX 流程產生。

## Doctor Mode

右下角嘸．改圖示的右鍵選單提供同一安裝包的版本、目前使用者的 Doctor 資料夾路徑及設定入口。
背景記錄與停用後的標記、匯出、清除沿用 `LiuKaiSettings.exe` 的同一份儲存處理，只有取得寫入權的程序能操作資料。
初始為關閉，開啟後才設定這個使用者的登入續記。原始紀錄保留上限為 32 MiB／7 天；敏感與未知欄位只有健康狀態及生命週期。
標記、預覽、儲存、清除與資料範圍見 [Doctor 使用說明](../docs/doctor.md)。
設定畫面與 WiX 安裝流程已整合。候選包已完成一小時穩定性及原版工具的三組受控效能對照；正式成品與完整宿主矩陣仍待驗收。實際範圍見 [驗證紀錄](VALIDATION.md) 及 [發行驗證計畫](../docs/superpowers/plans/2026-11-27-doctor-windows-release.md)。

### Terminal 的欄位資訊

Windows Terminal 1.25.3131.0 的預設設定實測未提供欄位種類，因此只能記錄健康狀態；中文與英文輸入功能不受影響。
若需要 Terminal 的輸入階段診斷，可在 Terminal 的 `settings.json` 頂層加入以下選用設定，再關閉並重新開啟 Terminal：

```json
"defaultInputScope": "alphanumericHalfWidth"
```

本設定會把 Terminal 宣告為一般欄位。Terminal 裡的 `sudo` 等密碼提示通常不會另行宣告密碼種類，Doctor 無法自動識別；
需要時請先停用 Doctor。記錄格式沒有輸入文字、原始按鍵或座標欄位。產品不會代改 Terminal 設定，也不會把缺少資訊的欄位當成一般輸入。

### 開發驗證

`--doctor-collector`／`--doctor-once` 是產品背景入口；`--doctor-probe` 才是開發驗證入口。
TSF callback 只讀已在生命週期快取的 InputScope 分類並嘗試非阻塞寫入；scope 透過 document/context
通知、`GetAppProperty(GUID_PROP_INPUTSCOPE)` 與有效 read edit cookie／range 的 `VARIANT` 取得。
`VT_EMPTY` 或失敗固定保留 Unknown，不推定 ordinary。collector 不存在、
queue 滿或 heartbeat 過期時，輸入不等待。DLL 不建立 worker，也不在卸載等待 flush。

共享 mapping 由低箱 producer 在自身 `Local\...` namespace 建立；medium collector 使用目標 token 的
`GetAppContainerNamedObjectPath` 開啟，先核對 target user/session/process creation 及 mapping owner。
mapping 為 protected DACL：exact user 與低箱自身 exact package SID 只給 section read/write，
collector 另以 READ_CONTROL 驗 owner；mutex 名亦包含 user SID。Sandbox 已完成不同帳號與 AppContainer 的隔離測試，
不代表所有宿主、登入工作階段或正式安裝流程已通過。

開發環境可用唯一輸出目錄重跑測試與 probe build：

```powershell
# 改為本機的 w64devkit 安裝目錄。
$toolchain = 'C:\tools\w64devkit\bin'
& .\native\run-doctor-p0-tests.ps1 -ToolchainBin $toolchain
& .\native\build-doctor-probe.ps1 -ToolchainBin $toolchain
```

儲存、背景收集、設定畫面與安裝維護測試由 `native/run-doctor-tests.ps1` 串行執行，使用獨立輸出及暫存目錄。
這些測試需要一般使用者權限；GitHub 的管理員 runner 透過明確的 `-DoctorEphemeralTestUser` 選項，
在暫建的一般測試帳號中執行並於結束後移除帳號，不放寬產品的權限檢查。

此 probe build script 只產生開發產物，不自動註冊或安裝 DLL；每次輸出位於新的 GUID 目錄，不刪既有 build。正式發版使用既有 WiX 發行流程。
`native/tests/run-doctor-host-probe.ps1` 的 Start／Snapshot／Stop／AccessCheck 供 Terra 以真實 host token
及不同帳號／AppContainer identity 驗證。已完成的受控宿主、AppContainer 與相容設定測試，不能代表預設 Terminal、SearchHost、Store、WSL 等完整矩陣均已通過；各項限制與結果見 [驗證紀錄](VALIDATION.md)。

## R60 系列舊安裝器的解除安裝方式

R60.20 起，安裝或移除發生錯誤時，安裝程式會嘗試自動收集 WuGai 安裝紀錄、交易及相關狀態，在 `%LOCALAPPDATA%\LiuKai\diagnostics` 產生診斷 ZIP，並在錯誤視窗與安裝 LOG 中記錄路徑。診斷資料留在本機，不會自動上傳；可能包含 Windows 帳號路徑。若收集失敗且 LOG 可寫入，會記錄原始錯誤與收集失敗原因。取消安裝確認不視為錯誤。

安裝及獨立讀回成功後，安裝器會在 Windows「設定 → 應用程式 → 已安裝的應用程式」加入 **WuGai** 的解除安裝入口。卸載器跟隨安裝檔一起提供，不需要另下載腳本。也可直接執行同版 Setup：

```powershell
.\WuGai-R60.2-x64-Setup.exe --uninstall
```

自訂的既有安裝位置可用 `--uninstall --install-root "C:\完整路徑\LiuKai"`。請使用原安裝帳號完成 UAC 授權；開始前關閉正在使用輸入法的程式。`--silent` 只隱藏結果視窗，不略過 UAC。

卸載器先確認套件、帳號、所有權及交易歷史，備份完整檔案，再移除輸入法與 COM 註冊、程式檔案及 Windows 應用程式入口。程式檔案若被占用、含未知檔案，或存在無法驗證的未完成交易，會保留現況並記錄失敗；不會直接刪除整個資料夾來掩蓋問題。

維護檔放在安裝目錄旁的 `<安裝目錄>.maintenance`，包含 `Uninstall.exe` 及 `maintenance.json`。使用者設定不刪除；LOG 在 `%LOCALAPPDATA%\LiuKai\installer-logs`，卸載備份在 `%LOCALAPPDATA%\LiuKai\uninstall-backups`，原交易歷史也保留。

此開發分支在刪檔前保存恢復紀錄。若刪檔途中程序被終止，下一次以原帳號執行新版安裝或解除安裝時，會先核對紀錄與備份，再恢復檔案及原有入口；若資料已改動或狀態無法確認，會停止並保留紀錄。此流程目前只完成隔離測試，尚未完成實機卸載或斷電驗證。R60 和 R60.2 的既有發布檔不包含這項新功能。

此功能不會將舊的失敗交易標成成功，也不會自動抹除歷史來強行重裝。尚未完成乾淨 Windows 的安裝／卸載／重裝實機驗收。

## 測試與來源範圍

`native/tests/` 保留本輪 machine COM、交易／回復、安裝腳本讀回及中英狀態的既有測試來源。部分整合測試需要另外提供受控的完整套件 fixture；私人／官版 fixture 不隨 repo 發布。選用的歷史 baseline 比對標頭亦未附帶。這份匯出未重新執行測試，不將來源測試視為實機驗收。

五個產品入口及其 quoted-include 依賴由來源 commit `2fea4b8` 匯出；`SOURCE-MANIFEST.json` 描述的是該匯出基準。
本開發分支的 Doctor P0 已直接修改 `native/tsf/service.cpp`、settings entrypoint 及其新增 header，不能再把目前產品 C++ 視為原樣。
七份資料來自實際 R60 套件，phonetic provenance 已清除私人路徑。第三方來源與聲明見 `THIRD_PARTY_NOTICES.md`。
