# Windows 專案整合索引

原 `hamanpaul/liu-kai_win` 的 `main` 從提交 `7c6d1c2b663d5258acba6167ccf891a7d73e50c5` 匯入 `windows/`。匯入提交以 Android 和 Windows 的原提交為雙親，兩邊的作者、日期及提交 ID 可直接查詢。在匯入前，Windows 路徑位在原 repo 根目錄。[Windows 原始碼與歷史](https://github.com/hamanpaul/liu-kai_win) · [舊版安裝檔](https://github.com/hamanpaul/liu-kai_win/releases)

| 原 Windows issue | 整合後的議題 | 內容 |
| --- | --- | --- |
| [3](https://github.com/hamanpaul/liu-kai_win/issues/3) | [15](https://github.com/hamanpaul/liu-kai/issues/15) | Windows Terminal 偶發 Shift 切換失效 |
| [6](https://github.com/hamanpaul/liu-kai_win/issues/6) | [16](https://github.com/hamanpaul/liu-kai/issues/16) | 重跑升級與安裝登錄 |
| [7](https://github.com/hamanpaul/liu-kai_win/issues/7) | [17](https://github.com/hamanpaul/liu-kai/issues/17) | 已結案的 Windows 單次注音查字與實測 |
| [8](https://github.com/hamanpaul/liu-kai_win/issues/8) | [10](https://github.com/hamanpaul/liu-kai/issues/10) | 專案整合與發行 |
| [9](https://github.com/hamanpaul/liu-kai_win/issues/9) | [11](https://github.com/hamanpaul/liu-kai/issues/11) | Doctor Mode |
| [10](https://github.com/hamanpaul/liu-kai_win/issues/10) | [12](https://github.com/hamanpaul/liu-kai/issues/12) | 錯字與組字失敗分析 |
| [11](https://github.com/hamanpaul/liu-kai_win/issues/11) | [13](https://github.com/hamanpaul/liu-kai/issues/13) | 日常輸入延遲評估 |
| [13](https://github.com/hamanpaul/liu-kai_win/issues/13) | [18](https://github.com/hamanpaul/liu-kai/issues/18) | 舊版升級等待時間 |
| [14](https://github.com/hamanpaul/liu-kai_win/issues/14) | [19](https://github.com/hamanpaul/liu-kai/issues/19) | 「勒」與讀音／候選表核對 |
| [15](https://github.com/hamanpaul/liu-kai_win/issues/15) | [20](https://github.com/hamanpaul/liu-kai/issues/20) | WinForms 中文輸出 |

Windows 原有五張已合併 PR 的審查與對話保留在原站：[WiX 安裝器與同音字](https://github.com/hamanpaul/liu-kai_win/pull/1)、[占用時卸載與 R64 遷移](https://github.com/hamanpaul/liu-kai_win/pull/2)、[Shift 與 R70](https://github.com/hamanpaul/liu-kai_win/pull/4)、[回復測試工具鏈](https://github.com/hamanpaul/liu-kai_win/pull/5)、[單次注音反查](https://github.com/hamanpaul/liu-kai_win/pull/12)。

在本 repo 執行 `git tag -l 'legacy/windows/*'` 可列出原 Windows 的標籤與分支快照。原 tag 依原名放在 `legacy/windows/`，分支快照在 `legacy/windows/heads/`。例如執行 `git log legacy/windows/r64 -- native/wix` 即可以當時的原路徑追查舊版。[R64 MSI 固定下載](https://github.com/hamanpaul/liu-kai_win/releases/download/r64/LiuKai.msi)
