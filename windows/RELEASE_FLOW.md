# Windows x64 發版

Windows 的公開版號在 [VERSION](VERSION)。每個正式版本使用獨立的 `windows-vX.Y.Z` tag。首版 `windows-v0.1.0` 與 Android 首版同時交付，此後兩端可獨立升版。兩個平台的發行紀錄見[版本文件](../RELEASES.md)。

## 安裝版本

WiX 的 MSI 與 Burn 依數字版號決定能否原地升級。舊版使用過 `70.1.1`，所以新版本的三段數字採 `(X + 70).Y.(Z + 2)`。例如公開版 `0.1.0` 對應 `70.1.2`，`0.1.12` 對應 `70.1.14`。公開版號用於安裝檔名與版本目錄，另保留既有 MSI、Burn 升級識別碼。建置腳本會檢查三段內部數值符合 255、255、65535 的上限。

## 從指定 commit 建置

1. 更新 Windows 的 `VERSION` 及版本文件，合併通過檢查的變更。首次整合及之後的跨平台功能在同一張 issue 記錄兩端結果。
2. 從目標 commit 建立 `windows-vX.Y.Z` tag。根目錄的 [Windows installer workflow](../.github/workflows/windows-installer.yml) 讀取 `windows/VERSION`，執行既有測試與套件核對。它也能由 GitHub Actions 手動啟動；對正在審查的 PR 會自動執行。
3. 下載該 workflow 的 `windows-x64-<commit>` 成品，內含 EXE、`SOURCE.txt` 及 `SHA256SUMS.txt`。將這三個檔案放進同版號的草稿 Release。依照下一節用草稿下載的檔案實測、比對 SHA-256，通過後才公開。已發版的 tag 與檔案保留原狀。

### 封裝前讀音資料 gate

在 repo root 的 Windows 原生環境先跑測試與只讀 audit：

```powershell
python -m unittest discover -s scripts/tests -p test_audit_readings.py -v
python scripts/audit-readings.py --repo . --report build/readings-audit.json --check
```

Windows workflow 在既有單一 job 的 repo root 跑同一組合並保存 `build/readings-audit.json`。它也直接比較 `HEAD` 中 canonical 與 Windows copy 的 blob object ID，不能讓 checkout EOL 轉換掩蓋 committed bytes 漂移。封裝只接受 invariant PASS；但 `pronunciation_review_complete=false` 可以與 exit 0 同時成立，表示未知讀音仍待裁決，不能當成全部正音已核准。`unexpected_missing_character`、格式／UTF-8／重複／空組錯誤、copy byte mismatch 或決策格式錯誤會使 `--check` 失敗。不得藉由修改候選、readings 或 suppression 讓 report 變成零。

2026-11-27 Root 的 pre-fix Windows checkout 為 14,172 primary、25 secondary、37 literal、3,738 unresolved、0 missing、4,644 empty，stdout 為 `invariants=PASS; unresolved_relations=3738; pronunciation_review_complete=false`。基準 `a3f2c65` 是不同情況：canonical LF SHA-256 `f2f5d289ccec051d2f7185b49ed4e9c7ce885f179bf4b781a5dba3a69a7c9da8`、Windows copy CRLF SHA-256 `b09b8f4b2a520908aeb63465ef7b2effbd580c61b7f883f76a8341cdf74fdffb`，基準實際 FAIL `copy_byte_mismatch`。禁止宣稱該基準 raw-identical。本輪已把兩個 readings 路徑以 attributes 固定為 LF policy；Root 已完成 working bytes 的 CRLF→LF 正規化；兩檔均為 614,553 bytes、SHA-256 `f2f5d289ccec051d2f7185b49ed4e9c7ce885f179bf4b781a5dba3a69a7c9da8`。14 項 audit 方法、22 項完整 scripts suite 與全表 `--check` 已通過，文字欄位、順序及主要音規則未變。

