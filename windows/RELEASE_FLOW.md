# Windows x64 發版

Windows 的公開版號在 [VERSION](VERSION)。每個正式版本使用獨立的 `windows-vX.Y.Z` tag。首版 `windows-v0.1.0` 與 Android 首版同時交付，此後兩端可獨立升版。兩個平台的發行紀錄見[版本文件](../RELEASES.md)。

## 安裝版本

WiX 的 MSI 與 Burn 依數字版號決定能否原地升級。舊版使用過 `70.1.1`，所以新版本的三段數字採 `(X + 70).Y.(Z + 2)`。例如公開版 `0.1.0` 對應 `70.1.2`，`0.1.12` 對應 `70.1.14`。公開版號用於安裝檔名與版本目錄，另保留既有 MSI、Burn 升級識別碼。建置腳本會檢查三段內部數值符合 255、255、65535 的上限。

## 從指定 commit 建置

1. 更新 Windows 的 `VERSION` 及版本文件，合併通過檢查的變更。首次整合及之後的跨平台功能在同一張 issue 記錄兩端結果。
2. 從目標 commit 建立 `windows-vX.Y.Z` tag。根目錄的 [Windows installer workflow](../.github/workflows/windows-installer.yml) 讀取 `windows/VERSION`，執行既有測試與套件核對。它也能由 GitHub Actions 手動啟動；對正在審查的 PR 會自動執行。
3. 下載該 workflow 的 `windows-x64-<commit>` 成品，內含 EXE、`SOURCE.txt` 及 `SHA256SUMS.txt`。將這三個檔案放進同版號的草稿 Release。依照下一節用草稿下載的檔案實測、比對 SHA-256，通過後才公開。已發版的 tag 與檔案保留原狀。

本機可在乾淨 checkout 的 `windows/` 下執行相同建置。工具為 PowerShell 7、Windows PowerShell 5.1、.NET Framework、x64 MinGW、WiX 6.0.2 與同版 Bal extension。R64 遷移補丁使用原 repo 的 [R64 MSI](https://github.com/hamanpaul/liu-kai_win/releases/download/r64/LiuKai.msi)，建置時核對既有 SHA-256。

```powershell
.\native\wix\build-platform-release.ps1 -WixExe $wix -BalExtension $bal -ToolchainBin $mingw -R64Msi $r64 -OutputDir "$PWD\dist\platform" -Tag 'windows-v0.1.0'
```

每次建置使用新輸出目錄。可發布的三個檔案在輸出目錄的 `release/`，其餘為中間產物與驗證所需的 MSI 和檔案清單。

## 驗收與診斷

首次共同發版在乾淨 Windows 分別驗證全新安裝、R64、R70 與 70.1.1 原地升級。升級前設定一項使用者偏好，升級後核對它仍保留、註冊的輸入法 DLL 與版本檔雜湊相符，在實際輸入框測試離線打字與該版共同案例，最後由同一支 EXE 解除安裝。核對目前版本的註冊與程式檔已移除。Windows Installer 回傳 `3010` 時記下重新啟動需求。

安裝失敗時保留 Burn 與 MSI 日誌（預設位於 `%TEMP%`）。符合條件的 MSI 失敗會嘗試在 `%LOCALAPPDATA%\LiuKai\diagnostics` 寫入診斷 ZIP。分享前請檢查帳號路徑及登錄資訊。[診斷範圍](native/INSTALL-DIAGNOSTICS.md) · [舊 repo 發行紀錄](https://github.com/hamanpaul/liu-kai_win/releases)
