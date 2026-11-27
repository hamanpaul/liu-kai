# Liu-Kai Doctor Mode Implementation Plan

2026-11-27 使用者已改為 Android 先行發版，Windows 另排實測。本分支的最新交付順序與條件見 [Android 先行計畫](2026-11-27-doctor-android-release.md)；下列雙端設計與歷史測試結果保留各自範圍，不代表本 Android PR 已交付 Windows。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 以 TDD 建立 Windows 與 Android 的本機匿名 Doctor Mode，依序完成 transport/scope RED、持久紀錄、四個設定操作、雙端實機驗收及效能 soak，最後產出可發行且不洩漏輸入內容的診斷 ZIP。

**Architecture:** Android 以純 Kotlin 投影器加 app 單一 writer/有界 queue；Windows DLL 只讀原子 gate 並以 `try_write` 寫固定容量共享記憶體，由 `LiuKaiSettings.exe` 的長駐 collector 或 `--doctor-once` 命令進入同一 storage handler 落盤。兩端共用 JSONL v1 欄位契約、epoch 與測試 fixtures，但各自使用既有 Kotlin/C++ 入口，不新增 framework。

**Tech Stack:** Kotlin/JVM、Android InputMethodService/SAF、C++17/Win32/TSF、Windows named file mapping/event/mutex、JSONL、ZIP、JUnit/Kotlin test、現有 C++ harness 與 Gradle JaCoCo。

---

## 0. 工作邊界與證據閘門

### 正式交付狀態（2026-11-27）

Android 與 Windows 的 0.1.1 已各自正式發布，版本、來源與下載見 [發行版本](../../../RELEASES.md)。Windows 已補齊本機 Search／Store、Terminal／WSL 及實體 Shift 驗證，正式成品與歷史候選的證據分列於 [Windows 驗證紀錄](../../../windows/VALIDATION.md)。Android 的實際覆蓋率、具名條件式接受及既有高速基準反例見 [Android 驗證](../../doctor-validation.md)，不冒稱 app 100% 通過；八小時觀測仍為非阻擋後續工作。

### 發版前整合驗證歷史（2026-11-27）

截至 2026-11-27 本輪完整重驗及使用者調整驗收後，整體尚未驗收，以下最新結果優先於後面的歷史摘要；原始反例均保留。

- Android 最終標點實作的十項針對性實機案例通過。缺少清理曾造成停用後的大量後續失敗；真正pipeline回歸取得RED後，最小清理修正GREEN，TestPilot全套189項及100%行／分支門檻通過。新完整118項自然結束為117通過、1項既有#22高速連鍵失敗；原37／81及平台UiAutomation錯誤紀錄保留，不改寫為通過。
- 修正前完整118案例的兩份覆蓋資料，其67／73個app class ID均匹配當時封存的編譯類別，無不符或缺件；app為3136行覆蓋／94行未覆蓋、1049分支覆蓋／124分支未覆蓋，未達100%。新修正後須重新核對，正在區分可達缺測與具體編譯器證據，沒有概括豁免。
- Windows 設定頁14項真HWND回歸及完整原生release檢查通過，後者包含19項host測試。已用真正延遲讀取案例重現 named-pipe 回覆遺失，再以有界等待客戶端關閉修正並驗證；這不代表已證實舊長測中斷的唯一原因。
- 最新WiX候選已在同一沙盒完成舊包卸載、重新安裝、診斷歷史保留及登入後collector啟動驗證；此次兩包MSI版號相同，屬卸載後重裝。合成TSF／WM的Shift短控制已通過，不宣稱涵蓋實體鍵盤。
- Windows 已完成64分22秒、93輪穩定測試，每輪257次提交／3089筆事件、drop與queue均為0，容量輪替及序列缺漏逐輪對帳。依使用者一小時新門檻停止原八小時run，保留原計時及終止碼，不算八小時通過。真collector中斷後正常恢復；另一正常設定視窗清除後，原視窗儲存舊預覽被拒且無ZIP，新epoch輸入亦通過。
- 最新完整021候選的最後卸載已通過：Burn與MSI實際回傳3010，僅重啟同一沙盒後，目標程式、MSI／Burn登記、COM／profile、Run、owner及產品待刪項目均清除。Root獨立核對114項封存檔及16段舊資料前綴全部相符，同意控制檔52bytes亦完全相同。沙盒已關閉回收資源，主機安裝未變更；真機驗證提案仍等待使用者確認。
- Android預設32MiB的隔離codec fixture經真Store跨界append／retention／export通過，2665筆刪除與gap相符、總量65,013,020B小於64MiB；這不是IME長跑。新增行為測試JVM49項、ART61項及實際ImeView狀態／敏感隔離／I/O復原通過。Actions新視窗查詢的accessibility設定、既有計數斷言及選用輸入法前置已修正；完整Actions現已通過，原失敗保留。
- Windows最後預定三對實測後兩對KeyDown p99增加52.975／22.482ms，超出本輪OFF跨輪3.426ms，效能驗收標為失敗；CPU與背景負載限制因果判定，不作豁免。既有匿名階段已將高成本縮小至同步edit及其後候選更新範圍，尚未定位唯一函式。Android最新效能驗收以下列quiet1結果為準，舊反例保留。
- 已依使用者指示直接與WSL Thala代理協調，在它完成測試後延後image建置，取得約6分26秒窗口。Windows119–124六輪功能均通過，三對p99增加2.170／1.633／6.661ms，OFF跨輪差3.996ms；主機平均CPU約22.5–28.8%，效能仍未通過。已通知Thala恢復且收到確認，正式及彩排容器均未停止。來源檢查沒有發現逐事件喚醒、輸入執行緒Doctor檔案寫入或跨程序mutex等待；不依猜測改產品，僅批准一次可還原的owner優先權比較，結果不能單獨證明因果或效能通過。
- 該單次比較已在Thala建置自然完成後取得另一空檔執行：NORMAL／BELOW_NORMAL／NORMAL三段p99為114.283／116.487／119.616ms，降低優先權的一段沒有專屬下降；不能據此排除collector相關機制或解除原效能失敗。各段輸入、257次提交／3089筆事件、drop／queue均通過，原程序優先權已還原並獨立讀回，81項證據雜湊及分位數由Root重算相符。第一次工具前置型別錯誤在送出輸入前停止，已修工具並以假資料／真資料唯讀／刻意失敗驗退出碼0／0／9，原失敗不覆寫。所有Thala保留均已通知恢復並收到確認。
- Neon第8輪已完成（總上限8輪用完），Android延後click操作關聯、producer close/staging競態及SAF選檔期間非config Activity銷毀三項缺陷均已取得真RED、最小修正及定向GREEN；producer ART63項及各項EC class ID亦已核對。修正後完整118項為116通過、兩項既有#22高速連鍵失敗；原始報告與退出碼1保留。後續綜合framework／ART63／Actions／Status補測通過，同一production的69／77個EC class ID全部匹配；先前兩次IME綁定逾時仍保留，不能以補測通過宣稱根因已修。當次失敗診斷的second-show控制未觸發，封存後已移除；目前不再重跑118，改以匹配資料產生最終覆蓋報告並準備目前來源的一小時驗證。跨欄位符號模式備註的「startInput會強制改模式」假設與現行controller不符，不依推測改動。
- producer修正已通過兩項定向JVM回歸與ART63項測試，17個當次class ID全部匹配；這不代表所有競態或最終覆蓋率已通過。延後click修正及關閉時避免多餘配置的保護條件，已通過真ImeView一般／取消／不同按鍵快速操作／敏感隔離，以及完整Actions含真service標點觸控與無觸控提交。原同view合成序列在Doctor開／關都只有一次click，保留此對照，改以不同鍵保留原清理競態場景且嚴格要求兩次動作、各自唯一KEY_ACTION。
- SAF已有真系統銷毀後temp被刪除的精確RED，一行修正及按可見breadcrumb導至根目錄的測試前置修正後，已實證原Activity銷毀時temp781B保留，返回新Activity使用原handle完成取消／temp歸零，定向案例GREEN；系統選項及picker亦已驗證還原與清理。原單次BACK只返回上層目錄的失敗保留，不以增加固定BACK次數掩蓋。先前未到斷言的timeout均不算產品RED。原AVD因ADB離線而被保護程序停止，已在同一資料根目錄恢復，沒有清除使用者資料；仍只開一個Android及一個Windows環境。
- 使用者已改為各一小時及針對性／防回歸發版門檻，八小時只作非阻擋的記憶體等長跑觀測。Android目前來源候選已自然完成3600秒、60批／360次成功提交，實際退出碼0；Root逐筆核對持久紀錄亦有360次成功提交。61次記憶體採樣均為同一程序，354次主機採樣的Windows實體可用最低34.808GiB、D槽最低112.947GiB，未把WSL guest cache重複相加。這不等於八小時或無洩漏證明。真實停止app後，正常輸入入口恢復6/6提交、100筆新事件、三段舊紀錄前綴及consent／epoch13保留，並於診斷instrumentation重新附加前封存；原有sequenceGap1未增加，不冒稱gap0。
- 正常雙設定視窗已驗清除epoch13→14；原視窗307筆的固定預覽返回後儲存被拒，真正SAF目的檔維持0bytes。兩項新公開契約亦在實機通過：另一訂閱清除後帶真URI返回，以及刪除已授權document後SAVE失敗、預覽暫存仍完整。Actions runner實際退出0／CODE−1；先前因尚未成為served view即呼叫showSoftInput而中斷的測試前置失敗保留，僅修正測試前置，沒有重跑或改寫完整118結果。
- 該次七份匹配EC合併後，行覆蓋3196／3291（97.1%）、分支1095／1213（90.3%），原100% verification退出1。當時具名例外提案尚未採用；使用者於2026-11-27改為先補真正缺測、移除無用程式，再讓Android先發版，最新執行依[Android先行計畫](2026-11-27-doctor-android-release.md)。編譯器保留本體、未使用accessor與真實runtime／組合分支仍須分列，不能概括宣稱已測；core／CLI／TestPilot及其他門檻不變。
- 後續Android效能前置於ADB失聯後失敗，原保護程序停止其模擬器；失聯前Windows實體可用39.3GiB、D槽112.94GiB，沒有容量門檻違反。guard先送SIGTERM，後才觀察QEMU退出−11，原因仍未知；舊ADB stderr未保存，不事後補造。已修正測試RPC重用711會讀到舊epoch的工具問題，並將ADB故障改為停止輸入但保留VM／資源監控的sticky pause，硬資源門檻不變；目前來源的一小時及恢復證據不受影響。已在原E槽userdata恢復單一AVD，新nonce控制實證epoch18、6/6提交及持久KEY_ACTION關聯；原六輪OFF／ON已自然完成exit0，每輪512樣本／64提交／drop0，12個RPC回覆ID及81份來源雜湊吻合，ON持久事件及bytes逐輪對帳。p99配對差區間為−3.0～−2.8／+1.1～+1.3／−3.6～−3.4ms，第二對高於OFF觀察bucket跨度上界0.6ms，效能仍未通過、原因未定；host CPU平均約39.7～80.3%，不以此豁免。正常GUI清除18→19後同程序立即輸入6/6及持久KEY_ACTION關聯已由Root獨立覆核。唯一AVD已由owned guard正常停止，SIGTERM後QEMU退出0，無SIGKILL或breach；停機後Windows／Linux模擬器皆0，可用實體記憶體42.67GiB、D112.93GiB。私人AVD的開發選項顯示旗標原null→目前1仍明記為未還原差異，不為此重啟測試環境。該次沒有quiet保留，原始高負載反例保留；後續quiet1結果另列，不能把該次失敗改寫成通過。

