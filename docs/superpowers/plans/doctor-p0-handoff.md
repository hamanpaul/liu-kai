# Doctor Mode P0 交接紀錄（歷史）

這份文件保留 P0 實作交接時的問題、修法與驗證順序，並非目前的驗收狀態。後續持久化、設定操作與安裝整合已繼續進行；最新結果及未完成閘門統一記於 [Doctor 實作計畫](2026-11-25-doctor-mode.md)，Windows 驗證範圍見 [VALIDATION.md](../../../windows/VALIDATION.md)。

## RED evidence

初始基準為 `e045f978eb52001abc8b632482a6f4536cbdf2ab`，加上當時新寫的測試，尚無 Doctor production API。以下為原始失敗紀錄的回填，沒有在目前完整實作上重新製造 RED。日期列出保全日誌的 UTC 最後寫入時間。

| 項目 | Windows transport | Android projection |
| --- | --- | --- |
| 日期 | 2026-11-25 08:03:41 UTC | 2026-11-25 08:05:26 UTC |
| 實際退出碼 | 1 | 1 |
| 正確失敗原因 | 缺少 `doctor_transport.hpp` | `:core:compileTestKotlin` 缺少四個新 Doctor API |
| 原始 log SHA-256 | `115afd0fefa9af4181d163fd02c8346de1474332b612b5ed6c6218f0aeb5f4be` | `cc5fb865fef5f492a94f35eedd6ee46e89a989e6da1cdb5f2c526ac7dcc2df8d` |

Windows 的原規劃工具鏈路徑不存在，因此當時先用可用的 w64devkit 直接編譯同一個 transport 測試取得 RED，沒有將路徑錯誤算成產品失敗。以下把本機編譯器與輸出路徑參數化，列出等價的 repo 根目錄命令；完整 `test-release.ps1` 的後續 GREEN 另記於實作計畫。

```powershell
python -X utf8 windows/native/toolchain.py --architecture x64 --compiler "$toolchain\g++.exe" -- -std=c++17 windows/native/tests/doctor_transport_tests.cpp -o "$output\doctor-transport-tests.exe"
```

原始 Windows 日誌共六行：

```text
liu-kai-doctor/windows/native/tests/doctor_transport_tests.cpp:11:10: fatal error: ../tsf/doctor_transport.hpp: No such file or directory
   11 | #include "../tsf/doctor_transport.hpp"
      |          ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~
compilation terminated.
Compiler exited 0x00000001

```

Android 在隔離的 WSL checkout 執行下列原始 argv，JSON 收據保留 `exit_code=1`：

```bash
./gradlew --no-daemon :core:test --tests '*DoctorProjectionTest*'
```

Android 原始日誌最後30行如下：

```text
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:59:13 Unresolved reference 'DoctorInputScope'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:60:13 Unresolved reference 'DoctorEventProjector'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:60:43 Unresolved reference 'DoctorEditorState'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:66:24 Unresolved reference 'DoctorEditorState'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:67:21 Unresolved reference 'DoctorEventProjector'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:72:22 Unresolved reference 'DoctorTouchSource'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:87:13 Unresolved reference 'DoctorEventProjector'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:88:17 Unresolved reference 'DoctorEditorState'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:92:26 Unresolved reference 'DoctorTouchSource'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:96:13 Unresolved reference 'DoctorEventProjector'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:97:17 Unresolved reference 'DoctorEditorState'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:101:26 Unresolved reference 'DoctorTouchSource'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:105:13 Unresolved reference 'DoctorEventProjector'.
e: file:///tmp/liu-kai-doctor-red-p0-1000/core/src/test/kotlin/com/hamanpaul/liukai/core/doctor/DoctorProjectionTest.kt:110:26 Unresolved reference 'DoctorTouchSource'.

FAILURE: Build failed with an exception.

* What went wrong:
Execution failed for task ':core:compileTestKotlin'.
> A failure occurred while executing org.jetbrains.kotlin.compilerRunner.GradleCompilerRunnerWithWorkers$GradleKotlinCompilerWorkAction
   > Compilation error. See log for more details

* Try:
> Run with --stacktrace option to get the stack trace.
> Run with --info or --debug option to get more log output.
> Run with --scan to get full insights.
> Get more help at https://help.gradle.org.

BUILD FAILED in 20s
5 actionable tasks: 5 executed
```

