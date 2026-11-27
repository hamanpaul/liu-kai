# 安裝與字表計畫對抗摘要

日期：2026-11-27。模型：透過 Windows 原生 agy CLI 使用 `gemini-4-neon-high`。共三輪，上限八輪；所有審查均為文字規劃，沒有操作 WSL、修改產品、部署或發版。

使用者已裁決兩個入口維持原有主要音規則，先核對資料；該裁決納入第二輪與最終文件。

## 逐輪採納與反駁

| 輪次 | Neon 主張 | 最終處理 |
| --- | --- | --- |
| 1 | 必須測真正跨版本與 3010 未重啟 | 採納；baseline 0.1.1、私有候選 0.1.2/70.1.4，manifest/目錄一致，分別驗證重啟前後 |
| 1 | 兩份表不能互相證明正音真值 | 採納；集合差與權威正音分開，未判定關聯不自動修正 |
| 1 | 先不要預設整份客製 ThemeFile | 以固定 WiX 原始碼核實後收斂：官方 RtfTheme 缺 action 控制項，最小 theme 有必要；不寫新 BA |
| 1 | 用直接 MSI 升級構造災後狀態即可 | 部分駁回；產品刻意有 bundle 升級 guard，直接 MSI 僅可作額外 oracle，不代替真 BA 中斷 |
| 1 | 長尾未判定可移到新單後結案 | 駁回；保留原 issue 16/19 的未完成驗收，不移走未知來關單 |
| 2 | Sandbox Restart 就清空，必須新建可重啟 VM | 駁回並由第三輪接受：Windows 11 22H2+ guest 內 Restart 保留狀態；關閉 instance 才清空。仍加本地 sentinel/登錄/boot time 預檢 |
| 2 | BA 故障注入不能太早且須核對目標 | 採納；只在 InstallFinalize 成功後的非提權 BA 窗口注入，最多兩次，未形成目標現場記未覆蓋 |
| 2 | 1618 需程式化 error-code theme 條件 | 駁回新變數/BAFunctions；保留原始錯誤文字，加靜態條件句操作引導 |
| 2 | 雙 authoring/UI 與 JDK 預檢需明確 | 採納；兩份 bundle 都建置抽取檢查；已找到可執行的原生 JDK 17，但未冒稱 Gradle 已通過 |
| 3 | 沒有第二筆 MSI 不代表撞過鎖 | 採納；加入 Apply 時間重疊、明確拒鎖 log、零第二筆交易三條件，未命中不能算 PASS |
| 3 | 未預期缺字與裁決表需要嚴格門禁 | 採納；缺字回非零、封閉 status、source 非空、禁止孤兒/重複裁決，不容許裁決掩蓋結構錯誤 |

第三輪結論：無剩餘 Critical、無過度設計；補入兩條具體 Important 門檻後即可定稿，不需要第四輪。上述門檻已寫入兩份 plan，仍未執行。

## 可開始實作的範圍與保留限制

- [安裝計畫](2026-11-27-windows-installer-flow.md)：先收集真跨版本/重入/恢復證據，再做最小官方 UI 資源修改。#16 的恢復驗收沒命中就不能關單。
- [資料計畫](2026-11-27-readings-consistency.md)：先交付完整只讀核對、同步門禁與裁決報告，不改 query/generator/候選資料；未知正音仍保留在 #19。
- [共用設計](../specs/2026-11-27-installer-readings-design.md)：使用者規則、資料計數、來源及不做事項。
- 本次沒有 code diff、產品建置/安裝、WSL、模擬器、commit/push/PR/release。文件檢查不代表產品測試。

## 可核對的官方依據

- [WiX 6.0.2 ApplyLock](https://github.com/wixtoolset/wix/blob/v6.0.2/src/burn/engine/apply.cpp#L418)：Burn 已有執行鎖，不新增鎖框架。
- [WiX 6.0.2 ACTIONSTART 更新](https://github.com/wixtoolset/wix/blob/v6.0.2/src/ext/Bal/stdbas/WixStandardBootstrapperApplication.cpp#L1101)：使用既有 action 控制項。
- [WiX 6.0.2 RTF Progress 頁](https://github.com/wixtoolset/wix/blob/v6.0.2/src/ext/Bal/stdbas/Resources/RtfTheme.xml#L57)：預設沒有 action 文字控制項。
- [Microsoft Sandbox](https://learn.microsoft.com/en-us/windows/security/threat-protection/windows-sandbox/windows-sandbox-overview)：Windows 11 22H2 起 guest 內重新啟動與關閉的狀態保留不同。