- Android在Thala的T1 driver真正結束後取得06:32–07:02的明確保留，沿用相同來源／release-held APK，完整OFF／ON各三輪自然退出0。每輪512筆dispatch、64/64提交、drop0，12個RPC ID皆唯一，同PID3017/start10155/epoch19；81份來源前後不變。Root重算p99差的開區間為(0,0.2)／(−1.1,−0.9)／(−0.9,−0.7)ms，三對額外耗時上界都低於原0.5ms目標；host CPU各輪平均17.36–18.82%，沒有依CPU豁免。ON三輪的64個持久COMMIT皆逐一對應KEY_ACTION來源；1035／1035／1034筆的差異僅為30秒週期HEALTH的5／5／4筆，非健康事件均1030筆。Android這項既定dispatch量測範圍通過，不代表延後performClick至輸出的端到端延遲。最低Windows實體可用40.107GiB、D112.897GiB；QEMU／guard受控退出0，停機後native／Linux模擬器皆0、Win48.66GiB。06:58已通知Thala恢復並獲確認。先前高負載失敗不改寫，Windows效能、主機宿主矩陣、app覆蓋裁決及正式發版流程仍未完成。

#### 歷史整合摘要（不代表最新候選已通過）

- 使用者於2026-11-26確認 thala 與 serialwrap 已停止，允許 Windows 沙盒與 Android 模擬器各開一個；可用記憶體按 Windows 與 WSL 合計至少30GB、D槽可用至少80GB控制，低於80GB時先處理WSL空間。Android模擬器可寫資料根目錄已移至E槽。避免停止或調整既有WSL環境；短效能配對錯開輸入與編譯，長測可並行。舊5594 AVD已清除，必要證據外存；本輪兩個環境均已啟動。未完成的115、覆蓋率、效能修正後重測與八小時仍未通過。
- Windows 已觀察到背景Status更新每五秒停用／啟用時間欄位，新真HWND回歸已正確RED；MAI完成最小編輯欄位predicate修正後，最新12項真HWND測試已全部GREEN，包含欄位編輯及焦點保留。最新WiX候選已在本輪乾淨沙盒完成安裝（Burn與MSI均回報0）；效能配對仍待實測，不挪用修正前d795的效能結果。
- 最新候選亦完成解除安裝及3010後的同一沙盒重啟：程式目錄、COM／profile、MSI、Run、背景owner與待刪檔均已清除，既有Doctor紀錄保持不變。本輪實機另發現停用後無法新建舊紀錄匯出，以及未編輯時匯出會把已標記的「剛才」移到當下；兩項新增真HWND回歸均在原程式得到RED，MAI正在做最小GUI修正，因此暫不開始Windows效能或長測。