完整未裁決清單、分類與代表例見 [讀音與候選資料核對](../docs/data/readings-audit.md)。原生 C++ 同音／長英文回歸及私有候選的沙盒跨版本／重入／資料保留已另行驗證，範圍見 [驗證紀錄](VALIDATION.md)；資料 gate 本身不能替代 runtime 驗證。[issue 19](https://github.com/hamanpaul/liu-kai/issues/19) 保持 open。

本機可在乾淨 checkout 的 `windows/` 下執行相同建置。工具為 PowerShell 7、Windows PowerShell 5.1、.NET Framework、x64 MinGW、WiX 6.0.2 與同版 Bal extension。R64 遷移補丁使用原 repo 的 [R64 MSI](https://github.com/hamanpaul/liu-kai_win/releases/download/r64/LiuKai.msi)，建置時核對既有 SHA-256。

```powershell
.\native\wix\build-platform-release.ps1 -WixExe $wix -BalExtension $bal -ToolchainBin $mingw -R64Msi $r64 -OutputDir "$PWD\dist\platform" -Tag 'windows-v0.1.0'
```

每次建置使用新輸出目錄。可發布的三個檔案在輸出目錄的 `release/`，其餘為中間產物與驗證所需的 MSI 和檔案清單。

### 安裝器 UI 資源 gate

`native/wix/ui/rtf-theme.xml` 與 `zh-TW.wxl` 是 [WiX 6.0.2 WixStdBA](https://github.com/wixtoolset/wix/tree/v6.0.2/src/ext/Bal/wixstdba) 的最小衍生，共用於 migration 與 development bundle。相鄰 `LICENSE.TXT` 是官方 [WiX 6.0.2 MS-RL](https://github.com/wixtoolset/wix/blob/v6.0.2/LICENSE.TXT)；bundle BA 另嵌入同檔，解出名稱為 `wix-theme-license.txt`。

platform build 之後，Windows workflow 以剛建出的真 MSI、migration EXE 及同次建出的 `LiuKaiInstallerGuard.dll` 執行 `native/tests/wix-installer-ui-build-tests.ps1`。測試從 MSI 的 ProductVersion 建立 development bundle，抽取兩個真 BA，核對 theme/wxl/license、BAFunctions 唯一註冊、guard payload/source SHA-256 與 x64 PE header，再重用單一 UI validator 與其 11 個 temp-copy mutation；不執行安裝。失敗目錄保留為 workflow artifact。

本機可對相同成品在 repo `windows/` 下另跑：

```powershell
.\native\tests\wix-installer-ui-build-tests.ps1 `
  -WixExe $wix `
  -BalExtension $bal `
  -Msi "$pwd\dist\platform\wix\LiuKai.msi" `
  -MigrationBundle "$pwd\dist\platform\wix\LiuKai-win-setup.exe" `
  -GuardDll "$pwd\dist\platform\wix\LiuKaiInstallerGuard.dll" `
  -OutputDir "$pwd\build\installer-ui-build-test" `
  -RuntimeVersion (Get-Content -LiteralPath "$pwd\VERSION" -Raw).Trim()
```

此 guard 只讓內含新 BAFunctions 的版本彼此排除，不能保護或 retrofit 已發行的舊 EXE。artifact/native core gate 不取代 Sandbox 的 100%/150% DPI、繁中不截斷、鍵盤導覽、真實 action/FilesInUse/Success/Failure 畫面、一般及已提權重入、取消、真 BA 中斷、engine cleanup、3010 與解除安裝驗收，也不預先宣稱修好歷史 `0x800700e8`。

## 驗收與診斷

首次共同發版在乾淨 Windows 分別驗證全新安裝、R64、R70 與 70.1.1 原地升級。升級前設定一項使用者偏好，升級後核對它仍保留、註冊的輸入法 DLL 與版本檔雜湊相符，在實際輸入框測試離線打字與該版共同案例，最後由同一支 EXE 解除安裝。核對目前版本的註冊與程式檔已移除。Windows Installer 回傳 `3010` 時記下重新啟動需求。

安裝失敗時保留 Burn 與 MSI 日誌（預設位於 `%TEMP%`）。符合條件的 MSI 失敗會嘗試在 `%LOCALAPPDATA%\LiuKai\diagnostics` 寫入診斷 ZIP。分享前請檢查帳號路徑及登錄資訊。[診斷範圍](native/INSTALL-DIAGNOSTICS.md) · [舊 repo 發行紀錄](https://github.com/hamanpaul/liu-kai_win/releases)
