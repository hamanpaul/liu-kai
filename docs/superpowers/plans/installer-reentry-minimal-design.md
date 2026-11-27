# Burn 安裝器重入最小設計

> **互斥核心與 BAFunctions 封裝已實作；私有修正候選的真實 Reentry 與 3010 後完整狀態續驗通過。** 真 BA 中斷與 embedded rollback restore 不因此算通過。初稿由 MAI Code 2 max 撰寫；Root 後續核對官方來源及實際建置／沙盒證據，取代初稿未核實的假設；不是新一輪 Neon 審查。最新範圍見 [Windows 驗證紀錄](../../../windows/VALIDATION.md)。

**Goal:** 第二個外層客戶端在任何 Burn 登錄交易前，以 `HRESULT_FROM_WIN32(ERROR_INSTALL_ALREADY_RUNNING)`（Win32 1618／`0x80070652`）退出，且不新增 launcher、服務或交易引擎。
**Architecture:** WiX 6.0.2 StdBA 上只加一個 BAFunctions 早期閘門；最小化後以官方 headers 編譯單一 DLL。
**Tech Stack:** WiX/Bal 6.0.2、未修改的官方 header、MinGW x64 C++17、named kernel mutex。只有一個 BAFunctions DLL，不新增 engine extension。

## 已核實的入口與退出路徑

- `WixStandardBootstrapperApplication.cpp:183-206` 的 `OnCreate` 呼叫 `InitializeData`；後者在 `2816-2839` 載入 BAFunctions。`LoadBAFunctions:4711-4769` 將 `BAFunctionsCreate` 的失敗 HRESULT 往上傳。
- `bootstrapperapplication.cpp:127-190` 的 `BootstrapperApplicationStart` 在 `175-176` 取得上述 OnCreate 結果；失敗時不進 Startup。
- `engine.cpp:704-794` 的 `RunApplication` 在 `719-727` 先 Start 成功才設 `fStartupCalled=TRUE`；Start 失敗會走 `778-780` 的 `pfSkipCleanup=TRUE`。因此 busy 必須從 **BAFunctionsCreate 回傳失敗**，不能等到 Plan／Apply 或用晚期取消冒充拒絕。
- 不採用更早的 `BootstrapperExtensionCreate`：它在 `RunApplication` 之前，單純回傳失敗仍可能進 `CoreCleanup`。也不使用自我 ExitProcess、修改 Burn 內部 struct 或額外清理 callback 繞路。
- 官方 `CreateBootstrapperApplicationProcess` 在 `bootstrapperapplication.cpp:449-473` 直接建立 BA 程序；父程序的 `RunNormal` 在 `engine.cpp:568-573` 完成 CoreCleanup 後才卸載資源，EngineRun 稍後才結束提權 companion。尚須以修正版真正沙盒程序觀測補足動態證據。
- 已用現有 MinGW 實際編譯未修改的四個官方 header，x64 create args/results/command 大小分別 32/24/72，無編譯警告。只需 Windows MSI 型別 include 及五個舊 SAL 註記相容別名；別名以 push/pop macro 隔離，不重宣告 ABI、不修改上游 header。

## 根因與更正

控制組 `original-concurrent-incident.json` 明確是 **FAIL**：22:16:28 第一筆 Apply；第二筆雖看到同 Bundle `InProgress`，仍進 Apply，MSI 以 1618 拒絕；22:16:53 `SessionEnd=None` 刪除共用 Burn 登錄。第一筆 MSI 22:18:35 才成功 3010，22:18:36 Full Burn 登錄為 `0x80070002`；最終僅 MSI 存在、Burn 缺失。因此不能把「第二 MSI 1618＋第一 MSI 3010」算成功。
`ordinary-recovery-result.json` 的空參數普通重開 PASS、未重跑 MSI；這不取代 I2 的 BA 故障注入。`control-uninstall-after-verification.json` 的普通卸載完成 0/3010，重啟後 MSI/Burn/COM/owner/產品檔案與待刪均清；原設定及 disabled Doctor 保留；`GetProfile E_FAIL` 只是不存在。原 `0x800700e8` 根因仍未知。
**更正必須保留，不可粉飾：** `apply.cpp:393-418` 的 `WixBurnExecutionLock` 整段在 `#if 0`，官方 Burn 沒有 ApplyLock 可依賴［R1］。先前「已鎖住」說法錯誤，仍見歷史規格 `...-design.md:28`［R2］。

## 唯一最小方案

由 WiX 官方 `BAFunctionsCreate` 建立 `Global\LiuKai-Burn-94C1F608-8C0F-424C-A572-E0F4BCB10327-v1`。名稱不含版本、路徑或 display。以 `initialOwner=FALSE` 建立；`ERROR_ALREADY_EXISTS` 才算 busy，其他 ACL/type/namespace/ABI 錯誤一律 fail-closed。永不 `WaitForSingleObject`，將「name 存在」而非 thread ownership 當鎖；interactive/passive 顯示可讀繁中訊息，quiet 只可靠寫入繁中 Burn log，但同樣回 0x80070652。

第一個 BA 把非 owning handle，以 `DuplicateHandle(..., DUPLICATE_SAME_ACCESS)` 留在 Windows 確認的直接 Burn 父程序；BA 自己保留一份。父程序須仍存活、建立時間不晚於 BA、使用者 SID 相同，且可取得 `PROCESS_DUP_HANDLE` 權限。正常啟動與使用者主動提權啟動都應支援，不強求父程序永遠未提權。這只傳遞 kernel object handle，不寫程序記憶體、不執行遠端程式碼；父子關係由 OS 提供，產品封裝仍以真實 BA hash 驗證。任一步失敗就關閉本次 local handle，從 Create 回傳失敗，沒有 Detect／Plan／Apply。