- Android projection 已走 RED→GREEN，API35 emulator 真正一般／敏感／未知投影通過；正式0.1.0升級基線已還原並保留。
- Windows transport、collector、legacy與TSF UI-less合成／供應字表完整 P0 runner 已由 Codex 執行 exit0，包含 epoch 中斷回收、多producer及 pending COM session module pin。scope 生命週期走 `GetAppProperty(GUID_PROP_INPUTSCOPE)`／有效 edit-cookie range，Service獨立cache；`VT_EMPTY`為NoMetadata(S_FALSE)/Unknown。
- Terra 已驗真正 Sandbox AC/lowIL→medium產品IPC、跨帳戶同package拒絕、16producer同時首次建立、實際collector強殺後舊Chrome續收及正常退出DLL釋放。明確IS_TEXT57→IS_PRIVATE61→IS_TEXT57真實輸入事件18→18→36，敏感階段沒有鍵量關聯。此證據不代表所有預設宿主都有metadata，主機Search/Store/WSL與最終WiX安裝驗收仍待完成。
- Terminal預設缺種類的限制已由使用者接受，選用相容設定有實測；既有右下角右鍵清單已實測版本、Doctor實際路徑與設定入口，不新增tray程序。
- 共用codec/store/export與golden已走RED→GREEN；Kotlin core/cli全套功能與100%行／分支門檻通過。最新Windows來源的完整 `test-release.ps1 -DoctorEphemeralTestUser` 已在單一沙盒的本機目錄通過，包含P0、儲存、12項runtime、17項host、12項真HWND設定及12項installer程序測試；另外10項workflow fixtures也全以數值退出碼0通過。新候選包實裝及效能仍待驗。第一次在分享資料夾執行時，測試用Junction建立遭拒；同帳戶差分確認後改用沙盒本機目錄，原失敗證據保留，產品程式與測試門檻未更改。
- 兩端四個UI操作已實作；Android實機framework、47項Doctor回歸及真SAF儲存／取消／清除／權限失敗復原通過。取消預覽與重複狀態文字更新另經真RED修正。先前115個E2E全量109通過，原失敗保留：三項檔案選擇案例已確認測試檔在第一頁外，修正有界導覽後逐項實機通過；rollover在新版Doctor收集中仍曾失敗，原因尚未確定，不以單次重測通過視為修復。既有兩項高速輸入問題依#22保留舊版對照。上一輪全量測試曾因模擬器退出而中斷，當時沒有完整報告或覆蓋資料；本輪結果以下段為準。TestPilot188項與行／分支100%通過；app100%門檻仍未通過，繼續補可達分支並分開記錄已證實的inline量測限制。
- 本輪Android重新完成115案例：113通過，失敗僅為既有#22的real／soft fast-order-control；本輪沒有再出現先前rollover與SAF失敗，但原始反例仍保留。framework、47項Doctor、actions與status標記全過；兩份EC各67／73個app class的JaCoCo CRC64均符合當次編譯bytecode，無缺件或不符。app為3109行覆蓋／94行未覆蓋、1025分支覆蓋／126分支未覆蓋，未達100%；MAI正在補可達行為測試，未擅自豁免工具限制。Release與held測試包亦已在唯一AVD安裝及核對來源／簽章；短工作量控制發現115留下的匯入字表，先保全並透過正式UI恢復內建表，正式效能三對尚未開始。
- 真正WiX候選已通過正式0.1.0升級、Doctor關閉／無owner的卸載及3010後沙盒內重啟，程式、COM、profile與待刪檔案均清乾淨。Doctor開啟的升級另重現linked憑證只有SecurityIdentification，以及複製token缺少兩項必要handle權限的錯誤；最小修正後，真實MSI升級、背景collector接續及沙盒重啟後Run自動啟動均通過，舊診斷內容保留。升級前預覽確實失效，但舊設定視窗顯示連線中斷，必須重新開啟；此限制已記於使用說明。沒有主機安裝、重啟或停止WSL。
- Android原八小時容量soak在約四小時二十七分鐘因輸入逾時失敗，模擬器亦出現嚴重記憶體／swap增長；原始證據保留，不算通過。已停止該測試模擬器並確認資源恢復，準備以隔離的官方修補版模擬器重新驗證。Windows效能第一輪的長尾尚待較長測試釐清，八小時soak尚未開始。完整app100%、最終安裝卸載、Neon第8輪、preflight/CI/review/merge與雙端release仍未完成；第7輪已完成並逐項修正。

- 每階段保留真正 RED→GREEN 輸出。任何 RED 因拼字、測試設定或工具鏈問題而失敗都先修測試；只有缺少API或正確行為斷言失敗才算預期RED。
- repo 沒有 `openspec/`，不建立 OpenSpec 結構；核准設計、實作順序與驗收均留在既有 `docs/superpowers/{specs,plans}/`。
- 驗證閘門通過前不升版或發布；完成後依使用者授權commit/push，待CI與GitHub review修正完成後合併並各自發布最小版號。沿用既有升級身分與Android簽章。
- 初始 Doctor 狀態必須是關閉；不得因開發 probe 或測試留下啟用預設值。
- Android 不增加 `INTERNET`；Debug `TouchDiagnostics` 的字、key id 與座標仍只供原有 debug 測試，Doctor 不呼叫它。
- Windows callback 熱路徑禁止檔案 I/O、mutex 等待、同步 IPC、完整 engine snapshot、額外 COM 查詢；DLL 不啟動 worker、不在 unload 等待 flush。

## 1. 已確認的最小整合點

### Windows

| 現有點 | 現況 | Doctor 整合 |
| --- | --- | --- |
| `windows/native/tsf/service.cpp` `Service::trace` | 每次 callback 檢查 marker、鎖 mutex、讀 context/active flags 並同步寫 JSONL | 移除磁碟與 COM；改成只讀一份 cached scope/state 和共享記憶體 atomic gate，呼叫 `SharedQueue::try_write`，忽略 Disabled/Full |
| `Service::activate_impl`、`OnSetFocus`、context watch | 原生生命週期已有 `ITfContext*` 可用 | 在生命週期位置查詢 TSF input scope／context 狀態並快取 `ScopeClass`；callback 不查詢 |
| `windows/native/tsf/runtime_diagnostics.hpp` | 目前的 marker＋512 筆同步檔案 journal | 保留檔名作相容 wrapper，或改為 Doctor producer adapter；不得再成為 callback 內同步 writer |
| `windows/native/tools/settings_main.cpp` | 只有設定 GUI | 解析 `--doctor-collector` 與 `--doctor-once`，其餘仍開 `SettingsWindow` |
| `windows/native/tools/settings_window.hpp` | 現有 Win32 設定頁 | 新增診斷頁／狀態；對 handler 的操作用 worker thread/response file 非同步等待 |
| `windows/native/build-fresh-release.ps1` | 一次編譯 `BoshiamyTsf.dll`、`LiuKaiSettings.exe` | transport header 同時由 DLL/settings compile；probe 不另做 DLL worker |
| `windows/native/test-release.ps1` | 現行 Windows 迴歸入口 | 已加入 `doctor_transport_tests`；P0 之後追加 collector/storage/host fixture |

### Android

| 現有點 | 現況 | Doctor 整合 |
| --- | --- | --- |
| `core/src/main/kotlin/.../ime/EditorPolicy.kt` | JVM 純 Kotlin 判密碼 | 保持輸入法既有行為；Doctor 分類放獨立 `core/doctor`，不把 `NO_PERSONALIZED_LEARNING` 誤改成輸入法停用 |
| `LiuKaiImeService.onStartInput` | 接收 `EditorInfo`、更新 controller | 建立 `DoctorEditorState(inputType, imeOptions)`；缺少有效 scope 時不再以 ordinary fallback |
| `ImeView` touch callbacks + `TouchDiagnostics` | 觸控已有 debug 詳細 trace | Doctor 另走 `ImeActions` 的匿名方法，只提供 gesture token、pointer count、pressed/source，不傳 key id/座標 |
| `SettingsActivity` | 系統設定、字表與報告 | 新增診斷區：開關、標記、預覽／匯出、清除及收集狀態 |
| `core/doctor/DoctorStore` 控制狀態 | 唯一 writer 保存使用者選擇與 epoch | 同一狀態同時供 IME 與設定頁讀取，區分開關與實際收集狀態 |
| `ImeView` 候選列 | 輸入中可見 | 顯示固定的 Doctor 狀態字樣（關／收集中／中斷），不顯示事件內容 |
| `scripts/emulator-e2e.sh` | app E2E＋100% coverage | framework instrumentation 加入 Doctor 公開操作與隱私回歸，覆蓋資料須匹配當次編譯類別 |

## 2. 檔案責任地圖

### P0 立即建立／接續