工具鏈設定為 Java 17、Gradle 8.11.1、Kotlin 2.0.20，以及 x64 w64devkit。收尾核對保留的同一路徑編譯器回報 GCC 16.2.0、檔案最後寫入早於本次 RED；原始 RED log 未保存完整版本 banner，不把收尾核對時間寫成原執行時間。

## 模型與邊界

- `feature/11-doctor-mode`；模型 `MAI Code 2 (mai-code-2)`／reasoning effort `max`
- 此次歷史交接只修 Windows P0 ACL/scope/build，不包含後續 persistence、四操作 UI、ZIP、8h soak 與發版。
- 以下「本輪」均指該次實作者交接；當時未執行 build/test、安裝或註冊 DLL、commit/push 或升版。
- 後續修正與驗證依上述實作計畫接續，不另維護一份 P0 待修清單。

## 已確認證據

- Codex 原先三個 native unit＋TSF synthetic/supplied 已 exit 0；`run-doctor-p0-tests.ps1` 只因結尾 `$true` typo 由 Codex 修正。這是**舊 source snapshot**，不代表本輪新 scope/ACL 修正已綠。
- Terra 真實 ACL RED：手拼 `Local\AppContainerNamedObjects\sid\leaf` 使 AC producer/collector 都回 `80070003 PATH_NOT_FOUND`。
- Terra 真實 scope RED：Chrome 158 `8000FFFF E_UNEXPECTED`、Terminal 1.25 `80004002 E_NOINTERFACE`；tokens 空、scope Unknown、eventsRead 0。輸入「對嘸」成功與 mapping attach 只證明非阻塞，不證明 Doctor ordinary gate。
- Android pure core projection 曾由 Codex GREEN；本輪未重做該結果，也不把它當 Windows P0 全過。
- 使用者接受 Terminal 未開 DefaultInputScope 時只有 health/lifecycle/Unknown；Terra 已證明開啟相容設定後 token 40 可得 Ordinary。不可藉此豁免仍宣告 scope 的 Chrome。

## 本輪實作修正

### AppContainer / ACL

- producer 在 AppContainer 自身 namespace 建 `Local\leaf`。
- medium collector 先以 PID 取得目標 process creation、token user、session、AppContainer SID，再以官方
  `GetAppContainerNamedObjectPath(targetToken, nullptr, ...)` 取得 path；兩次 sizing/read 都只傳 token，不與 SID 混用。
- collector／既有 producer OpenFileMapping 使用 `0x20006`，先後驗：
  - 目標 PID creation 與要求一致；
  - target user SID／session 等於 collector；
  - mapping owner SID 等於 target token user。
- mapping 使用 protected DACL `D:P`：
  - exact user `0x6`；
  - lowbox producer 自身 exact package SID `0x20006`，讓 AC self owner-check 並在同 PID 多 Service 時開既有 map。
- collector owner 取得 READ_CONTROL 後讀 owner；未授 Everyone、All Application Packages、System。
- ownership mutex 名改為 `Local\LiuKaiDoctorP0-Collector-<user SID>`，mutex DACL 只給該 user。
- AccessCheck snapshot 另外輸出 `targetValidationHr`、`mappingOpenHr`、`ownerValidationHr`。

### scope lifecycle

- scope 只透過 `ITfContext::GetAppProperty(GUID_PROP_INPUTSCOPE)` 與有效 read edit cookie/range 取 `VARIANT`，再 QI VARIANT 的 `ITfInputScope`。
- `Service` 新增 `ITfThreadMgrEventSink`／`ITfTextEditSink`：
  - document focus、push/pop context 先 invalidate，避免沿用舊 ordinary；
  - async `RequestEditSession` 結果只放 heap session，接受前核對 context generation；
  - `TrackProperties(GUID_PROP_INPUTSCOPE)` 後，OnEndEdit 只在 pending/retry 或 property change 時讀新 scope。
