# Issue 16：真實 BA 中斷與查核工具補強（2026-11-28）

本輪只在一個 4 GiB Windows Sandbox 串行測試。沒有主機安裝、主機重啟、WSL 操作或 Android 模擬器。產品原始碼及版號未改；正式 baseline 0.1.1 與 candidate 0.1.2 分別核對 SHA-256 `ba32e8e62dd4eddad057ca44218ea96860f94afaec86d026a7471515baef43ae`、`56a986f5332b65bfc9fdfddbcd076c1fbb5835a5831584bee6e1b02964e1f23e`。

- 真正 MSI 提交並回原生 3010 後，僅終止已核對 hash／PID／建立時間／ancestry 的獨立 `wixstdba.exe`，以同一 held native handle 再核對身份後退出 23063。未終止 Burn engine、msiexec 或服務。此輪真正 BA stop 為一次；較早的空日誌參數錯誤及程序樹 lookup 漏報沒有終止 BA，不列為故障注入。
- 原 engine 自行退出 0，實際 Burn 日誌有 `0x800700e8` 回呼錯誤。獨立讀回新版 MSI／Burn 70.1.4、12 個 manifest payload、COM／TSF、owner 及舊登錄清除皆一致；Burn 為 `Installed=1 / Resume=3 (ARP)`。因此**沒有形成 MSI 存在而 Burn 登錄不完整的前提**，原始 Recovery 收據保持 `NOT_COVERED`，沒有改寫為 PASS，也未宣稱歷史事故唯一原因已定位。
- guest 重啟後，以同一 EXE、空參數普通開啟維護介面，選修復；遇真實檔案占用選擇延後替換，成功退出 3010。再次只重啟 guest 並核對完整狀態後，同一 EXE 空參數選解除安裝，成功退出 3010；最後只重啟 guest。
- 最後獨立讀回 MSI／ARP 均為空、COM／profile absence、產品目錄不存在、產品相關 pending deletes 為零、`Program Files` 同層專屬退休 DLL 為零。合成設定、disabled Doctor 的一筆歷史及完整指紋均保留。補強退休檔觀測後另重讀相符；沒有用原本較窄的產品目錄檢查代替同層退休檔檢查。
- PS5／PS7 的 scenario 契約、219 項逐階段重啟契約、22 項 Burn 三態登錄契約、25 項實際退休檔 snapshot 契約、程序樹及 absence snapshot 串接均通過。原生 helper 另在 PATH 不含 g++、只指定絕對 ToolchainBin 時兩端通過，包含持有 handle 的負向身份檢查及一個自有 benign fixture 的終止；沒有把 fixture 終止算成產品 BA 注入。
- post-exit 契約先重現「summary 0 尚未寫 native 3010」的缺口，再修為原 engine 退出後重讀該次明確日誌。最新實作另以不可變的本次真實 MSI／Burn 日誌重放，兩端均讀到恰一筆候選 commit、native 3010 及已知重啟要求。這是唯讀真實日誌重放，不是第二次安裝或故障注入。
- 獨立審查發現並修正退休 DLL 漏報、首次日誌取樣過早及 dot-source 覆寫 ToolchainBin。未經真缺登錄情境 E2E 的自動續跑 runner 已移出交付範圍；純函式 GREEN 沒有冒稱為完整安裝 E2E。