| 檔案 | 責任 |
| --- | --- |
| `windows/native/tsf/doctor_transport.hpp` | `EventRecord`、scope 分類、固定容量 shared-memory queue；header-only 方便 DLL／settings／test 共用 |
| `windows/native/tests/doctor_transport_tests.cpp` | 本輪 RED：實際 file mapping 雙 view、64-byte record、ordinary/sensitive/unknown、滿載即返 |
| `core/src/main/kotlin/.../core/doctor/DoctorProjection.kt` | P0 pure projection API |
| `core/src/test/kotlin/.../core/doctor/DoctorProjectionTest.kt` | 本輪 RED：密碼、NO_PERSONALIZED、TYPE_NULL、未知、匿名欄位 |
| `windows/native/tsf/service.cpp` | 快取 Doctor scope，callback 只 gate＋try-write |
| `windows/native/tests/tsf_uiless_input_tests.cpp` | GREEN 後確認 queue disabled/full 不改變已驗證輸入結果 |

### 持久化與 UI

| 檔案 | 責任 |
| --- | --- |
| `core/src/main/kotlin/.../core/doctor/DoctorEvent.kt` | Kotlin 共用事件值、固定 enum code 與 JSONL v1 欄位投影 |
| `shared/testdata/doctor-v1/*.jsonl` | 合成事件、尾段損毀與白名單 golden fixtures |
| `core/src/main/kotlin/.../core/doctor/DoctorStore.kt` | JSONL、rotation、retention、控制狀態、epoch 與固定 ZIP |
| `app/src/main/kotlin/.../doctor/DoctorSpscRing.kt`、`DoctorWriterCore.kt` | 有界 queue、單 writer 與操作序列化 |
| `app/src/main/kotlin/.../doctor/AndroidDoctorController.kt` | IME／設定頁共用的四個操作與狀態通知 |
| `app/src/main/kotlin/.../doctor/DoctorSafExportTarget.kt` | Android SAF 目的地，預覽與 epoch 檢查仍由同一 Store 負責 |
| `windows/native/doctor_store.hpp` | JSONL rotation/retention/epoch/export 的 common handler |
| `windows/native/doctor_runtime.hpp` | writer ownership、producer 掛載、heartbeat、drain、reconnect、drop 計數 |
| `windows/native/doctor_host.hpp` | collector／`--doctor-once`、同一 handler 與非同步 IPC client |
| `windows/native/tools/settings_main.cpp`、`settings_window.hpp` | process mode、四個 UI 操作及狀態 |

### 實機與效能

| 檔案 | 責任 |
| --- | --- |
| `windows/native/doctor_poc.hpp` | settings EXE 的實機 probe／snapshot 模式 |
| `windows/native/tests/run-doctor-host-probe.ps1` | 安裝包／probe 啟停、status snapshot 與結果收集 |
| `app/src/androidTest/kotlin/.../ime/DoctorActionsDeviceProbe.kt` | Android 真實標記／預覽／SAF／清除操作 |
| `testpilot/liu_kai_testpilot/cases/doctor-input-scope-p0.yaml` | Android 一般／敏感／未知欄位 |
| `testpilot/liu_kai_testpilot/steps.py` | 受控工作量與實際成功提交、容量、耗時計數 |

各平台長時間驗證的編譯來源、啟動指令、原始資料與雜湊另存於該次驗證 run；沿用既有測試入口，不把實機控制工具打包進安裝檔。下方任務保留 TDD 實作順序，實際檔案責任以上表為準。

## 3. Phase 0 — transport、scope 與真實入口

### Task 0: 保存本輪 RED 證據

**Files:**
- Read: `docs/superpowers/plans/doctor-p0-handoff.md`
- Test: `core/src/test/kotlin/.../DoctorProjectionTest.kt`
- Test: `windows/native/tests/doctor_transport_tests.cpp`

- [ ] **Step 1: 執行 Android RED**

```powershell
Set-Location 'E:\agent-work\Liu-Kai\liu-kai-doctor'
.\gradlew.bat --no-daemon :core:test --tests 'com.hamanpaul.liukai.core.doctor.DoctorProjectionTest'
```

預期：Kotlin test compilation 因 `DoctorEditorState`、`DoctorEventProjector`、`DoctorInputScope`、`DoctorTouchSource` 尚不存在而失敗；若出現 Gradle、fixture 或既有測試錯誤，先修正該 RED 環境，不准把其他錯誤包裝成 Doctor RED。

- [ ] **Step 2: 執行 Windows RED**

```powershell
Set-Location 'E:\agent-work\Liu-Kai\liu-kai-doctor'
$work = Join-Path $env:TEMP ('liu-kai-doctor-red-' + [Guid]::NewGuid().ToString('N'))
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File '.\windows\native\test-release.ps1' `
  -ToolchainBin 'C:\msys64\mingw64\bin' -WorkDir $work
```

預期：第一個 C++ test 因 `../tsf/doctor_transport.hpp` 不存在而編譯失敗；`test-release.ps1` 回報 `Compilation failed: doctor_transport_tests`。若不是此原因，先修 test source 或 runner。

- [ ] **Step 3: 保存證據**

把兩次命令、退出碼、最後 30 行錯誤、日期、commit、工具鏈版本寫入 `doctor-p0-handoff.md` 的「RED evidence」；只在兩條失敗原因都正確後開始 Task 1。

### Task 1: 建立 Windows transport 的最小 GREEN

**Files:**
- Create: `windows/native/tsf/doctor_transport.hpp`
- Test: `windows/native/tests/doctor_transport_tests.cpp`

- [ ] **Step 1: 定義固定 ABI 契約**

Header 必須至少公開下列型別與數值；所有欄位都是固定寬度數值／布林／enum：

```cpp
namespace boshiamy::doctor {
enum class ScopeToken : std::uint32_t { Default, Text, Number, Private, Password, Pin, Unsupported };
enum class ScopeClass : std::uint32_t { Unknown = 0, Ordinary = 1, Sensitive = 2 };
enum class WriteResult : std::uint32_t { Written, Disabled, Full, Invalid };
enum class ReadResult : std::uint32_t { Read, Empty, Invalid };

struct ScopeObservation {
    std::int32_t query_result;
    std::uint32_t token_count;
    bool context_available;
    bool context_disabled;
    std::array<ScopeToken, 8> tokens;
};

struct EventRecord {
    std::uint64_t producer_id;
    std::uint64_t operation_id;
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::uint32_t schema_version;
    std::uint32_t event_code;
    std::uint32_t callback_source;
    std::uint32_t scope;
    std::int32_t result_code;
    std::uint32_t duration_us;
    std::uint32_t flags;
    std::uint32_t reserved;
};
static_assert(sizeof(EventRecord) == 64);
}
```

- [ ] **Step 2: 實作 pure scope classification**

`classify_scope` 的順序固定為：

```text
context 不存在 / query 非 S_OK / token_count=0 / unsupported token -> Unknown
context_disabled / Private / Password / Pin -> Sensitive
其餘且所有 token 為 Default / Text / Number -> Ordinary
```

不得把 missing query、空 token 或 context 不明推成 Ordinary。

- [ ] **Step 3: 實作 compile-time capacity queue**

公開 `template<std::size_t Capacity> class SharedQueue`，具備：

```cpp
static constexpr std::size_t storage_bytes() noexcept;
explicit SharedQueue(void* storage) noexcept;
bool initialize(std::uint64_t epoch, bool enabled) noexcept;
bool set_enabled(bool enabled) noexcept;
WriteResult try_write(const EventRecord& record) noexcept;
ReadResult try_read(EventRecord* record) noexcept;
```

控制頭至少有 magic、protocol、capacity、epoch、enabled、write/read index、heartbeat、dropped。hot path 只做原子 load/CAS、固定長度 `memcpy` 與計數；`try_write` 滿載時立刻 `Full`，不 Sleep、不 Wait、不 lock。

- [ ] **Step 4: 驗證 Windows GREEN**

```powershell
$work = Join-Path $env:TEMP ('liu-kai-doctor-green-' + [Guid]::NewGuid().ToString('N'))
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File '.\windows\native\test-release.ps1' `
  -ToolchainBin 'C:\msys64\mingw64\bin' -WorkDir $work
```