- probe 固定數字階段：`scopeAppPropertyHr`、`scopeRequestOuterHr`、`scopeRequestInnerHr`、
  `scopeRangeHr`、`scopePropertyValueHr`、`scopeVariantType`、`scopeUnpackHr`。
- `VT_EMPTY` 保持 Unknown，不轉 IS_DEFAULT；Chrome 的 E_UNEXPECTED 會在後續合法 OnEndEdit retry。
- per-Service cache gate 不依 shared latest scope，sensitive/unknown 不配置 operation/sequence。
- async edit session 持有既有 module lifetime pin；無 worker、timer、join 或 unload wait。

### build

- DLL/settings build 與 UI-less runner 補齊 `-ladvapi32 -lbcrypt`。
- `service.cpp` 在第一個 Windows GUID header 前引入 `initguid`；正式 source 直接定義 TSF/InputScope GUID。
- shared protocol 升至 2，避免舊 control layout 被新 collector 誤讀。

## 需要的回歸

- `doctor_transport_tests.cpp`：official namespace/path、DACL exact rights、same user/session、creation、owner、VT_EMPTY、property stage、stale completion、live writer、interrupted-reader epoch recovery、multi producer、suppressed/no-gap。
- `doctor_collector_tests.cpp`：collector ownership、target/mapping/owner stage。
- `tsf_uiless_input_tests.cpp`：ordinary→sensitive→unknown→ordinary、queue full 不影響實際提交、pending scope session 的 module pin／unload。
- 以上是當時交接的回歸要求；後續執行結果以上述實作計畫為準。

## Codex 命令

```powershell
# 從 repo 根目錄執行，將路徑改為本機的 w64devkit 安裝目錄。
$toolchain='C:\tools\w64devkit\bin'
& .\windows\native\run-doctor-p0-tests.ps1 -ToolchainBin $toolchain
& .\windows\native\build-doctor-probe.ps1 -ToolchainBin $toolchain
```

build script 只產生新的唯一目錄 setup，不安裝／註冊、不刪舊產物。

## 原 P0 實機驗證順序

```powershell
# 先在唯一 test workdir 找到 doctor_transport_tests.exe，於 A 的真實 AppContainer token 執行
& <A_APP_CONTAINER_LAUNCHER> <doctor_transport_tests.exe> --appcontainer-self
```

預期輸出 `Doctor AppContainer self-open passed`。

再使用 `windows\native\tests\run-doctor-host-probe.ps1`：

1. A standard collector 對 A Terminal/Chrome/必要 Search/Notepad/Store `Start → ordinary input → Snapshot`。
2. ordinary 必須 `scopeClass=1` 且 token/variant/stage 來自實際 host；Chrome E_UNEXPECTED 若持續、Terminal VT_EMPTY 則保留 Unknown，不推 ordinary。
3. `normal1 → password/private → normal2` 不能從舊 ordinary cache 寫逐鍵；敏感後 ordinary 必須是新 lifecycle/property result。
4. AccessCheck 先跑 A 正向，再由 B medium 與 B 同 package AppContainer 指向 A PID/processCreation；B 必須 `opened=0`，不能只因 process token 權限失敗而宣稱 DACL 已驗。
5. crash/restart collector 後已載入 host 重新接入；DLL unload 不等待；queue full 不影響輸入，且新 memory crash-window 回歸保持 4/4 容量。

這個順序記錄的是 P0 驗證要求，不能用來宣稱所有宿主均已通過。後續已有真實 AppContainer、跨帳戶隔離、受控 ordinary／private 切換、collector 重連及 DLL 釋放證據；缺少的 Search／Store／WSL、最終安裝包與發版閘門仍依實作計畫逐項驗收。