只允許官方 command 的 `UPGRADE` relationship、`EMBEDDED` display，且 action 為 `UNINSTALL` 或 `INSTALL` 的內部呼叫略過鎖：前者為移除舊 bundle，後者為升級失敗時還原舊 bundle，不能只放行卸載而破壞 rollback。`core.cpp:1838-1844` 設 embedded display，`engine.cpp:647-666` 在 RunNormal 之前完成 Burn parent pipe 連線驗證；外部 `/quiet` 是 NONE，不是 EMBEDDED。其他模式不憑命令列字串或 `fPassthrough` 單獨略過。這些分支仍須修正版實測，不以 enum 判定測試代替。

建置保持 repo 現況：`windows/native/wix/build-installer.ps1` 以 g++ 編譯原生 DLL（46），之後 WiX build（80）；沒有 CMakeLists。固定 WiX 6.0.2 官方 NuGet 所含 header 的 include 路徑，不手抄 `BAFunctions.h`／`BootstrapperApplicationTypes.h`／`IBootstrapperEngine.h` 或 struct；若該 package 缺 header，才用固定 blob 的原檔逐一鎖 hash，禁止整棵 vendoring。ABI `cbSize`／version 必須依官方 header 驗證，不相容即 fail-closed。

## 先驗證的三個停止條件

1. 上述 Create 失敗到 skip-cleanup 的官方呼叫鏈已核實；需用修正版在一般及使用者提權啟動的第二客戶端各觀察零 Apply／零 SessionBegin／零 MSI，不能只驗退出碼。
2. 由實際 process tree、ACL 與 kill 實證「BA dies、parent survives」及 parent 確實活到 elevated engine cleanup；若它先退出且 elevated engine 仍交易，這個方案不是產品級，應停止而非加 wrapper。
3. `BootstrapperApplicationTypes.h:351-371` command 欄位及 embedded 連線路徑已核實；仍須驗證正常跨新鎖版本升級、移除舊 related bundle 與 rollback 還原可用。沒有證據的其他 related 模式不先增加支援。

## TDD 與實機驗收

截至 2026-11-28（UTC+8），`installer-session-guard.hpp` 已完成，PS5／PS7 分別從含空白的工作路徑編譯並執行七項真正多程序案例，皆 exit 0。覆蓋正常／意外 UI helper 退出後仍拒絕重入、keeper 退出後無殘鎖、barrier 競爭僅一勝者、真實 process handle 缺 DUP 權限的失敗清理、同名 Event 型別衝突及 OS 直接父程序驗證。只使用唯一測試物件與自建子程序／job，不涉及真正 installer 或 BA 故障注入。

第一個核心 RED 是缺少產品 header/API 的編譯失敗；沒有將它冒稱行為斷言已執行。另有先前原始安裝包的真實競態 FAIL 作為產品行為反證。PS5 初次 runner 未取得短命程序退出碼，Root 改用持有原始 handle 的 Diagnostics.Process，之後兩種 PowerShell 皆通過；失敗收據保留。

封裝 TDD 也已用真實舊安裝包抽出 BA：修正測試 reader 應遵循 XML BOM／宣告後，PS5／PS7 均精確報 GUARD101「WixBalBAFunctions found 0」。原先 UTF-16 被誤當 UTF-8 的解碼失敗不列為有效產品 RED。之後 MAI 已接入單一 BAFunctions DLL 及兩種 authoring；Root 的兩種封裝與真正沙盒 Reentry／正常 guarded 升級已有 PASS，最新範圍見本文頂端連結。這不取代仍未覆蓋的 BA 中斷與 embedded rollback restore。

先 RED 各判定，再 GREEN：兩程序 barrier 只一方獲准；不同路徑／版本同 UpgradeCode；UI 被殺但 parent 活著仍擋；parent 正常退出或取消後自動可再進；related `UPGRADE+UNINSTALL` 可用；外部 quiet 仍 1618；未知 ABI/size、ACL 或 duplicate 失敗都零 Detect/Plan/Apply。最後必做真 Sandbox，不得以 pure 判定器替代：第二客戶端零 MSI/零 Burn transaction，第一筆完整 MSI＋Burn、COM/owner/payload、設定與 Doctor 皆符合；最後 ordinary uninstall 及 3010 後 reboot 仍完整清理並保留使用者資料。只保證「有新鎖版本彼此排除」；0.1.1 無此鎖、跨舊版兩客戶端缺口不能 retrofit，必須公開。

## 來源索引

- R1 `https://github.com/wixtoolset/wix/blob/v6.0.2/src/burn/engine/apply.cpp#L393-L418`。
- R2 `docs/superpowers/specs/2026-11-27-installer-readings-design.md:28`。
- R3 `windows/build/installer-plan-baseline/BA/manifest.xml:1`。
- R4 `windows/native/tests/wix-installer-scenarios.ps1:1762-1789`。
- R5 固定 blob `wix-sdk-reference/{BAFunctions.h,WixStandardBootstrapperApplication.cpp及官方sample}`；本階段 read tool 未獲該 repo 外目錄權限，故行號未寫、不冒稱已核。
- R6 固定 blob `wix-sdk-reference/{BootstrapperApplicationTypes.h,IBootstrapperEngine.h,engine command source}`；同上。
- 控制證據：`validation/sandbox-resumed-20261127/output/{original-concurrent-incident.json,scenario-original-reentry-6/{first-burn.log,second-burn.log},ordinary-recovery-result.json,control-uninstall-after-verification.json}`；timestamp 取自本任務提供的已完成控制組，本階段未重讀行號。
- 建置：`windows/native/wix/build-installer.ps1:46,80`；bundle identity：`windows/native/wix/bundle.wxs:12-17`。