預期：`doctor_transport_tests` 先通過，輸出 `Doctor transport, scope classification and bounded shared-memory queue passed`；既有 Windows preflight 仍通過。

### Task 2: 接入 TSF 生命週期而非 callback 查詢

**Files:**
- Modify: `windows/native/tsf/runtime_diagnostics.hpp`
- Modify: `windows/native/tsf/service.cpp`
- Modify: `windows/native/tests/tsf_uiless_input_tests.cpp`

- [ ] **Step 1: 先寫 service regression**

在 UI-less test 建立滿載 Doctor queue，填入 `Service` 的 test-only constructor parameter，仍呼叫既有 `OnTestKeyDown`／`OnKeyDown`／`OnKeyUp` commit 一個合成字；排程必須和 queue 狀態無關。注入的是實際 queue view，不是假 Writer／mock。

- [ ] **Step 2: cache scope on lifecycle**

`activate_impl` 建立 context 關係時及 `OnSetFocus(true)` 查詢 TSF scope；失敗或 absent scope 寫入 `ScopeClass::Unknown`。context disabled 寫 `Sensitive`。將結果 cache 到 Service，callback 不新增 `QueryInterface`。

- [ ] **Step 3: 替換 callback hot path**

`trace` 只可：

```cpp
if (!doctor_producer.enabled.load(std::memory_order_acquire)) return;
const auto record = doctor_cached_record(event, result, session, eaten);
doctor_producer.queue.try_write(record);
```

不得保留 `valid_marker`、`write_line`、mutex、`engine->snapshot()`、`read_context_record` 或任何完整 snapshot。ordinary 之外不逐鍵；health/lifecycle 事件仍可寫。

- [ ] **Step 4: 驗證**

```powershell
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File '.\windows\native\tests\run-tsf-uiless-input-tests.ps1' `
  -ToolchainBin 'C:\msys64\mingw64\bin' `
  -WorkDir (Join-Path $env:TEMP ('doctor-tsf-' + [Guid]::NewGuid().ToString('N')))
```

預期：原有 synthetic/supplied dictionary 兩 fixture 及新增 queue-full commit 通過，`DllCanUnloadNow()==S_OK`。

### Task 3: 驗證 settings EXE collector 骨架與重連

**Files:**
- Modify: `windows/native/tools/settings_main.cpp`
- Create: `windows/native/doctor_collector.hpp`
- Create: `windows/native/doctor_command.hpp`
- Modify: `windows/native/test-release.ps1`
- Test: `windows/native/tests/doctor_collector_tests.cpp`

- [ ] **Step 1: 寫 collector P0 tests**

測試先從兩個真實 file-mapping view 驗證：collector heartbeat、producer attach/reconnect、epoch 不符拒收、abandoned producer slot 回收、disabled 停接收。第二次 `initialize` 不能清除正在服務的 queue。

- [ ] **Step 2: settings command modes**

`LiuKaiSettings.exe` 公開：

```text
--doctor-collector [--output-dir <dir>]
--doctor-once <status|mark|preview|export|clear> --request <response-file>
--doctor-probe collect|snapshot|stop --output <path>
```

無參數仍開設定 GUI。collector 所有磁碟與背景工作都在 settings EXE；DLL 只連接 mapping。`--once` 與 collector 呼叫同一 handler，writer ownership 以 named mutex、response file 與 heartbeat 仲裁。

- [ ] **Step 3: reconnect 與 unload**

collector crash 後 DLL 保持每次 focus/lifecycle 嘗試以 bounded open 接回同一 named mapping；DLL 不等 collector。mapping owner 釋放後 stale view 關閉。Unload 時沒有 join、wait、flush 或 worker 析構阻塞。

- [ ] **Step 4: build and test**

```powershell
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File '.\windows\native\build-fresh-release.ps1' `
  -ToolchainBin 'C:\msys64\mingw64\bin' -Version '0.1.1-dev' `
  -OutputDir (Join-Path $env:TEMP ('doctor-build-' + [Guid]::NewGuid().ToString('N'))) `
  -SetupName 'WuGai-doctor-p0-x64-Setup.exe'
```

僅建 dev probe；不得改 root/package version，亦不宣稱宿主已通過。

### Task 4: 建立 Android projection 的最小 GREEN

**Files:**
- Create: `core/src/main/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjection.kt`
- Test: `core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt`

- [ ] **Step 1: 實作 core-only 合約**

```kotlin
data class DoctorEditorState(val inputType: Int, val imeOptions: Int)
enum class DoctorInputScope { UNKNOWN, ORDINARY, SENSITIVE }
enum class DoctorTouchSource(val code: Long)
data class DoctorTouchEvent(
    val gestureToken: Long,
    val pointerCount: Int,
    val pressed: Boolean,
    val source: DoctorTouchSource,
) {
    fun toV1Fields(): Map<String, Long>
}
object DoctorEventProjector {
    fun classify(editor: DoctorEditorState?): DoctorInputScope
    fun touch(editor: DoctorEditorState?, gestureToken: Long, pointerCount: Int,
              pressed: Boolean, source: DoctorTouchSource): DoctorTouchEvent?
}
```

分類順序：缺少 editor、`TYPE_NULL`、不支援 class -> Unknown；密碼變體或 `IME_FLAG_NO_PERSONALIZED_LEARNING` -> Sensitive；已列為支援的一般 text/number -> Ordinary。missing scope 絕不 fallback。

- [ ] **Step 2: 驗證 exact RED test**

```powershell
.\gradlew.bat --no-daemon :core:test --tests 'com.hamanpaul.liukai.core.doctor.DoctorProjectionTest'
```

預期：四個 tests 全綠，anonymous `toV1Fields()` 只有六個固定欄位。

- [ ] **Step 3: 連 app 而不碰 debug 詳細 trace**

`LiuKaiImeService.onStartInput` 只建 `DoctorEditorState`；`ImeView` 另送 gesture token/pointer count/pressed/source 到 `ImeActions`。不要將 `MotionEvent`、key id、座標、文字或 `SoftKey` 傳給 Doctor projection。`TouchDiagnostics` 的 debug-only 程式維持不變。

- [ ] **Step 4: 驗證 build variants**

```powershell
.\gradlew.bat --no-daemon :app:compileDebugKotlin :app:compileReleaseKotlin
```

預期：Debug 詳細 `TouchDiagnostics` 與 Release no-op 各自仍編譯；Doctor projection 同時存在且不依賴 Android SDK 字串型別。

## 4. Phase 1 — JSONL v1 與持久紀錄

### Task 5: 鎖定跨平台事件 schema 與 golden fixtures

**Files:**
- Create: `shared/testdata/doctor-v1/ordinary-touch.jsonl`
- Create: `shared/testdata/doctor-v1/sensitive-lifecycle.jsonl`
- Create: `shared/testdata/doctor-v1/unknown-lifecycle.jsonl`
- Create: `shared/testdata/doctor-v1/torn-tail.jsonl`
- Modify: `core/src/test/kotlin/.../core/doctor/DoctorProjectionTest.kt`
- Create: `windows/native/tests/doctor_jsonl_tests.cpp`

- [ ] **Step 1: 定義白名單**

JSON 每列只允許 `schemaVersion`、build/source 固定識別、`producerId`、`epoch`、`operationId`、`sequence`、`monotonicNs`、UTC、時區位移、`eventCode`、`callbackSource`、`scope`、mode/focus/composition/candidate stage、`handled`、固定 `resultCode`、`durationUs`、drop/gap。敏感與未知只允許 health/lifecycle 與固定結果；ordinary 才允許逐鍵階段。

- [ ] **Step 2: 寫 Kotlin/C++ fixture tests**

兩端各自編碼相同合成事件，輸出逐 byte 相符。遞迴白名單檢查遇到 text、keyCode、keyId、coordinates、candidateText、rootCode、surroundingText、path、title、clipboard 或 exception message 必須失敗。

- [ ] **Step 3: 跑 test**

```powershell
.\gradlew.bat --no-daemon :core:test
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File '.\windows\native\tests\run-doctor-jsonl-tests.ps1' `
  -ToolchainBin 'C:\msys64\mingw64\bin' `
  -WorkDir (Join-Path $env:TEMP ('doctor-jsonl-' + [Guid]::NewGuid().ToString('N')))
