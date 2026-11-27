# Liu-Kai Doctor Mode 計畫

狀態為原生整合驗證進行中，設計三輪與實作四輪對抗已完成，整體尚未驗收。對應 [跨平台議題 #11](https://github.com/hamanpaul/liu-kai/issues/11)。現況核對基準為雙平台 0.1.0 的提交 `e045f978eb52001abc8b632482a6f4536cbdf2ab`。

## 使用結果

使用者在既有設定頁開啟 Doctor Mode，日常輸入時保留本機診斷紀錄。遇到偶發問題，透過「標記剛才的問題」留下時間，從預覽頁確認時間區間與缺漏，再儲存診斷 ZIP。兩端沿用各自設定介面，共用事件欄位、匯出格式及驗收案例。

## 已確認的現況

- Android 的 Release TouchDiagnostics 是 inline no-op，Debug TouchDiagnostics 會記按鍵識別與座標。EditorPolicy.isPassword、SettingsActivity、TestPilot 都可沿用。
- Windows 的 runtime_diagnostics.hpp 已有純數字的模式、焦點、TSF callback 與 HRESULT 紀錄；目前在 callback 內檢查 marker、鎖定及同步寫檔，每程序 512 筆後停止。
- Windows DLL 過去曾有卸載死鎖，需要把任何新增的背景執行及關閉等待列入實測。

## 首版操作

沿用設定頁增加「診斷」區域，提供開關、問題標記、匯出、清除四個入口。Android 鍵盤與 Windows 語言列顯示啟用狀態。設定頁另外顯示實際收集狀態、已保留區間與缺漏，區分使用者有開啟和背景收集已中斷。

Windows 右下角嘸．改圖示的既有右鍵選單顯示目前安裝版本、此使用者的 Doctor 資料夾路徑與開啟資料夾操作，以及直接進入 Doctor 設定的入口。版本取自同一安裝包的建置資訊，資料夾與實際記錄、匯出、清除使用同一份路徑來源。路徑只呈現在使用者介面，不加入日常事件或匯出資料。此項為 2026-11-25 使用者新增要求。

初始狀態為關閉。使用者明確啟用後記住選擇，重新開啟輸入法／裝置後續記。問題標記可以選「剛才」或指定時間，預設匯出前 5 分鐘、後 30 秒，資料不足的部分在預覽中標示。時間區間可修改。按標記及匯出會留下實際操作時間，與使用者選的問題時間分開。

匯出先建立一份固定內容的 ZIP，預覽從該 ZIP 產生，再由使用者選擇儲存或系統分享。標記後不用重現原問題。清除只回報實際刪除結果。

## 事件與資料邊界

Doctor 只接受白名單數值、布林與固定列舉。版本、來源提交與 schema 版本為固定建置資訊。每個 producer 有隨機識別碼、單調時鐘、序號及本次操作 ID，用於關聯 callback、引擎、候選和提交階段。以 UTC、時區位移及時鐘錨點對應問題時間。跨程序的接收順序只供呈現，同一操作的因果由來源序號與操作 ID 決定。

允許的狀態包含模式、焦點、組字／候選階段、回呼是否被輸入法處理、固定原因碼、HRESULT／InputConnection 結果與各階段耗時。Android 觸控只記當次手勢 token、指標數、按下狀態和固定回呼來源，不保留可對回按鍵的 token 或座標。Windows Shift 等模式切換以固定事件類別辨識。

文字、字碼、字元雜湊、候選字、原始鍵值、key id、座標、周邊文字、剪貼簿、宿主文件、視窗標題與程序完整路徑都沒有進入事件的欄位。例外訊息以固定錯誤碼投影。輸入型別不傳入任意字串或整份引擎 snapshot。

兩端的輸入情境採以下判定。這是依平台宣告的分類，宿主未宣告的密碼提示可能無法辨識，例如 Terminal 中的 sudo，使用說明須揭露此限制。

| 情境 | Android | Windows |
| --- | --- | --- |
| 一般 | EditorInfo 存在且為受支援的一般文字／數字型別，未命中敏感旗標 | TSF 成功回覆一般 input scope，包括實際回覆的 IS_DEFAULT，且 context 可用 |
| 敏感 | EditorPolicy.isPassword 命中的文字、可見、網頁、數字密碼，或 NO_PERSONALIZED_LEARNING | disabled context，或 private／密碼／PIN scope |
| 未知 | 缺 EditorInfo、TYPE_NULL 或不認識的型別 | 缺 scope、查詢失敗或 context 不明 |

敏感與未知欄位都停止逐鍵、手勢、組字／候選數量及提交長度的紀錄，只保留與按鍵無關的固定頻率健康訊號和生命週期。2026-11-25 實測後，使用者已同意：確認宿主未提供欄位種類時，只保留健康／生命週期，明示該宿主沒有逐鍵診斷，並提供可用的相容設定；此限制不阻擋整體發版。宿主有提供種類卻因整合錯誤讀不到時，仍須修正與重測，不能藉此略過。IS_PASSWORD 不能單獨作為可靠安全判斷。[Android EditorInfo](https://developer.android.com/reference/android/view/inputmethod/EditorInfo) · [Windows InputScope](https://learn.microsoft.com/en-us/windows/win32/api/inputscope/ne-inputscope-inputscope)

Windows Terminal 1.25.3131.0 的預設設定已實測缺少 scope；明確設置頂層 `"defaultInputScope": "alphanumericHalfWidth"` 並重新開啟 Terminal 後，實際回覆 scope 40、留下模式／提交事件鏈。文件需提供此選用設定及未宣告密碼提示的限制。產品不代改使用者的 Terminal 設定，也不把缺 scope 推定為一般欄位。

日常紀錄留在本機。Android 維持無 INTERNET 權限。錯字與字根內容由 [#12](https://github.com/hamanpaul/liu-kai/issues/12) 的獨立同意流程處理。效能 [#13](https://github.com/hamanpaul/liu-kai/issues/13) 共用此處的操作 ID 與階段耗時。

固定健康訊號也可能因 queue 或寫入故障掉筆，完整且合法的健康紀錄不得僅因序號缺號被丟棄。敏感／未知按鍵不分配事件序號或操作 ID，也不累計被抑制鍵量；這項隱私規則由 producer gate 驗證。缺漏由健康狀態與來源序號呈現，不能以拒讀合法健康紀錄阻止復原。

## 兩端實作

共用 JSONL v1 的小型事件表與合成 golden fixtures，由 Kotlin 與 C++ 各自驗證。沿用既有原生程式與測試入口。

Android 在 IME 所在 app 程序使用有界 queue，由一個背景 writer 負責寫檔、輪替及操作命令。SettingsActivity 與 IME 使用同一控制狀態。Doctor 事件另作 Release 可用的匿名投影，Debug 的詳細 TouchDiagnostics 繼續供既有測試使用。

Windows 先驗證 settings EXE 的背景 collector 模式。收集啟用時執行，登入自啟只為已同意的使用者設定。host DLL 只向固定容量的共享記憶體嘗試寫入，collector 統一落盤。以程序建立時間與本次 instance 辨識 producer，處理超限、slot 回收、collector 中斷和重新連線。實際 slot 數及記憶體上限由第一階段測量決定。

collector 與停止後的清除／匯出都呼叫 settings EXE 的同一份儲存 handler。收集關閉時，UI 按需啟動一次性 handler，以同一把 writer ownership 鎖與常駐程序仲裁。只有持有者能寫狀態和操作診斷檔。這把鎖也必須處理 abandoned mutex、同帳戶重複啟動及多個登入工作階段的衝突，未取得收集權時顯示原因。UI 非同步等待操作回覆，collector 與一次性 handler 不互等。

共享控制頭含協定版本、啟用狀態、epoch 及 collector heartbeat。DLL 在按鍵路徑只讀取 gate 並嘗試寫入。掛載／重連在啟用及焦點／context 生命週期處理，舊映射的釋放與新 epoch 的接入列入 POC。既存宿主切回後能加入收集是必要條件。

輸入 callback 不做磁碟操作、等待鎖、同步 IPC、額外宿主 COM 查詢或完整 engine snapshot。直接使用輸入流程已取得的純量狀態。連線準備、權限確認和敏感型別查詢放在對應生命週期位置。DLL 卸載不等待背景 flush。寫入或收集不可用時，輸入照常執行，診斷顯示缺漏及原因。

## 保存、停用與清除

| 動作 | 預期行為 |
| --- | --- |
| 保存 | 原始紀錄最多 32 MiB，可回溯區間最長 7 天，先到者淘汰最舊片段。容量不保證一定能保留 7 天。收集中定期淘汰，另外在啟動、預覽、匯出前清理過期檔。收集程序停用期間的過期檔在下次操作時刪除，匯出一律排除過期事件。 |
| 滿載／故障 | 有界 queue 滿時丟棄診斷事件並計數，檔案寫入失敗時記收集狀態，輸入不等待。異常結束可能損失最後未落盤批次，匯出保留缺口說明。 |
| 停用 | 停止新紀錄並丟棄尚未落盤 queue，保留既存紀錄，可完成同一 epoch 已建好的匯出。 |
| 清除 | 先停接收，原子保存新 epoch，丟棄舊 queue，作廢待確認的預覽／匯出並刪除舊紀錄、暫存。完成才回報成功，之後依先前開關選擇在新 epoch 續記。 |
| 清除失敗 | 顯示未完成並停止收集，拒絕讀取與匯出舊 epoch。已保存清除意圖時，重啟後維持封鎖並續清。若第一筆清除意圖就無法寫入，目前 writer 仍封鎖操作、保留原開關選擇供重試；未保存的意圖無法保證跨重啟延續。 |
| 匯出暫存 | 同時最多一份、最多 32 MiB，完成／取消／到期後刪除。本機原始紀錄加暫存合計最多 64 MiB，使用者自行存出的檔案另由使用者管理。 |

清除、輪替與建立快照由唯一 writer 序列化。儲存目的地選擇完成後仍須核對預覽 epoch，防止清除後分享舊快照。匯出只讀本次 Doctor 白名單檔案。

## 實作順序與驗收

| 階段 | 交付與通過條件 |
| --- | --- |
| 0．原生小型驗證 | Windows 在使用者的 Win11 版本、一般帳戶下，實測 Terminal／WSL、Chrome、記事本、搜尋、Store 的實際 token／IL、共享記憶體連通、啟用後既存宿主加入、collector crash/reconnect、DLL 卸載及跨帳戶隔離。測試環境身分只放在開發報告。兩端驗一般、密碼、private、未知欄位。有提供種類的一般欄位須留下來源、模式與提交鏈；已證實未提供種類的宿主依使用者裁決，只驗健康／生命週期、抑制逐鍵與明示限制，另驗可用的相容設定。敏感／未知沒有逐鍵紀錄。通訊或整合錯誤仍須先修正，再寫完整 UI。 |
| 1．事件投影與儲存 | 共用事件 fixtures 驗兩端欄位，加入禁用資料的探針。測 queue 滿、磁碟滿、權限拒絕、程序 crash、重啟、尾段損毀、時鐘跳動、rotation、epoch 與清除／匯出競爭。 |
| 2．原生入口 | 接上兩端四個操作與可見狀態，測 IME 切換、睡眠／喚醒、登出／登入、升級、卸載、匯出取消、清除失敗。 |
| 3．功能診斷 | 用 Windows Shift 中英切換與 Android 多指輸入案例核對事件鏈，必要時以合成故障控制驗證缺 callback／拒絕 commit／背景丟筆能被區分。使用實際安裝包匯出 ZIP，再依問題時間回查。 |
| 4．效能與發版 | 同工作量交替關／開各至少三輪，比較輸入 callback p50／p95／p99、掉字、CPU、RSS 與每秒事件量。p99 額外 0.5 ms 為初始工程目標，正式門檻依同機基準與量測誤差記錄，不能為通過而忽略長尾。依使用者2026-11-26裁決，發版前改為各端至少一小時穩定測試，加容量、故障恢復、清除與防回歸驗證，核對Android #22基準及Windows Shift。八小時另作記憶體洩漏等非阻擋長跑觀測，不取代前述檢查。通過本機與repo CI後再出雙端安裝包。 |

已有的 Android 高速多指漏字由 [#22](https://github.com/hamanpaul/liu-kai/issues/22) 追蹤。Doctor 的驗收目的是讓故障階段可回溯，逐字內容的復原與既有輸入 bug 修正各有對應議題。

## 對抗審查

使用本機 WSL 的 agy 1.5.16，模型為 gemini-4-neon-high。設計三輪工作階段為 `6334c58e-948f-4ccb-9c54-44255cff865f`；實作 IPC 第四輪工作階段為 `6b5edce3-60e7-4813-95c7-04034bac055a`。總輪數最多 8。

| 輪次 | 質疑與採納 |
| --- | --- |
| 1 | 補上清除時的匯出作廢、明確的 ZIP 暫存容量與 Windows 敏感欄位 POC。Windows 是否受限依實際 token 確認。先驗一條 transport，避免同時製作兩套儲存實作。 |
| 2 | 補上停用後由相同 handler 處理清除／匯出、writer ownership、既存 DLL 重連、輸入型別真值表與必要宿主的通過條件。 |
| 3 | Neon 認為設計可收斂並開始 POC。兩個尚待實測的風險為跨權限宿主連通／重連，以及普通宿主缺 scope 而只能被分為未知。另補 abandoned mutex 與 UI 非同步回覆的驗證。 |
| 4 | 根據真正 AppContainer 的 ACCESS_DENIED 回歸審查產品 IPC，指出 package ACE、namespace、user／session 篩選與 owner 檢核。獨立沙盒確認保護 DACL 配合 exact user／package 可互通並拒絕跨帳戶；產品修正與宿主驗證仍在進行。 |
| 5 | 對照 Chrome 真實各階段與官方 source，確認現有讀取鏈無根本錯誤，先排除多 producer IPC 干擾再重測。VT_EMPTY保守Unknown、明示缺漏符合使用者裁決，需與QI失敗分開；COM身分於生命週期統一。未採納未知欄位逐鍵 mismatch counter，避免鍵量側漏。此輪非正式通過。 |
| 6 | 後端審查補正 native 紀錄的數字排序、清除未完成的操作封鎖、同一 epoch 重試，以及兩端固定三個 ZIP 項目的 STORED／CRC 契約；儲存目的地改為原子替換並拒絕同檔。另驗證連結替換保護、歷史建置資訊及缺漏持久化。各項已有獨立回歸，整體整合與長時間驗證仍繼續。 |
| 7 | 發現高權限卸載前置檢查建立 mutex 時的擁有者不符、同一 context 變成受限後 Shift release 仍記錄、producer 掃描過早停止，以及過期預覽造成介面鎖住。另修清除第一筆寫入失敗時遺失原開關選擇。高權限問題已在沙盒重現，清除修正已通過雙端儲存回歸；其餘修正與完整候選實測進行中。此輪未核准發版，總計只剩第八輪。 |

跨程序 ETW 的控制權限及 AppContainer 的命名物件限制，均須依官方文件與實際宿主觀測決定，不能以輸入法名稱或程序名稱推定連通。[Microsoft ETW](https://learn.microsoft.com/en-us/windows/win32/etw/controlling-event-tracing-sessions) · [共享命名物件](https://learn.microsoft.com/en-us/windows/apps/develop/communication/sharing-named-objects)

[三輪完整問答](https://github.com/hamanpaul/liu-kai/issues/11#issuecomment-7154875536) · [原始 JSON 回覆](https://github.com/hamanpaul/liu-kai/issues/11#issuecomment-7154875536)