上述證明新版在這次真實 BA 中斷後自行維持完整登錄，且能普通維護與卸載。使用者於 2026-11-28 明確接受此結果作為新版驗收，要求完成 PR 並關閉 [issue 16](https://github.com/hamanpaul/liu-kai/issues/16)。原計畫的「先形成缺登錄再恢復」仍未重現，原始 NOT_COVERED 收據保持不變；後續驗收使用明確 `AcceptRetainedBundle` 模式，全部版本／完整性／原生結果／重啟／清理／保留條件仍須通過。測試入口及證據界線見[恢復測試說明](../docs/windows-installer-recovery.md)。以下保留先前測試的歷史狀態。

## 正式防重入 DLL 補充回歸（先前紀錄）

本輪只新增測試與 CI，產品碼及版本未改。使用者已確認 #18 並要求結案，#19 也依已完成範圍結案；兩單已補上對應 PR，不再把額外資料裁定列為本次門檻。

- 直接載入正式 0.1.2 建置抽出的 `LiuKaiInstallerGuard.dll`（來源 `2d3693111dc20b8d8fadc3f14cee6a43ffb02264`，DLL SHA-256 `55dd006e34a4f506f906edc64dcf7eea62a925e649c44da781803bf2f160800d`），呼叫官方 BAFunctionsCreate／Destroy，沒有以 include 產品 cpp 代替 DLL。
- 單一 4 GiB Windows 沙盒中，PS5／PS7 各通過 19 個呼叫案例及三組 keeper 生命週期。涵蓋 malformed args、callback／Log、合法 embedded UNINSTALL／INSTALL、quiet 與錯 relation/action 的拒絕，以及 BA 正常結束或強制結束後保持 busy、keeper 結束後可重新取得保護。
- 私有 mutation 只移除 embedded INSTALL 例外，測試實際得到 1618／`0x80070652`、缺 callback 與 marker 的語意 RED；正式 DLL GREEN。較早 fixture 的 include／MinGW IID 編譯錯誤另行保留，沒有計為功能 RED。
- 既有 PS5／PS7 各 749 項純情境斷言與七項原生 guard 案例亦通過。CI 改為對每次正式建置的實際 guard DLL 執行同一組新測試，保留失敗診斷。

這些是 DLL 元件及真實 fixture 程序生命週期測試：`installer_runs=0`、`msi_actions=0`。**不能代替真正 MSI 已提交、BA 中斷後普通雙擊恢復的端到端驗證，也未宣稱完整 Burn rollback 或歷史 `0x800700e8` 根因已解決。** [Issue 16](https://github.com/hamanpaul/liu-kai/issues/16) 因此仍保持開啟。沒有主機安裝、主機重啟或 WSL 操作。

# Windows 0.1.2 安裝流程候選驗證（2026-11-28）

本批依使用者指示只在單一 4 GiB Windows Sandbox 驗證，不在本機安裝或重啟，也不啟動 WSL／Android 模擬器。正式來源及成品以本版 Release 附件為準；以下先記錄私有候選的實測，不將私有 SHA 冒稱為合併後成品。

- 原始控制組的真實重入為 FAIL：第二 MSI 雖回 1618，第二 Burn 客戶端仍刪除共用登錄，第一 MSI 成功 3010 後 Burn 登錄不完整。此反例保留，不能只用退出碼判通過。
- 私有修正候選來源 `cbb00831482eb3b38b8bc3f5cb1599c1cda22904`、EXE SHA-256 `4dc4b13dd6fe4ac1385a1a2b0a0c6d644971f532b4bcc9b8707f8c3a1d717005`，產品 0.1.2、MSI／Burn 70.1.4。兩種 authoring 的建置／抽取、guard payload／PE／官方 SDK hash、字形及 11 項 UI mutation 均通過。
- 真實 Reentry repeat：第一 Apply 17:58:35–18:00:43 UTC；第二程序 17:58:36 UTC 回 `-2147023278`／`0x80070652`，完整 log 記錄 early guard busy，零 Detect／Apply／Session／MSI。第一程序真實退出 3010，MSI 提交、Burn Full 登錄完成。先保留 NOT_COVERED 收據；客體重啟後獨立核對 12 個 payload、manifest／owner、HKCU／HKLM COM、MSI／Burn、舊登錄消失、設定／Doctor 指紋及待刪清空，續驗才判 `ACTUAL_SCENARIO_PASS`。
- 相同候選已由同一 EXE `/uninstall /passive /norestart` 完成卸載 3010；guest 重啟後獨立 MSI inventory、HKCU／HKLM 兩視圖 COM／owner、原生 profile/category 狀態、產品目錄與產品待刪均不存在，設定及 disabled Doctor 歷史保留。原生 GetProfile 在不存在時回 E_FAIL，沒有改寫為 0。
- 新候選 Progress 的原生 PrintWindow 繪圖已保存：96 DPI、實際 576×439 視窗，logo、繁中套件名、作業標籤、誠實 fallback、等待說明及取消按鈕可讀，原先方框字形已消失。此時 MSI 尚未送出 action，因此沒有把 fallback 當成已收到 action。較早相同 theme／wxl 的候選另有實際 100%／150% 安裝說明、勾選、Tab 與取消；150% 由桌面及 BA 144 DPI 獨立核對，主機縮放已恢復。
- 新 stock 沙盒沿用預設 EnableLUA=0，沒有變更安全政策；此次完整 Reentry 是既有登入管理員 context。先前普通 token 的 early-rejection preview 有另外的原始證據，但不能稱為完整普通 UAC 升級。檢視器黑畫面限制了互動頁面觀測，不能把靜態 theme／glyph gate 或原生繪圖當成全部互動畫面通過。
- 第一次新候選的收集器因多個絕對路徑被誤合併而失敗，原收據保留為 FAIL。MAI 先補測試、Root 取得 PS5／PS7 RED，再修正 extractor；兩端各 648 項純契約通過後才重跑上述完整案例。純測試不冒稱安裝實測。
- 獨立 adversarial review 另找到 Recovery 卸載 absence 與 native 3010 的判定缺口，按負向 TDD 修正。後續差異必須經 PR CI／GitHub review；八輪 Neon 已用完，不再追加或冒稱 Neon 審過新 guard。

真正非提權 BA 在 MSI commit 後中斷的恢復、歷史 `0x800700e8` 根因，以及全部 UI 頁面仍須分別看其實際證據；沒有把競態事故的普通恢復當成 BA 注入。未覆蓋部分保留在 [安裝議題](https://github.com/hamanpaul/liu-kai/issues/16)。讀音核對維持主要音規則，3,738 筆未知關聯仍待裁定，[資料議題](https://github.com/hamanpaul/liu-kai/issues/19) 保持開啟。

## 先前候選與 0.1.1 歷史

以下保留當時的來源、結果與未完成狀態，並非本批最新驗收清單。

# Windows Doctor 0.1.1 正式成品驗證（2026-11-27）

[正式發行](https://github.com/hamanpaul/liu-kai/releases/tag/windows-v0.1.1)的來源為 `a3f2c65cea6db99e1a4cf3e7e468d6296621296f`，MSI／Burn 70.1.3、repo 批次 0.1.4。EXE SHA-256 為 `ba32e8e62dd4eddad057ca44218ea96860f94afaec86d026a7471515baef43ae`；EXE、SOURCE.txt 與 SHA256SUMS.txt 在公開前後均下載核對。

- 合併前 CI／review 修正完成，正式平台標籤 CI 全綠。Windows 10 SDK 目標明確指定，完整原生回歸與 WiX 封裝使用相同 MSYS2 工具鏈；canonical preflight 使用固定引擎與真正 symlink，沒有豁免規則。
- 同一正式 EXE 在單一沙盒完成安裝（exit 0）、12 個 manifest payload 雜湊核對、設定保留、已註冊 TIP 提交「對嘸」、一般權限下的 Doctor 同意開啟／標記／固定預覽／ZIP 儲存／停用／清除。匯出 21 筆匿名事件的白名單、來源、CRC 及清除 epoch 0→1 相符，另存 ZIP 不變。
- 用同一 EXE 在沙盒卸載（3010），只重啟客體後，程式目錄、實際 MSI／Burn 登記、COM／profile、owner、Run、產品程序與待刪項目皆清理；設定、診斷歷史及另存 ZIP 保留。
- 本機 R70／MSI 70.0.0 升級至本次成品，MSI 成功回傳 3010。12 個 payload、HKLM／HKCU COM 路徑與新版 DLL SHA 相符，官版嘸蝦米登記未變。桌面帳戶升級前及剛升級後均沒有 settings.conf，沒有把此條件冒稱為既有自訂設定的保留實測；既有設定的候選升級證據另列下方。
- 本機 Search／Store 實際載入新 DLL，使用者回報「對／嘸」與中英切換正常。Store 另以逐次原生按鍵及畫面核對中文提交、左 Shift 轉英文、右 Shift 返回中文；兩個宿主的 Doctor mapping 皆核對 PID／建立時間，實際保存一般欄位輸入事件鏈，drop／queue 為 0。
- 全新 Terminal 程序內的 Windows／WSL 分頁分別取得限定字串收據：Windows 為「對」、「嘸」、11 字母英文，WSL 為「對嘸」、11 字母英文。快速同顆／左右交替 Shift、焦點往返由使用者實體操作並回覆正常，不冒稱為自動量測按鍵次數。驗證後只清理自建測試視窗與子程序，保留使用者安裝及其主動啟用的 Doctor，沒有執行 WSL shutdown 或重開主機。
- Terminal 1.25.3131.0 預設未提供欄位種類，實測 Unknown／逐鍵抑制、共享佇列有效且無 drop。輸入正常不等於具備輸入階段診斷；相容設定另依下方候選及 [README](README.md#terminal-的欄位資訊) 的範圍記錄。

實體裝置及沙盒證據分開保留，原始診斷沒有公開。原新視窗曾共用 R70 程序而不列驗收；另一次測試啟動器的 30 秒逾時曾關閉自建測試視窗，修正觀察工具後才取得上述收據，沒有將其寫為產品故障或通過。八小時長跑、斷電及所有硬體／宿主組合未宣稱完成；舊效能反例保留。

## Task I3：Windows 安裝器 UI（2026-11-27，Root 已驗封裝與 DPI；產品情境進行中）

- Root 於 09:47:14 UTC 以正式 `0.1.1` EXE 的真實抽出 BA 取得已驗證 feature RED：exit 1、`UI020`、Progress 缺少 `ExecuteProgressActionDataText`。來源 BA 穩定，先前 launcher timeout 與 XML 回傳型別錯誤不是 feature RED。
- 現已從該 WiX 6.0.2 RTF theme/localization 最小衍生 `windows/native/wix/ui/rtf-theme.xml` 與 `zh-TW.wxl`；正式及開發 bundle 共用同一對資源。Progress 顯示原生 action，Failure 保留真實錯誤文字，1618 指引是靜態說明，UI 指引本身不使用自訂 error-code variable 或 BAFunctions callback；後續新增的 BAFunctions 僅負責下一節的前置互斥。
- Root 於 10:06 UTC 以舊契約已通過的 `ui-source-positive` 取得第二個 feature RED `UI037`：Success close 在不需重啟時仍提示重啟，且正常 uninstall action 4 漏用移除重啟文案。現已把 close 改為無條件「關閉」加 `WixStdBARestartRequired` 的短字串「稍後重啟」，卸載條件同時覆蓋 action 3/4；測試契約未放寬。
- Root 於 10:41 UTC 在同一 owned Sandbox 取得已驗證 `FONT001`：四個 theme Font face 都是 Segoe UI，樣本「嘸改安裝取消」各漏 6/6 glyph；RichEdit notice 與 OS dialog 正常，preview 只確認後取消、未 Apply。獨立 GDI probe 已確認 Microsoft JhengHei UI 支援全部六字，因此只將四個 face 改為該字型，height/weight/layout/controls/localization/transaction 不變。
- `rtf-theme.xml` 與 `zh-TW.wxl` 的 upstream 來源為 [WiX 6.0.2 WixStdBA](https://github.com/wixtoolset/wix/tree/v6.0.2/src/ext/Bal/wixstdba)；相鄰 `ui/LICENSE.TXT` 收錄官方 [WiX 6.0.2 MS-RL](https://github.com/wixtoolset/wix/blob/v6.0.2/LICENSE.TXT)。兩份 bundle 都把該檔以 `wix-theme-license.txt` 嵌入 BA，build/verifier 以 SHA-256 綁定。
- `verify-bundle.ps1` 以 SHA-256 綁定 source/embedded theme 與 wxl，並重用 `wix-installer-ui-tests.ps1` 解析真實抽出資源。UI 本身不改 MSI／TSF 註冊動作；後續因真實重入失敗新增 BAFunctions 前置互斥，範圍及目前證據見下一節。
- 新增 `wix-installer-ui-build-tests.ps1` 與 workflow step：先對 source theme 執行 font glyph test，再讀 MSI ProductVersion 建立另一個實際 development bundle，抽取 migration/development BA，驗證共同 source、license hash、11 個 mutation，且不安裝 fixture。CI 只追加在 platform build 之後，未改資料 HEAD-blob guard。
- MAI 實作階段未自行執行 build/install；Root 已獨立完成兩種真實 authoring 的 build/extract、11 項 mutation、CJK glyph gate，以及同一沙盒 100%／150% 安裝說明、同意勾選、Tab 導覽與取消。150% 以桌面及 BA 視窗皆為 144 DPI 核對，保存原始 JPG；測後主機兩顯示器皆還原 96 DPI。未用圖片放大或登錄設定值代替實際 DPI。
- `wix-installer-native-capture-tests.ps1` 與既有純情境契約均納入 Windows CI。前者實際驗證原生 exit 0/23、空輸出字串、仍開啟 writer 的 live-log 共享讀取、唯讀 MSI 清單，以及含空白 DLL／輸出路徑的兩秒有界模組；後者經第一筆最終狀態的負向 TDD 補強後，PS5／PS7 各通過 505 項純合成斷言。MSI 清單採正式唯讀 [Installer.RelatedProducts](https://learn.microsoft.com/en-us/windows/win32/msi/installer-relatedproducts) 及 indexed ProductInfo，不使用會觸發修復的 Win32_Product。
- Root 的正式基準快照已核對 12 個 payload、owner／manifest、COM、MSI／Burn 與合成設定；後續控制組已重現重入失敗，普通同 EXE 恢復與卸載／重啟後核對通過，詳見下一節。原子收據、空串流／exit code、路徑引號與 COM 存取的工具錯誤均保留原始 RED；前置工具失敗不列為產品故障，也未計入 BA 注入次數。尚不宣稱 issue 16 完成。

## Task I1/I2：實際重入缺陷與前置防護（2026-11-28，成品實測待完成）

- 原始 0.1.2 控制組第一筆 MSI 執行期間，第二客戶端雖收到 MSI 1618，仍在 SessionEnd None 刪除共同 Burn 登錄；第一筆 MSI 提交 3010，隨後 Full 登錄失敗 0x80070002。guest 重啟後僅 MSI 存在，故 Reentry 判 FAIL。WiX 6.0.2 ApplyLock 的 mutex 程式實際在 `#if 0` 停用，先前「Burn 已提供互斥」的假設已更正，沒有將本例等同歷史 0x800700e8 根因。
- 普通空參數雙擊同一控制組 EXE 已恢復 Burn 登錄，未重跑 MSI，12 個 payload／COM／owner／版本及合成設定／Doctor 狀態相符。同 EXE 維護介面正常卸載，保留占用程式並稍後 guest 重啟後，MSI/Burn/COM/owner、產品目錄及產品待刪項目皆不存在，設定及 Doctor 歷史保留。GetProfile E_FAIL 是 profile 不存在的原始結果，未冒稱全部查詢回 0；空 TIP 根鍵也未要求消失。這些是控制組證據，不替代真正 BA 故障注入，該次數仍為 0。
- 新增最小官方 BAFunctions Create 閘門，將非 owning named mutex 的一份 handle 留在 OS 核對的直接 Burn 父程序。早期拒絕返回失敗會走 Burn 尚未 Startup 的 skip-cleanup 路徑；不使用自我 ExitProcess、自製 launcher／交易引擎、服務、lockfile 或遠端程式碼。合法 embedded upgrade 的移除與還原保留；quiet 本身不略過。
- 七項真正多程序核心案例已在 PS5／PS7、含空白的工作路徑通過，包括 UI helper 結束後仍 busy、keeper 結束後無殘鎖、barrier 競爭、DuplicateHandle 權限失敗清理及同名 Event 型別衝突。只控制測試建立的子程序與 job，不是產品安裝或真正 BA 注入。
- 真實舊 BA 的封裝測試已在兩種 PowerShell 取得 GUARD101（找不到 BAFunctions 註冊）的有效 RED。更早將 UTF-16 XML 誤當 UTF-8 的 reader 失敗保留原收據，改用遵循 BOM／宣告、禁止 DTD／外部 resolver 的 XML reader 後才取得有效 RED。
- 新擴充已實際編譯，PE 有兩個官方入口，僅相依 ADVAPI32、KERNEL32、msvcrt、USER32。先前 linker 參數解析與額外 libwinpthread 依賴均保留失敗紀錄，已按既有安裝動作採完整靜態連結及 Windows 10 target；沒有把新擴充的建置缺陷當作舊版故障原因。
- 尚待新的兩種真封裝、一般／提權重入、正常升級／embedded restore、取消、真正 BA 中斷、進度／占用／失敗／3010／卸載 UI 及註冊輸入驗證，完成前沒有交付 commit 或 release。八輪 Neon 已用完，後續核心與包裝差異沒有冒稱經過 Neon 審查。具體設計與限制見 [防重入設計](../docs/superpowers/plans/installer-reentry-minimal-design.md)。

## Windows Doctor 候選歷史（2026-11-27，發版前紀錄）

以下為發版前候選的原始範圍與失敗紀錄。後續正式成品及本機矩陣的完成情形以上節為準，不改寫候選的來源、雜湊或當時尚未驗收的狀態。

目前測試包的執行版本為 `0.1.1-doctor-p0`、MSI `70.1.3`，EXE SHA-256 為 `021a4f295c0aa22dd351ed8e96f59d8a338151aa92049539227bc5df98cd636f`。它是私有驗證產物，尚非正式發行版。

- 最新來源的 `native/test-release.ps1 -DoctorEphemeralTestUser` 在單一沙盒以一般測試帳戶通過，包含19項host、14項真HWND設定測試及完整原生發行檢查。延後讀取named-pipe回覆的真實錯誤233已走RED→GREEN；無讀取客戶端仍須有界結束。
- 符號連結條件另有強制入口：完整 Doctor 隔離帳戶流程結束後，由具權限的父程序執行 `doctor_store_tests <repo-root> clear-staging-symlink`。它必須成功建立真實連結，再以 Win32 reparse tag 核對清除前後仍為同一類型的符號連結，且外部與無關檔案不變；無法建立連結會失敗。一般帳戶的完整 suite 若缺少此權限，會明確輸出 `SKIP`，不能把它當成已覆蓋。此測試不依賴部分 MinGW 版本會誤報的 `std::filesystem::is_symlink()`。
- 最新包在同一沙盒完成舊包卸載、重新安裝、已安裝檔案／註冊核對、Doctor歷史保留及登入後collector啟動。兩個私有候選MSI同為70.1.3，這次是卸載後重裝；較早候選另有正式0.1.0升級的證據。最新包最後卸載的Burn與MSI均回傳3010，僅重啟同一沙盒後，程式目錄、MSI／Burn登記、COM／profile、Run、owner及產品待刪項目均清除。獨立核對114項封存檔、16段舊紀錄前綴及52bytes同意控制檔相符；沙盒已關閉，主機安裝未變更。
- 合成TSF／WM控制可輸出完整中文及英文，並核對Shift切換、匿名事件與提交計數；這不代表實體鍵盤或所有應用程式均已驗證。已完成受控ordinary／private切換、真實AppContainer、跨帳戶拒絕及collector重連；沙盒缺少的Search／Store／WSL仍列未驗。Terminal預設未提供欄位種類及選用相容設定的範圍見 [README](README.md#terminal-的欄位資訊)。
- 已完成64分22秒、93輪穩定測試，每輪257提交／3089事件、drop0／queue0，32／64MiB限值與retention gap逐輪對帳。依使用者新範圍停止原八小時run，原計時與終止碼保留；八小時記憶體長跑另作非阻擋觀測。
- 真collector中斷後正常Status自動恢復，原宿主續收完整工作量；第二個正常設定視窗清除使epoch1→2，第一視窗嘗試儲存舊預覽回報期次已變更且未產生ZIP，新epoch完整輸入亦通過。清除後temp查無檔，但沒有同一temp檔名的清除前清單，不宣稱逐檔前後比對。
- 最後預定三對的功能、事件及提交計數均通過，但後兩對KeyDown p99增加52.975／22.482ms，超過同輪OFF跨輪3.426ms，效能驗收為失敗。背景CPU不同限制因果判定，不能據此豁免退化。既有匿名階段與固定按鍵位置已縮小同步edit的高成本範圍，尚未定位唯一函式。
- 與WSL Thala協調後，較低背景負載的三對p99差為+2.170／+1.633／+6.661ms，OFF跨輪差3.996ms，仍未通過效能判定。單次collector優先權NORMAL／BELOW_NORMAL／NORMAL對照未見中段專屬改善，已還原優先權；所有Thala保留均已釋放並獲確認。
- 2026-11-27 在背景負載較低、單一沙盒的同一原版宿主完成新三對 OFF／ON。使用原未增加 QPC metadata 的 `775c50a7…` 測試工具，每輪 775 對按鍵、260 字、2322 筆樣本；ON 各 3089 筆事件，drop／queue 均 0，OFF 沒有逐鍵事件。770 筆 KeyDown 的 p99 OFF／ON 分別為 7001.1／6961.7、6719.2／6612.4、6454.3／6790.2 µs，配對差 −39.4／−106.8／+335.9 µs；各小於原初始 500 µs 目標，OFF 跨輪幅度 546.8 µs。Root 由原 CSV、結果、queue 與 canonical 紀錄獨立重算相符。主機 CPU 逐輪平均約 4.57–7.29%；本結果僅支持此次受控環境，不覆寫前述高負載失敗，也不代表所有負載均達標。
- 另以增加 QPC boundary metadata 的工具取得一對診斷資料及 ETL，並不混入上述原版三對。typed SDK 讀出的 clock 為 QPC 10 MHz，所有 loss 計數為 0；目標 thread／process 生命週期包住測量區間，165 次同 TID sleep 控制均有 switch-out／in。控制執行緒後來被另一程序重用，已依互不重疊的真實生命週期分開，原保守分析失敗仍保留。產品程序的 trace rundown 只當 capture 邊界，不冒稱真實出生或死亡；控制程序沒有捕捉 FILETIME，以 ETW ProcessStart／Stop QPC 識別。
- 此工作量的 QPC 包住 `ITfKeystrokeMgr::KeyDown` 同步呼叫，包含 TSF 分派、同步處理及呼叫期間的排程延遲；不含呼叫前的 WM_TIMER 等待或呼叫後的 fallback 訊息分派。每輪各求 p99 再相減，不是逐事件差的 p99。ETL 的 guest scheduled 區段可包含主機對整個 VM 的搶占，未排程區段也不能直接區分等待與排程延遲；不同分布的 p99 不能相加，不據此推定舊退化的唯一根因。
- 正式來源與下載包、宿主矩陣、CI／review 仍須完成；不把一小時結果寫成八小時通過。Search、Store、WSL、實體鍵盤及真實 Shift 長跑尚未驗證；真機安裝提案仍待使用者確認。

本輪交付狀態見 [Windows 發行驗證計畫](../docs/superpowers/plans/2026-11-27-doctor-windows-release.md)；完整設計保留在 [Doctor 實作計畫](../docs/superpowers/plans/2026-11-25-doctor-mode.md)。

# Windows x64 單次注音查字 70.1.1（2026-11-25）

測試安裝檔為 `LiuKai-win-setup.exe`，SHA-256 為 `bd71002eb44a7aefeec31c0ff03fa77d59489ebee29860f71f90920b33937fd2`。

- 在本機 Windows Sandbox（組建 26100）安裝 GitHub 的 R70，再用這份 EXE 升級。核對 11 個已安裝檔案、輸入法註冊路徑，以及 WPF 文字框實際載入的 DLL 雜湊。升級與卸載均回傳 3010（成功，Windows 要求重新啟動）。卸載後確認產品登錄項目與原位置的 DLL 均已移除。
- 在一般 WPF 文字框依序按 `'`、`;`、`vu8`、空白，首兩個候選為「瞎 MNFO」、「蝦 COFU」。按下鍵、Enter 選「蝦」，接著輸入 `ci`、空白，得到「蝦中」。再次查詢時按 `0` 得到「瞎」。二聲、三聲、四聲及輕聲分別選出「麻」、「螞」、「罵」及「嗎」。
- 在「ㄧˊ」的候選清單按 Backspace 返回原讀音，再按住空白重新確認，第一頁停在「疑」。放開後重新按住空白可連續翻頁。另驗證長按聲調鍵、Esc 取消、無結果時按 Enter、原字碼同音查詢、Shift 切換中英，以及密碼框接收原始鍵序。
- 執行 `native/test-release.ps1 -ToolchainBin <工具鏈目錄> -WorkDir <新的測試目錄>` 通過。此入口涵蓋引擎、Windows 文字服務按鍵與既有安裝流程的回歸測試。另載入封裝的 DLL 確認可提交兩個中文字及 599 個英文字母，並核對 MSI 的 25 個檔案、內嵌 MSI、安裝圖示及說明。

# R60 驗收範圍

日期：2026-10-26  
版本：`0.0.0-dev.20261026.r60.machine-com-x64`  
發行定位：Windows x64 prerelease，tag `r60`。

使用者在已安裝的 R60 上確認「三項都正常」：

- Windows Search：`a + Space`、`oek + Space` 中文輸入正常。
- Microsoft Store：`a + Space`、`oek + Space` 中文輸入正常。
- 工作列獨立中／英模式圖示正常。

安裝後 12 個套件檔案的 SHA-256 核對通過。全新獨立程序確認 HKCU64／HKLM64 均指向 R60 DLL，機器層級的 owner、安裝帳號 SID、安裝根目錄及 64 位元視圖符合預期；registered、immersive、input_mode、system_tray 與 user_enabled 讀回通過。這些讀回與上述使用者實測分開記錄。

Terminal 沿用先前正常結果，本輪未重新測試。不宣稱完整 77 項 backlog、官版 1:1 全功能或所有宿主情境均已驗收。公開來源匯出副本未另跑完整 clean-checkout build／test。

已驗安裝檔：`WuGai-R60-x64-Setup.exe`  
SHA-256：`b65fa61ff1fc26580c603dd82093c0081d3d097c06a0ccdad8d0c4803fd3bda4`
# R60.1 安裝器修正驗證（2026-10-26）

R60 在新註冊的 TSF Profile 沒有 `HiddenInSettingUI`、`SubItemInSettingUI` 時，讀圖示路徑會連帶要求完整 metadata 而失敗。修正版只讀取 HKLM64 的 `IconFile`；完整顯示快照、所有權檢查及既有交易格式維持原有規則。

- 同一個受影響 Profile 的唯讀對照：R60 原始 `WindowsTsfSharedApi::profile_icon_path` 回傳 `2147942402`（`0x80070002`），修正版回傳 `0` 並取得原圖示路徑。沒有補寫缺省欄位。
- `profile_icon_readback_tests.cpp`：缺少 settings metadata、無關欄位型別錯誤仍可讀取圖示；缺少、空白、型別錯誤及內嵌 NUL 的 `IconFile` 均拒絕。測試只建立及清除獨立 HKCU 測試鍵。
- 既有 command-line、dual-architecture lifecycle、production status report、durable singleton recovery 測試通過。
- 安裝腳本讀回測試 18 項、套件安裝入口測試 21 項通過。
- R60.1 release bundle 的 16 個檔案與 manifest 一致，且包含新編譯的 CLI；安裝計畫檢查通過。

本輪尚未宣告乾淨 Windows / 新 SID 的端到端安裝驗收，也沒有重新驗收 Search、Store 或中英圖示。R60.1 只重建安裝 CLI 與 Setup 外殼，其餘已校驗 payload 沿用 R60。

使用 `native/build-release.ps1 -SourceBundle <解壓後的 R60 bundle> -InstallerCLI <修正版 CLI> -Version <新版本> -OutputDir <不存在的輸出目錄>` 可重新封裝。輸出包含安裝 EXE、release bundle 及 SHA256SUMS.txt；不會發布或安裝。

# R60.1 跨電腦失敗追查（2026-10-26）

使用者確認另一台執行 R60.1，提供的 `20261026T133315Z-a966877d91f041c1a7941ecb26552d44.log`
記錄 operation=`0x8007000D`（ERROR_INVALID_DATA）、cleanup=`0x8007051A`（ERROR_REVISION_MISMATCH）。
最終所有資源 absent 且 readback=0；摘要沒有第一個失敗階段，不能據此宣稱端到端安裝已修復。

已重現獨立的回復證據驗證缺陷：shared resource 的 apply observation 使用投影後的狀態，
restore 使用完整原始狀態，原本卻直接比較兩者雜湊。修正將 apply observation 與同格式的
prestate 比較；跨格式的 restore 仍要求有效的 before hash、精確的原始 snapshot 目標及讀回。
執行時 rollback 的狀態比對保護沒有放寬。此缺陷可造成 cleanup 錯誤，尚未證明是他機的完整原因。

另外補強 Setup 持久 LOG（版本、SHA-256、OS build、階段、exception、exit code）、
套件部署預設 diagnostic trace，以及 profile 註冊／啟用／圖示／最終驗證的分段 trace。
trace 不增加 TSF API 呼叫，也不更改啟用語意。新增唯讀紀錄收集腳本，操作方式見
`native/INSTALL-DIAGNOSTICS.md`；本機實測可讀取原始 journal / snapshot 並輸出 ZIP。

驗證包含 production projection 的正負案例、profile trace 與原 API 呼叫一致性、既有 status report、
Setup log helper 及 package wrapper 測試。整合 CLI 已編譯。這些驗證沒有執行真機安裝，
也沒有處理原本未完成的交易。沒有重新封裝或發布新版本。

仍需另一台的 `transaction.journal` / `transaction.snapshot`，才能判定最初的 ERROR_INVALID_DATA。
R60.1 安裝檔目前無 Authenticode 簽章；Google Drive 警告未取得完整文字，不能認定為誤判，
也不能將未簽章直接視為 Google 的偵測原因。

# R60.2 卸載入口與診斷（2026-10-28）

新增 Setup `--uninstall`、安裝後的 `Uninstall.exe` 及 Windows 應用程式解除安裝入口。
只有部署與獨立註冊讀回成功才發布入口；維護檔置於安裝目錄旁的 `.maintenance`，
不改變原套件 manifest 或受管理根目錄的固定檔案規則。安裝與卸載使用同一外層 mutex，
卸載器先交接到暫存副本，避免自己的執行檔阻擋刪除。

卸載先嚴格驗證整棵安裝目錄、所有版本、帳號、所有權及交易歷史，保存可核對的完整備份，
再移除 modern-host 類別及既有七個 lifecycle resources。所有類別、原始 Profile 登錄葉節點、
API 與 COM 都必須在獨立程序讀回為 absent，才允許刪除程式檔案。modern-host 類別先行清理
不屬於七個資源的原子回復；若後續失敗，會如實保留卸載進度、套件與診斷資料。

- Native package fixture 通過：完整 inventory、多版本、未知檔案／類別、外來及借用註冊、
  SID／路徑不符、未完成 journal 拒絕；部分類別失敗、七資源失敗回復後重試，以及
  成功卸載、刪除 fixture 根目錄後保留原 ledger 再安裝。
- Profile 殘留清理、parser、status report、Windows backend 所有權測試通過；登錄測試
  限制於隔離的測試鍵，未安裝或卸載本機真實輸入法。
- PowerShell 卸載 proof、完整 inventory 及檔案復原測試 67 項通過；安裝讀回測試 20 項通過。
  一般刪檔例外會從已驗證備份恢復可再次卸載的檔案。程式在檔案清理階段遭強制終止或斷電時，
  尚不支援自動跨程序還原，應保留備份與 LOG 進行人工復原。
- 維護入口測試 114 項、維護入口中斷／回復測試 53 項、Setup 參數及交接測試 51 項通過。
  這些使用隔離的檔案 fixture 和模擬 Apps 登錄，沒有啟動真正的安裝或卸載程序。
- 此輪新 CLI 對使用者已清理電腦的唯讀檢查：七資源 absent、all_categories_count=0、
  profile_registry_absence_result=0、operation/cleanup/readback=0。

本機候選安裝檔 `WuGai-R60.2-x64-Setup.exe` 的 SHA-256 為
`23f3f0a715d2d550826e3d14c078e3aaa37603105acb6071d0c625d882ded2f4`。
已核對內嵌 ZIP 與 release manifest 的 18 個檔案；Windows 讀取簽章結果為 `NotSigned`。
候選檔尚未發布至 GitHub。

使用者設定、備份及歷史交易保留。舊失敗交易沒有被刪除、改寫或假造成功 receipt；
未解決的歷史交易仍可能阻止該目錄的後續安裝，需依原 journal / snapshot 處理。
此候選版不是另一台 `0x8007000D` 已修復的證明，也尚未通過乾淨 Windows／新 SID 的
真實安裝、卸載及重裝驗收。尚無 Authenticode 簽章，不能宣稱 Google Drive 警示已消除。

# 本機真實安裝追查（2026-10-28）

在確認本機程式目錄與所有註冊已不存在、核對原 journal / snapshot 與備份的雜湊後，
將原失敗交易原樣封存到同一 ledger namespace 的 `manual-reconciliation`，另寫明確的
人工處置紀錄。此處置不是 compensation 成功證明；原 journal 沒有改寫。

- R60.2 實機部署在建立新交易之前失敗。管理員探針顯示 HKCU 的 Classes 可寫，
  HKLM 的 `Software\Classes` 以寫入權限開啟回傳 5，`Software\Classes\CLSID` 可寫。
  已把機器 COM 開啟範圍縮小到 CLSID 父鍵，隔離登錄的建立、讀取和移除測試通過，
  並保留相鄰 CLSID。
- R60.3 / R60.4 實機記錄在鍵盤類別 `RegisterCategory` 回傳 0 之後，同程序的
  類別列舉仍顯示 absent，導致 `0x8007000D`。新的獨立程序後來讀到該關係存在；
  已依特定失敗交易證據只移除該單一殘留。這是此電腦的直接證據，不足以推論另一台的主因。
- R60.5 類別獨立讀回後越過上述錯誤；後續 Profile capability 在不同時間點顯示
  `S_FALSE` 或尚缺必需位元。R60.9 為相關查詢加入有限重試，仍要求實際讀回能力位元；
  子程序讀回及能力稍後出現／超時的隔離測試通過，尚未完成新的實機驗收。
- R60.8 安裝失敗後，註冊已讀回 absent，但檔案 rollback 回傳 `0x80070005`，
  留下一筆未完成交易與部分檔案。`explorer.exe`、`ChatGPT.exe` 曾載入該版本的 DLL；
  這些檔案與 journal 保留，未強行關閉程式。受限復原腳本須確認載入者已釋放 DLL、
  精確檔案清單與雜湊、所有註冊及使用者帳號，才備份檔案並封存原交易。
- 本機候選 `WuGai-R60.9-x64-Setup.exe` SHA-256：
  `50c980222f05f185c1bb979c0d13bd026eff67d7f7e29ce1384825354982dd50`。
  18 個內嵌檔案與 manifest 已核對；CLI 參數、獨立 status report、durable singleton
  復原與套件卸載／重裝 fixture 回歸通過。尚未完成真實安裝或卸載測試，也未發布。

Google 通知確認 R60.1 被依惡意軟體或類似惡意內容政策限制，但未提供偵測名稱或命中原因。
Drive API 拒絕下載該受限制原檔；它與本機 R60.1 檔名及大小相同，但無法據此證明雜湊相同。
Microsoft Defender 對本機 R60.1 安裝檔和解包內容，以及後續候選安裝檔的自訂掃描沒有發現威脅；
這不證明 Google 的判定錯誤，也不證明檔案絕對安全。安裝檔未簽署，申請 Google 人工複查
仍需要檔案擁有者執行。

# Google 政策對照（2026-10-28）

Google Drive 的[惡意軟體和類似惡意內容政策](https://support.google.com/docs/answer/148505?hl=zh-Hant#malware)
禁止惡意程式、損害裝置或網路的內容及未經同意安裝軟體。僅依現有程式碼與本機檔案的檢查，
未發現可確定符合這些條件的惡意行為；這不是完整安全審計，也不能證明 Google 的判定有誤。

比對 Google 另行公布的[網站下載及不受歡迎軟體指引](https://developers.google.com/search/docs/monitor-debug/security/malware)，
R60.1 存在應改善的透明度：一般安裝在 UAC 授權後直接開始，沒有安裝前畫面清楚說明輸入法、
程式檔案及系統註冊變更；R60.1 也沒有隨套件提供易找的 Windows 解除安裝入口。
新版原始碼已加入解除安裝入口，但完整實機安裝／卸載驗收仍未完成；安裝前說明與明確同意
尚待補上。上述指引和 Drive 的處置並非同一份裁決，不能據此推斷 Google 的具體命中原因。

R60.1 的 Setup 會將內嵌 payload 展開至 `%TEMP%\LiuKai-Setup-<job>`，目前查閱的程式碼
沒有在結束時移除該解壓目錄；新版 Setup 也保留此行為，且安裝後的解除安裝入口會另建立
`%TEMP%\LiuKai-Uninstall-<job>\Uninstall.exe` 暫存工作程式，目前沒有看到其完成後清理。
因此「反安裝入口已加入」不等於所有程式元件已從暫存位置移除；這是應修正並實測的具體缺口，
但仍不能認定是 Google 此次判定的原因。使用者設定、備份與診斷紀錄是否保留須另外清楚告知。

R60.1 的安裝外殼會自我提權、解壓內嵌 ZIP，並以隱藏視窗執行 PowerShell，使用該子程序的
`ExecutionPolicy Bypass`；這些是實際行為，但本身不能證明是惡意程式或 Google 的觸發原因。
程式碼所採用的 TSF in-process COM 輸入法架構可能由 Windows 載入至其他應用程式，屬
[Microsoft 文件描述的正常整合方式](https://learn.microsoft.com/en-us/windows/apps/develop/input/input-method-editor-requirements)。
來源靜態搜尋未發現列舉的常見網路上傳 API，R60.1 的原生二進位靜態匯入表也未列出常見網路 DLL；
這不能排除動態載入或其他執行行為。來源中的可選執行期診斷紀錄採數值欄位，不包含按鍵文字。

Google 的[不受歡迎軟體指引](https://developers.google.com/search/docs/monitor-debug/security/malware)
明確指出未簽章本身不是標記為不受歡迎軟體的理由，同時仍建議使用可驗證的程式碼簽章。
該陳述不能推論 Drive 的惡意軟體偵測細節。需讓 Google 檢視被限制的確切檔案並提供更多資訊，
才有機會確認實際偵測原因。


## 2026-10-28: local setup and uninstall follow-up

- The old R60.8 partial files were removed after confirming the DLL was no longer mapped. The original failed transaction was archived unchanged, with a verified backup.
- R60.11 installed and passed independent installed-state checks. Its actual installed uninstaller removed registrations but failed after a stale same-process profile read; payload and worker temporary cleanup both logged completion. A fresh retry completed removal and verified absence.
- R60.12 changed profile readback globally to a fresh process. The actual installation exposed a transient false-absence immediately after registration, leaving an orphan profile and failed transaction. The profile was subsequently removed; a fresh status shows all seven registrations absent and profile registry absence. The failed transaction remains active pending elevated archival: the Windows UAC request was cancelled. Do not retry installation until history is resolved.
- R60.13 narrows fresh profile readback to after a successful profile unregistration; its candidate installer has not completed a real installation/uninstallation cycle. SHA-256: 87d9bf063907062d27d7f580526ccaf13a603d8f47dc523bc9e15f1ca4548898. Do not treat it as validated or claim it resolves Google Drive's malware verdict.
- Setup confirmation, extraction cleanup, worker cleanup and Windows PowerShell module-path handling are implemented. The Setup unit test passed 56 checks, including execution of a temporary worker cleanup fixture. The actual confirmation UI has not been interactively verified.


## 2026-10-28: ambiguous TSF readback follow-up

The R60.12 real-machine log showed a successful profile registration followed by a transient absent read. A later independent read found the profile present. An absent read immediately after a successful write is therefore not sufficient proof of rollback. When a successfully created profile or category remains pending, the shared store now returns ERROR_RETRY for an absent observation instead of treating it as a restored prestate. Bounded post-write readback retries use observed values and report ERROR_RETRY when the requested state is not observed; retries do not guarantee future state and do not manufacture an observation.

Isolated tests cover delayed profile visibility, refusing to report restoration while creation remains pending, recovery once the profile becomes observable, delayed and timed-out presence reads, and an end-to-end simulated transaction with an unreadable possibly mutated profile. The latter preserves its installed fixture files and original journal and rejects subsequent machine-scope installation history inspection. Package-uninstall and durable singleton regression fixtures also passed. These tests do not prove cross-process automatic recovery for every interruption and do not exercise real TSF mutation.

On this computer, a fresh read-only status observed all seven product registrations absent and profile registry absence; the prior failed transaction still causes history inspection to return ERROR_INVALID_STATE. No new installation or uninstallation was performed in this follow-up. The current source has not been packaged or validated in Windows Search or Microsoft Store. The previous R60 Search/Store user verification is historical and does not establish current-version behavior.

Candidate UI follow-up: GetPageIndex now supports the documented count-only call and reports S_OK when the provided array exactly fits; an undersized array is rejected without a partial write. Empty candidate selection returns S_FALSE. Isolated COM-object contract tests and compilation of the x64 service DLL passed. This is a contract correction, not evidence that it caused the Search/Store issue, and the rebuilt DLL has not been installed or tested in either application. Reference: https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcandidatelistuielement-getpageindex and https://learn.microsoft.com/en-us/windows/win32/tsf/uiless-mode-overview.

Failed-registration uncertainty follow-up: a registration API failure is now retained as unknown, rather than adopting an unmarked profile/category or declaring rollback complete; pending profile icon ownership must match the expected path. Simulated profile/category failures after mutation and a foreign-icon collision passed. PowerShell 5 and PowerShell 7 maintenance-recovery fixtures each passed 53 checks; PowerShell 7 JSON integers are normalized only when reading publication objects, while live-registry QWORD values remain rejected.

R60.14 candidate (not installed): release manifest and all 18 payload files, embedded ZIP, current scripts, lifecycle CLI and freshly compiled TSF DLL were hash-verified. Setup SHA-256 bfddc9f9d9099dc1e8bd13ce4b25610ad3b0ed58a92314f2ab568ef99a4b6512. Microsoft Defender custom scan with remediation disabled returned no threats; this does not determine a Google Drive verdict. A renewed UAC request to archive the exact manually reconciled R60.12 transaction returned Windows' cancellation error, so the archive operation did not run and real install/uninstall and Search/Store tests remain pending.


## 2026-10-28: mapped-DLL uninstall preflight

An exclusive file open is insufficient to establish that a Windows image is not mapped: the R60.8 failure supplied direct evidence of that gap. Before changing registrations, the uninstall wrapper now queries processes that tasklist reports loading BoshiamyTsf.dll, compares their actual module paths with all installed package DLL paths, and stops if a match or inaccessible process is found. The process inventory is only a point-in-time check; another process could load the DLL afterwards, so the existing backup and failure restoration remain necessary. This guard reports process names/PIDs in the log and does not close programs.

Isolated PowerShell tests passed 77 checks, including different-path DLLs, inaccessible processes, invalid process IDs and missing module data. A separate bounded test process actually loaded the candidate DLL by path, and the production helper identified that exact process and module without registering an input method. Real package uninstallation was not performed. R60.15 includes the guard and previous fixes; its embedded ZIP and 18 release payloads match their manifests and compiled artifacts. SHA-256: 62a08500442271972bcd420ef46185c6b998804b42cbb76f13eb721c113a6d70. Microsoft Defender reported no threats for a custom scan with remediation disabled; Google Drive classification is unknown. This candidate has not passed real install/uninstall or Search/Store input testing.

A separate outstanding limitation remains: if the uninstaller process or computer is abruptly stopped during recursive file removal, the in-process catch cannot run. Backups are retained, but automated cross-process restoration of that partial removal is not yet implemented or validated.

Repeatable mapped-module integration is now in native/tests/mapped_module_integration_tests.ps1 and uses native/tests/mapped_module_fixture.cpp. Windows PowerShell 5 ran it successfully: a short-lived fixture loaded only the workspace DLL; the production read-only helper found its exact PID and rejected a different path, without running an installer or touching TSF registration. Simulated inaccessible or inconsistent process-module inspection also stops uninstall before registration removal.


## 2026-10-28: interrupted uninstall file recovery

The uninstall file validators and restore helper are now shared by installation and uninstallation. After registration absence, verified package and maintenance backups are bound to a flushed, atomically renamed receipt under Program Files before deletion. A subsequent uninstall or install detects the receipt under the release mutex, checks the same user/root, original plan and maintenance hashes, live registration absence, and refuses unknown or changed files/Apps values before restoring; it holds read-only backup handles during revalidation and restoration. Successful removal or restoration clears the exact unchanged receipt. If validation fails, evidence and backups are retained. A failed temporary receipt write cannot create a partial final receipt.

A dedicated isolated test killed a child process after deleting test product and maintenance files, then a separate process restored them from the receipt. Other cases cover removal of the entire test tree, absent original maintenance, modified plan/backup/Apps entry, different SID, live registration, active backup writer, foreign file contents and duplicate receipt. Windows PowerShell 5 and PowerShell 7 each passed 34 assertions, including forced termination during recovery and successful recovery on a subsequent attempt. On this NTFS test volume, renaming the backup directory while the recovery file handles were held was blocked; this observation is not a guarantee for every filesystem. The installer recovery-loop fixture passed 23 assertions; the uninstall validators passed 78. Tests use fake registry data and did not invoke an actual installer or remove real product files. A forced process kill is not a power-loss test and is not proof that every filesystem crash state is recoverable.

R60.17 was assembled and its 19 release payloads, embedded ZIP, current scripts, TSF DLL and CLI hashes verified. Setup SHA-256 df0f2356dd7bc1c2f25922e35892f6b313642266339942219716195b23c590bc. Defender custom scanning without remediation found no threat; Google Drive classification is unknown. R60.17 has not been installed, uninstalled, or tested in Windows Search/Microsoft Store. The prior failed local transaction still blocks installation pending Windows UAC.

A simulated UI-less host now exercises candidate COM interface discovery, exact page-index retrieval, host-owned display, selection, finalization, update and end notifications. It passed, but does not launch Windows Search or Microsoft Store and cannot prove text entry in either application.


## 2026-10-28: isolated UI-less input using the packaged DLL

A repeatable isolated TSF fixture loads the actual R60.17 package DLL and its adjacent release dictionary, creates an in-process TSF document with TF_TMAE_NOACTIVATETIP and UI-element-only activation, and invokes the DLL's ITfKeyEventSink. The input sequences a + Space and oek + Space produced 對 and 嘸 in the TSF document. Settings are confined to the unique build fixture directory and removed by the test. The fixture did not register the TIP or activate other installed TIPs. A fresh read-only status afterwards showed all seven product resources absent; the original unresolved transaction still blocks installation.

This test exercises the shipped DLL, dictionary and isolated UI-less TSF editing, but it does not test Windows Search, Microsoft Store, app-container restrictions, actual user keyboard focus or a normally registered installation. Real install/uninstall and Search/Store acceptance remain outstanding.

The isolated packaged-DLL run is reproducible with native/tests/run-tsf-binary-uiless-tests.ps1; it validates the DLL and dictionary hashes against the release manifest before launching a unique fixture process. Authenticode inspection reports the R60.17 TSF DLL as NotSigned; this does not establish why Google Drive restricted an earlier installer.

## 2026-10-28: real install and blocked uninstall

The R60.12 failed transaction was manually archived after elevated checks and hash-verified backup; the original outcome was not rewritten. History inspection then returned success. R60.17 completed an interactive installation with its confirmation and reported payload cleanup. The installed uninstaller matches the installer SHA-256. Its real uninstall failed before registration changes because an exclusive open of the input-method DLL encountered a sharing violation; elevated readback confirmed the registration remained present. The uninstaller's temporary worker and extracted payload were cleaned up.

The mapped-module preflight now runs before exclusive file opens and again before registration removal, so a known loader is identified before the generic sharing violation. The isolated uninstall validators passed 81 checks. R60.18 was built with this change; its 19 manifest entries and embedded ZIP contents were checked. Setup SHA-256: a3fea781d5fb7b67b42a1fa86f932bd0f2b5ae76ec45dca23bfd10547c37783e. A real R60.18 uninstall attempt against the installed R60.17 stopped before registration changes and reported msrdc.exe PID 12188 as holding the DLL; elevated status confirmed registrations remained. The extracted temporary payload was cleaned. The occupied-process preflight is a point-in-time check and does not prevent subsequent loads. A successful real uninstall, reinstall, and Search/Store input test remain outstanding.

After the user explicitly authorized ending the identified msrdc.exe process, R60.18 successfully uninstalled the R60.17 installation. An independent elevated status showed all seven product resources absent, both installation directories absent, and history inspection succeeded. R60.18 was then installed interactively; all 12 installed package files matched the manifest, and the installed Uninstall.exe matched the R60.18 setup SHA-256. The user reported that both Windows taskbar Search and Microsoft Store produced the expected Chinese characters for `a` + Space and `oek` + Space. This is user-reported acceptance, not an automated captured test.

The installed R60.18 uninstaller was subsequently launched for its own end-to-end test. It correctly stopped before registration removal because Explorer, SearchHost, ChatGPT and Chrome had loaded the input-method DLL, and its temporary worker and payload were cleaned. A successful installed-R60.18-uninstaller run and a final reinstall remain pending release of those applications; the R60.18 installation remains present.

## R60.20 automatic failure diagnostics

Setup embeds the read-only collector as a separate resource and includes it in the release bundle. On an install/uninstall error it attempts to create a local diagnostic ZIP before removing the extracted payload, records the ZIP path or collection error in the setup log when writable, and reports the location in the non-silent error dialog. It also creates a fallback setup log for failures before normal argument parsing completes. Cancellation of the install confirmation remains a cancellation, not a collected failure. The collector adds diagnosis.json with observed lifecycle results and collection issue counts; it does not claim the error code proves a root cause. It does not upload the archive.

The Setup fixture passed 61 checks, including a simulated collector, argument quoting, collector failure and temporary script removal. A separate run exercised the real collector and verified the diagnosis summary. The built R60.20 executable was run with an intentionally invalid argument and --silent: it returned the expected error and produced a local ZIP containing the original error log, environment.json, collection-issues.json and diagnosis.json, without executing installation or uninstallation. All 20 release files and ZIP entries matched their manifests and source files. Setup SHA-256: ea593cf2f266754871f739f6e7ef58490222861b8ab2a820ea08d2f27b5fbab6. R60.20 has not undergone an actual install/uninstall cycle or a real child-installer failure test; collection can be partial when Windows denies access.

## 2026-10-29: installed uninstaller and R60.20 installation

The installed R60.18 uninstaller completed successfully after applications released the DLL; its elevated worker exited 0 and both worker and payload cleanup were logged. Independent elevated readback found all seven registration resources absent, no product or maintenance directory, and history inspection succeeded. R60.20 then completed interactive installation with exit 0 and payload cleanup. All 12 installed package files matched the R60.20 package manifest; the installed uninstaller matched the R60.20 setup SHA-256. Independent elevated readback reported the expected x64 COM registration, profile, categories, and user enabled. Actual Search/Store input confirmation for R60.20, a successful R60.20 uninstaller run, and a real child-installer failure diagnostic remain pending.

The user subsequently confirmed R60.20 produced the expected Chinese characters for both input sequences in taskbar Search and Microsoft Store. The installed R60.20 uninstaller then stopped on a real mapped-DLL preflight failure for ChatGPT and SearchHost, before modifying registration. The uninstaller generated a local diagnostics ZIP, and inspection confirmed that it contained the actual failure log, 31 transaction evidence files, diagnosis summary and zero reported collection issues; worker and payload cleanup completed. Independent elevated status confirmed R60.20 remained registered. A successful installed R60.20 uninstall and final reinstall remain pending release of the DLL.

After the user authorized closing the identified TourBox process, the installed R60.20 uninstaller completed successfully (exit 0); worker and payload cleanup were logged. Independent elevated status showed all seven product resources absent, profile registry absence, no product or maintenance directory, and history inspection succeeded. A subsequent R60.20 interactive reinstall was canceled at its confirmation dialog before deployment (exit 1223). The user explicitly chose to leave the product uninstalled. A successful R60.20 reinstallation after this removal is therefore not verified; the earlier R60.20 installation and Search/Store input results remain separately documented above.

## R60.21 long English composition

Normal composition no longer drops input after five characters, allowing Enter to commit the accumulated original keys. Dictionary lookups and the separate phonetic/homophone query limits retain their existing rules; normal candidate selection by number is unchanged. TSF text writes reject values that exceed the Windows LONG length representation instead of truncating on conversion.

The engine fixture verified 2,048 letters, case preservation in the engine, digit and auxiliary-key entry after a long code, Backspace, Escape, and Chinese candidate selection. An isolated TSF UI-less fixture loaded the actual R60.21 packaged DLL and dictionary, committed two Chinese characters, typed 600 English letters, removed one with Backspace and verified all 599 committed by Enter. One repeat failed at the first Chinese-character assertion and a subsequent repeat passed; the intermittent fixture failure's cause is unknown. This fixture does not test an installed IME, Windows Search or Microsoft Store. The package's 20 release payloads and embedded ZIP were hash-verified. Setup SHA-256: b8322f649330b5b0b4a3b8cf3976b30b722853ddeb0364c50c2f3c5b1f12b30f. The candidate has not been installed or tested interactively; the local machine remains uninstalled per user preference.