```

### Task 6: Android 單 writer、rotation、retention 與 epoch

**Files:**
- Create: `app/src/main/kotlin/com/hamanpaul/liukai/doctor/DoctorWriter.kt`
- Create: `app/src/main/kotlin/com/hamanpaul/liukai/doctor/DoctorStore.kt`
- Create: `app/src/main/kotlin/com/hamanpaul/liukai/doctor/DoctorState.kt`
- Create: `app/src/test/kotlin/com/hamanpaul/liukai/doctor/DoctorStoreTest.kt`

- [ ] **Step 1: RED**

測 bounded queue 滿、磁碟滿／IOException、權限拒絕、crash 前尾批、重啟後 torn tail、時鐘跳動、7 天＋32 MiB retention、epoch 清除前後、export 與 clear race。

- [ ] **Step 2: 最小 writer**

IME hot path 只 `offer`；一個 daemon worker flush、rotate、trim，queue 滿只增加 dropped count。停用清空未落盤 queue、保留既有檔。writer failure 更新狀態為中斷，不 throw 到輸入 thread。

- [ ] **Step 3: epoch/clear**

先停止接收、保存清除意圖，再建立並 `fsync` 新 epoch、丟棄舊 queue、作廢 snapshot，最後刪舊 records/temp。只有刪除結果全部確認才回 `success`。第一筆意圖寫入失敗時停止收集並封鎖目前 writer 的舊資料操作，保留原開關選擇供重試；不可宣稱尚未保存的要求能跨重啟延續。

- [ ] **Step 4: verify**

```powershell
.\gradlew.bat --no-daemon :app:testDebugUnitTest
```

若 app 尚無 JVM test runner，先在 app Gradle 增加既有 Kotlin/JUnit test dependency；不要以 instrumentation test 取代純 storage fault tests。

### Task 7: Windows common storage handler

**Files:**
- Create: `windows/native/doctor_storage.hpp`
- Create: `windows/native/doctor_collector.hpp`
- Create: `windows/native/doctor_command.hpp`
- Create: `windows/native/tests/doctor_storage_tests.cpp`

- [ ] **Step 1: RED failure matrix**

使用獨立 temp root 測磁碟滿／WriteFile 失敗、拒絕權限、process crash、torn tail、rotation、retention、clock jump、clear/export race、abandoned mutex。

- [ ] **Step 2: 單一 handler**

`run_doctor_command(Command, root)` 是 collector drain 及 `settings.exe --doctor-once` 唯一落盤實作。named mutex 是 writer ownership；abandoned 取得後先檢查 state/epoch、重建暫存，再繼續。不同登入工作階段若未取得收集權回固定原因碼。

- [ ] **Step 3: operation commands**

`status`、`mark`、`preview`、`export`、`clear` 都持有/等待 writer owner；UI 只等待獨立 worker 回覆。collector 與 `--once` 互不在彼此 stack 上等對方。

- [ ] **Step 4: verify**

```powershell
$work = Join-Path $env:TEMP ('doctor-storage-' + [Guid]::NewGuid().ToString('N'))
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File '.\windows\native\test-release.ps1' `
  -ToolchainBin 'C:\msys64\mingw64\bin' -WorkDir $work
```

## 5. Phase 2 — 四個操作與可見狀態

### Task 8: Android Settings、匯出與鍵盤狀態

**Files:**
- Modify: `app/src/main/kotlin/com/hamanpaul/liukai/settings/SettingsActivity.kt`
- Modify: `app/src/main/kotlin/com/hamanpaul/liukai/doctor/DoctorController.kt`
- Modify: `app/src/main/kotlin/com/hamanpaul/liukai/ime/LiuKaiImeService.kt`
- Modify: `app/src/main/kotlin/com/hamanpaul/liukai/ime/ImeView.kt`
- Test: `testpilot/cases/doctor-*.yaml`

- [ ] **Step 1: add E2E RED**

四操作各有一案：初始關閉；開啟顯示實際收集中；「剛才」標記預設前 5 分、後 30 秒並分開顯示操作時間；修改區間後先預覽固定 ZIP 再 `ACTION_CREATE_DOCUMENT`；clear 只有完成刪除才回成功且新 epoch。

- [ ] **Step 2: wire settings**

新增「診斷」區域，只提供 toggle、mark、preview/export、clear 四入口。status 至少分使用者 preference、collector health、retained start/end、gap/drop reason。

- [ ] **Step 3: safe export**

先在 app 專屬暫存建立一份最多 32 MiB ZIP；預覽與存檔都讀這份 snapshot。save URI 後重新核對 epoch，clear 或新 clear request 使舊 snapshot 失效。暫存 completion/cancel/expiry 後刪除。

- [ ] **Step 4: keyboard visible state**

`ImeView` 候選列加固定小字顯示 `關閉`／`收集中`／`已中斷`，不在一般輸入頁顯示 key/event/錯誤 message。

- [ ] **Step 5: E2E**

```powershell
bash .\scripts\emulator-e2e.sh
```

Windows PowerShell 無 bash 時由 Codex 在 repo 支援的 Bash 環境執行同一命令；不得用「環境不便」移除 E2E。

### Task 9: Windows 設定頁與 language bar 狀態

2026-11-25 使用者追加：右下角嘸．改圖示既有右鍵選單須顯示同包目前版本、實際 Doctor 路徑與開啟資料夾操作、Doctor 設定入口；沿用既有原生選單與設定頁。加版本／路徑同來源測試，Terra 驗真實右鍵畫面及點擊結果。UI 路徑不寫入日常事件與 ZIP。

同日使用者接受：已證實未提供欄位種類的宿主只保留健康／生命週期、明示限制，文件提供 Terminal 相容設定；這項限制不阻擋發版。已有種類但讀取錯誤仍須修正，敏感／未知 guard 不放寬。原 Task 10／11 的逐鍵驗收依此最新裁決執行，不能把受控 Terminal 設定變體當成預設支援。

**Files:**
- Modify: `windows/native/tools/settings_window.hpp`
- Modify: `windows/native/tsf/language_bar.hpp`
- Modify: `windows/native/tools/settings_main.cpp`
- Test: `windows/native/tests/doctor_settings_tests.cpp`

- [ ] **Step 1: add UI contract RED**

測初始關、toggle、mark chosen/just now、preview ZIP 與 save epoch guard、clear success/failure status。collector 未取得 writer owner 時 UI 顯示固定原因，不顯示路徑／帳號字串。

- [ ] **Step 2: async operation**

UI worker 啟動 `--doctor-once` 或直接 request；message loop 只更新狀態，不等 mutex/pipe flush。collector crash 時重新判斷 owner 狀態。

- [ ] **Step 3: language-bar state**

只增加固定 Doctor 狀態 icon/text/menu，不把 scope token、record 或 host path 放入 UI。點狀態可開 settings。

- [ ] **Step 4: verify**

跑 `test-release.ps1`、probe 的 fake UI contract、現有 language bar regression；沒有實機 host 測試不得標記 Phase 2 實體驗收。

## 6. Phase 3 — 雙端實測與故障矩陣

### Task 10: Windows 實機 P0 probe

**Files:**
- Modify: `windows/native/doctor_poc.hpp`
- Create: `windows/native/tests/run-doctor-host-probe.ps1`
- Create: `windows/native/tests/doctor-probe.html`

- [ ] **Step 1: probe interfaces**

probe status JSON 只允許數值和固定 enum：`pid`、`processCreation`、`clientId`、`hostKind`、`integrityLevelRid`、`isAppContainer`、`scopeQueryHr`、最多固定數量 `scopeTokens`、`scopeClass`、`queueOpenHr`、`eventsWritten`、`dropped`、`epoch`、`heartbeatAgeMs`。不寫 host path、window title、文字或 key。

- [ ] **Step 2: build/deploy**

```powershell
New-Item -ItemType Directory -Path '.\windows\build' -Force | Out-Null
$probeOutput = Join-Path '.\windows\build' ('doctor-probe-' + [Guid]::NewGuid().ToString('N'))
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File '.\windows\native\build-fresh-release.ps1' `
  -ToolchainBin 'C:\msys64\mingw64\bin' -Version '0.1.1-dev' `
  -OutputDir $probeOutput `
  -SetupName 'WuGai-doctor-probe-x64-Setup.exe'
```

用既有 installer 以同一升級身分安裝到 dev 根，關閉其他版本的輸入法 host 後啟用 probe；不修改 DLL COM 註冊身分。

- [ ] **Step 3: Terra host matrix**

從設定 EXE 啟動同一 collector/probe，逐一在 Terminal／WSL、Chrome 本機 `doctor-probe.html`、記事本、Windows Search、Microsoft Store 的欄位打固定輸入。有提供種類的一般欄位須出現 hostKind/source/mode/commit chain。已證實宿主未提供種類時，依使用者裁決驗證 Unknown、健康／生命週期、逐鍵抑制及輸入正常，明示限制並另驗可用的相容設定；不得改名成 ordinary。有種類卻讀不到、IPC 錯誤或預期事件消失仍算失敗。沙盒缺少的應用程式另列未驗，不以其他程序冒充。

- [ ] **Step 4: sensitive/private/unknown**

Chrome 的 ordinary/password input 及 Windows available private/disabled context 各驗一次。sensitive/unknown 只有 lifecycle/health，禁止任何逐鍵 event、gesture/composition/candidate/commit-length 記錄。

- [ ] **Step 5: reconnect/crash/unload**

先開著 host 再啟動 collector，確認已載入 DLL 無需重啟 host 即接入。記錄 collector PID，特定 PID crash，再啟動新 collector，原 host 再次輸入並看到新 heartbeat。最後關閉全部五個 host，確認 `BoshiamyTsf.dll` 可由 dev 安裝根 rename；若不能 rename，視為 unload 未通過，保存 handle owner evidence。

### Task 11: Windows privilege／mapping probe

**Files:**
- Modify: `windows/native/doctor_poc.hpp`
- Modify: `windows/native/tests/run-doctor-host-probe.ps1`

- [ ] **Step 1: observe actual token**

每個 host 僅輸出 process token integrity RID、AppContainer flag、mapping/mutex open HRESULT。不得以 Program Files 路徑、名稱或輸入法名稱推定 access；Chrome/Store/Standard user 實際值才是 evidence。

- [ ] **Step 2: cross-session decision**

以兩個同機使用者或至少不同登入 session 驗證 writer/mapping namespace 隔離。無證據時保留 feature fail，不能放寬 ACL、不能讓不同帳號讀取。

### Task 12: Android device/E2E privacy

**Files:**
- Modify: `app/src/main/...`
- Create: `testpilot/liu_kai_testpilot/cases/doctor-input-scope-p0.yaml`
- Create: `app/src/androidTest/kotlin/.../ime/DoctorActionsDeviceProbe.kt`

- [ ] **Step 1: field matrix**

實機／testhost 驗 ordinary text、text/password/visible/web/number password、`NO_PERSONALIZED_LEARNING`、`TYPE_NULL`、unsupported type。ordinary 可見 source/mode/commit chain；其他 scope 不可產生 touch/key/composition/candidate/commit-length。

- [ ] **Step 2: lifecycle/fault matrix**

睡眠喚醒、IME 切換、登出登入、重啟、升級、export cancel、clear 失敗都要狀態可見。release APK 重做 privacy probe；Debug `TouchDiagnostics` 不得被當作 release 證據。

- [ ] **Step 3: export proof**

用真的安裝包先記錄、mark、選區間、預覽 ZIP、取消一次、成功分享一次；解壓核對 event schema 與 epoch，確認沒有文字、key、座標、候選或 path。

### Android 同批修正：標點頁預設英數

使用者定案：進入 `?123`／ALT 時一律英數直出，包含正在組字或選字；沿用既有中英切換取消未完成組字，不自動提交。手動返回字母、Space／Enter 自動返回及換欄位重設時恢復進入前模式，原英數保持英數，明確點選中英鍵則依該次選擇。

使用者另定案：空白右側標點鍵即使留白，長按仍開既有標點清單，短按維持字根判斷；其他留白按鍵仍受限制。開啟／收起清單保留組字，選取標點才取消未完成組字並直接輸出標點、保持中文；原英數模式維持英數。

- [ ] 新預設直出、兩項組字取消／literal 符號與模式恢復、留白標點長按開／收、組字中選 `@` 及單引號直出測試實機通過；既有英文、重音字母、autoReturn 開／關、換欄位、智慧鍵盤及布局回歸通過。
- [ ] 最終來源完整 Android E2E／匹配 class ID 的 coverage 與 release 成對效能重新驗證；不以標點修正前的量測代替。

## 7. Phase 4 — fallback、效能與發版

### Task 13: 故障／保留上限矩陣

**Files:**
- Modify: `app/src/main/.../doctor/*`
- Modify: `windows/native/doctor_*`
- Test: both platform fault suites

- [ ] **Step 1: queue full**

固定寫入者不 drain，持續輸入三輪；確認 only Doctor drop count 增加、輸入結果逐字相同、p99 不出現 writer wait。

- [ ] **Step 2: storage faults**

各自注入磁碟滿、拒絕、rename failure、truncate/torn tail；UI 顯示 `中斷＋固定原因`，下一次操作可恢復或明確拒絕。

- [ ] **Step 3: retention**

以合成 clock 建立跨 7 天與超過 32 MiB、暫存 32 MiB 的 fixtures。原始 records＋暫存合計不得超過 64 MiB；匯出排除過期；容量先到者淘汰最舊且在 status 顯示 gap。

### Task 14: 一小時穩定性、針對性驗證與效能比較

**Evidence:**
- 各平台驗證 run 保存實際長時間工作量、來源與成品雜湊、啟停時間、逐輪原始結果及分析；測試控制工具不加入安裝包。

- [x] **Step 1: measure baseline**

同一裝置／Win11 host、同一工作量、關閉 Doctor 及開啟 Doctor 各至少三輪。記 p50/p95/p99、掉字、CPU、RSS、events/sec 與 queue drop，並保留量測邊界：Windows 為同步 `ITfKeystrokeMgr::KeyDown` 呼叫的經過時間，包含同步 callback、等待與呼叫期間搶占；Android 為 `ImeView.dispatchTouchEvent` 經過時間，不含延後 `performClick` 至輸出的全程。兩者不是同一指標，不直接跨平台比較，也不稱為純 callback CPU 時間。

- [x] **Step 2: gate result**

p99 額外 0.5 ms 是初始工程目標，不直接當唯一 hard pass。若長尾惡化超過同機量測誤差或錯誤列數增加，必須標示 fail 並優化；不得只看平均值。

Android quiet1六輪已依原512樣本／64提交完成，Root獨立核對持久紀錄、來源及p99後通過上述dispatch量測範圍：三對ON−OFF的100µs量化開區間為(0,200)／(−1100,−900)／(−900,−700)µs，沒有新增錯誤或掉字。

Windows於2026-11-27使用原未增加QPC metadata的工具，在同一原版宿主完成三對完整OFF／ON；每輪775對按鍵、260字、2322樣本，ON3089事件、drop0／queue0，OFF無逐鍵事件。KeyDown p99配對差為−39.4／−106.8／+335.9µs，各小於初始500µs目標；OFF跨輪幅度546.8µs。Root由原CSV、文字結果、queue及canonical紀錄獨立重算一致。本項僅勾選雙端已測的受控量測範圍，舊高負載反例保留，不推論所有負載均達標或已找出唯一根因。另有metadata的單對ETL只作診斷，不作本項gate；Windows正式成品與宿主矩陣另依[發行驗證計畫](2026-11-27-doctor-windows-release.md)追蹤。

- [x] **Step 3: 一小時穩定性、容量與防回歸驗證**

使用者於2026-11-26明確調整：發版前各端至少一小時穩定測試，加上針對性與防回歸驗證；八小時改觀測記憶體洩漏等長跑問題，不阻擋發版。原先的八小時硬門檻由此取代，功能、隱私、效能、安裝／卸載、CI及review要求維持。

保存實際一小時時間與逐輪結果，核對事件／容量計數、rotation、retention、collector heartbeat、Android #22 基準及 Windows Shift 中英切換。容量另作針對性驗證，明確區分fixture限值與預設32MiB實測；不假定一小時一定能填滿32MiB。仍在同一宿主工作階段做一次核身後的程序中斷與正常恢復，再建立預覽、清除，確認新epoch及舊預覽失效。Windows中止collector；Android writer與IME同程序，須如實記錄IME程序中斷。容量、故障復原、清除與回歸各自保存結果，全部完成才勾選此項。

已獨立覆核Windows 64分22秒／93輪、Android 3600秒／360次提交，以及雙端容量、真程序中斷後普通入口恢復、正常第二視窗清除與舊預覽拒絕。Android另已實證正常GUI清除18→19，前／後／立即普通輸入皆同PID21574與start145456，6/6提交、drop0；六個不同operation的持久COMMIT全部join KEY_ACTION，不需重啟app／AVD或重附probe。Windows Shift為實際TSF合成工作量，Android完整118的兩個既有高速基準失敗仍保留。本項不包含尚待判定的效能、主機宿主矩陣、app覆蓋率或八小時。

已啟動的原八小時run不得改寫計時或退出碼。若因新範圍停止，保留原始啟停、終止結果與已完整驗證輪數，另出一小時驗收紀錄。

- [ ] **Step 4: 非阻擋的八小時長跑觀測**

在來源與程序穩定時追蹤產品程序的RSS／private bytes、CPU、佇列、檔案及handle等長時間趨勢，記錄宿主與模擬器本身的負載，區分產品與測試環境。八小時尚未完成不阻擋本版發行；仍需另交觀測結果，不能把一小時結果寫成八小時通過。先前Android約四小時二十七分逾時的原始反例保留，不以縮短門檻宣稱已解決。

### Task 15: docs、版本與發版前核對

**Files:**
- Modify as required: `README.md`、`docs/plan.md`、`windows/README.md`、`windows/VALIDATION.md`、`RELEASES.md`
- Version files only at release gate: `VERSION`、`app/VERSION`、`app/VERSION_CODE`、`windows/VERSION`

- [x] **Step 1: docs alignment**

同步新的設定入口、隱私白名單、ordinary/sensitive/unknown 限制、缺 scope 不推普通、Terminal sudo 可能無法辨識、ZIP／清除／保留上限。

- [x] **Step 2: full validation**

```powershell
.\gradlew.bat --no-daemon :core:check :cli:check
Push-Location testpilot; .\.venv\Scripts\python.exe -m pytest; Pop-Location
bash .\scripts\emulator-e2e.sh
python -m policy_check --repo .
```

Windows 另跑完整 `test-release.ps1` 與 release verification。core／CLI／TestPilot 的覆蓋閘門不變；Android app 依使用者已核准的[先行計畫](2026-11-27-doctor-android-release.md)及[具名覆蓋對帳](../../doctor-coverage.md)條件式交付，實際為行 97.2%／分支 90.7%，原 100% 驗證失敗保留，不改寫全專案門檻。Windows 未修改 app，沿用同一已驗收 APK 的結果。

- [x] **Step 3: version only after all gates**

依 `RELEASES.md` 同步核對 Android／Windows `0.1.1`、Android versionCode `3`、Windows MSI `70.1.3`。Android 先行批次為 `0.1.3`，後續 Windows 正式交付批次為 `0.1.4`，依[Windows 發行驗證計畫](2026-11-27-doctor-windows-release.md)記錄。保留既有 Android signing identity、Windows COM/product upgrade identity及已發布標籤；文件收尾不重新封裝成品。

## 8. Self-review checklist

- [ ] 每個逐鍵入口都可直接追到 ordinary guard；sensitive/unknown 沒有逐鍵 token。
- [ ] `EventRecord` 及 JSONL 不接受文字、keycode/key id、座標、字碼、候選內容、周邊文字、path/title/clipboard。
- [ ] TSF callback 沒有檔案、sync IPC、lock wait、worker、COM query 或 engine snapshot。
- [ ] DLL unload 沒有 join/wait；collector crash/reconnect 與 host 已載入情境都有實證。
- [ ] queue full 不改輸入結果，drop/gap 只可見於 Doctor status。
- [ ] Android Release 有匿名 Doctor projection；Debug TouchDiagnostics 仍只存在於 debug/test。
- [ ] `LiuKaiSettings.exe` collector 與 `--once` 真的進同一 storage handler；UI 不同步等待。
- [ ] 清除先改 epoch、作廢 preview/export、只在刪除完成後報 success。
- [ ] export 先建 ZIP、preview 取自該 ZIP、save 時檢查 epoch、暫存上限 32 MiB。
- [ ] 32 MiB/7 day/64 MiB 規則、故障狀態及 retained interval 均可測。
- [ ] 已測宿主符合實際 scope：有種類的一般欄位有輸入事件鏈；已證實缺種類者只有健康／生命週期且限制已說明。Terminal 相容設定另驗；缺少或未實測的 Search／Store／WSL 明列未驗，不算通過。
- [ ] 未在本輪升版本、發簽名、commit、push 或 GitHub 寫入。

## 9. Probe 執行契約（已實作，實際宿主驗證範圍依報告）

Probe 會放在已部署的 `LiuKaiSettings.exe`，避免第二個 collector 協定：

```powershell
$settings = 'C:\DevInstallRoot\LiuKaiSettings.exe'
$run = Join-Path $env:LOCALAPPDATA ('LiuKai\doctor-probe\' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$collector = Start-Process $settings -ArgumentList @(
  '--doctor-probe','collect','--output',$run
) -PassThru -WindowStyle Hidden

& $settings --doctor-probe snapshot --output (Join-Path $run 'status-1.json')
# 在一個目標 host 執行固定輸入後再次 snapshot。
& $settings --doctor-probe snapshot --output (Join-Path $run 'status-2.json')
Get-Content (Join-Path $run 'status-2.json')

# 只停止這一個 collector，重啟後讓已載入 DLL 重新連線。
Stop-Process -Id $collector.Id
& $settings --doctor-probe stop --output $run
```

`snapshot` 必須包含固定 enumerated `hostKind`、integrity level RID、AppContainer bit、scope query HRESULT/raw numeric tokens、scope class、mapping/mutex HRESULT、producer attach、queue drop/heartbeat 與 DLL lifecycle。它不得包含任意文字內容、key、candidate、coords、path 或 title。Collector PID 必須用精確 ID 停止。

Terra 的驗收順序固定為：先載入 host → 啟 probe collector → 一般欄位 → snapshot → crash collector → 新 collector → 同一 host 再輸入 → sensitive 欄位 → unknown evidence → 關 host → rename DLL。任何一步缺 event、scope unknown、mapping 未開或 DLL 無法卸載，都保存 status/evidence 並回 Phase 0 修 transport／scope；不得直接宣稱「宿主已通過」。
