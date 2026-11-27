# 讀音與候選資料核對 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用一個可在 Windows 重跑的離線核對工具，列清所有讀音/候選差異並阻止雙端資料漂移，保留現有主要音與候選排序。

**Architecture:** 不修改 C++/Kotlin runtime。沿用 core `readings.tsv` 作 canonical、Windows 作 byte-identical 複本；獨立候選觀察表不自動合併到讀音真值。一個 Python 標準庫腳本負責檢查及報告，CI 只阻擋可確定的不變量破壞。

**Tech Stack:** Windows 原生 Python 3.11、unittest、UTF-8 TSV/JSON、既有原生 C++ 測試；generator 真有改動時才增加 Windows JDK 17/Gradle JVM 回歸。

---

狀態：三輪 Neon 對抗後，使用者已於 2026-11-27 核准執行；以下 checklist 按真實證據更新。[對抗摘要](2026-11-27-installer-readings-review.md)。關聯：[issue 19](https://github.com/hamanpaul/liu-kai/issues/19)。使用者已明確決定：**兩入口維持現行主要音規則，先只核對資料**。

## 範圍

本次交付是核對工具、全表報告與明確裁決，不是重建正音字典。沒有新的已證實錯誤前，不改 `gen-readings`、readings 原始列、C++/Kotlin query 或既有候選順序；不先做 override 平台。

| 檔案 | 用途 |
| --- | --- |
| Create `scripts/audit-readings.py` | 一個標準庫工具：資料解析、不變量、完整關聯分類、deterministic JSON |
| Create `scripts/tests/test_audit_readings.py` | 合成最小 fixture，包含 Unicode、空組、次讀音、真資料漂移與排序不變 |
| Create `docs/data/readings-audit.md` | 來源/hash、完整分類數、主要音政策及未判定範圍 |
| Create `docs/data/readings-decisions.tsv` | 只有逐項有證據的裁決；欄位 char/reading/status/source/reason，不是 3,738 項免審白名單 |
| Modify `.github/workflows/windows-installer.yml` | 在既有 Windows job 跑小型純資料檢查並輸出 audit artifact，不新增另一條重負載 CI |
| Modify `windows/RELEASE_FLOW.md` | 封裝前核對 canonical/copy、記錄資料來源 |
| Modify `docs/plan.md` | 標示已完成核對與仍未裁決的部分，避免將報告寫成全部修好 |
| Create `changelog.d/19-readings-audit.md` | 實作完成時記錄核對與門禁，不宣稱已改音 |

## Task D1：固定不改行為的契約與 RED

- [ ] 用合成 readings 建立 A 主音 R1/次音 R2、B 主音 R2、字典包含 A/B、候選表包含 R1/A、R2/A/B、明示空組與注音符號的最小 fixture；不依賴私人字表或網路。
- [ ] 先寫測試再執行 `python -m unittest discover -s scripts/tests -p test_audit_readings.py -v`，保存因缺少核對工具而出現的 RED；之後的解析錯誤必須明確指向資料，不以 exception 被吞掉當過關。
- [ ] 定義程式入口 `audit_readings(canonical, windows_copy, candidates, dictionary, decisions)`；測試依 repo 現有腳本方式用 importlib 載入 hyphen 檔名，不再建立第二套 package。
- [ ] 保證這個工具只讀 input，output 只能寫到指定 report 路徑；測試比較輸入前後 bytes，避免「核對」意外改候選或主要音。

合成期望：A/R2 屬 `listed_secondary`，不是缺漏；C/未知音屬 `unresolved_relation`；`ㄚ/ㄚ` 屬 `bopomofo_literal`；明示空組保持一筆 `explicit_empty`。次讀音出現在報告不表示 runtime 會查出該字。

## Task D2：最小離線工具與不變量 GREEN

- [ ] 使用 `csv.DictReader`/明確 TSV 欄位、`hashlib`、`json`、`unicodedata`（只作識別/報告，不隱式改碼）及標準集合。沒有外部資料庫、網路、自然語言模型或 JVM 依賴。
- [ ] 核對 canonical 與 Windows bytes；不只比較解析後集合。BOM、換行及輸出順序差異均呈現為資產同步問題，不能悄悄正規化後說一致。
- [ ] 驗證 UTF-8、必要欄位、重複字／關聯、非空有效 readings、候選明示空組不得與非空列混用；輸入缺少、格式不合法與讀取失敗回非零，保留檔名/行號。
- [ ] 分離 `primary_match`、`listed_secondary`、`bopomofo_literal`、`unresolved_relation`、`unexpected_missing_character`、`explicit_empty`。`bopomofo_literal` 僅容許單一 U+3105–U+3129 且 reading==text；不能用整個非漢字範圍當白名單。
- [ ] `unexpected_missing_character > 0` 時 `--check` 必回非零。裁決表的 `status` 只接受 `consistent`、`confirmed_error`、`intentional_difference`、`unresolved`；source 必須非空，每個 `(char, reading)` 必須存在於當前讀音或候選輸入，重複/孤兒列必失敗。裁決不可壓掉結構錯誤；`confirmed_error` 未修復或仍有未判定關聯時，正音審查完成狀態仍為 false。
- [ ] 字典過濾以明確 section 與 exact text 作投影，不把簡繁轉換、罕見字或長字串默認為等價。報告說明哪些差異是 active dictionary 的可用性，不冒稱讀音真偽。
- [ ] 完整報告按 Unicode key 排序、固定 JSON 格式；同一 input 必須 byte-identical。source hash 記在報告，時間/機器路徑不進 deterministic 內容。
- [ ] `--check` 只代表格式、雙端一致、裁決格式及已確認代表字契約通過；即使回 0 也要輸出 `pronunciation_review_complete=false` 與未判定數，不能命名成 all-correct。
- [ ] 取得全部合成測試 GREEN；另外改動 Windows 複本一 byte、刪主要音、混合空組、重複關聯等負例必須失敗。

CLI 契約（實作時才新增）：

```powershell
python scripts/audit-readings.py --repo . --report build/readings-audit.json --check
python -m unittest discover -s scripts/tests -p test_audit_readings.py -v
```

第一條輸出 `invariants=PASS; unresolved_relations=<count>; pronunciation_review_complete=false|true`。不得用沒有檢查報告內容的 exit 0 來關 issue。

## Task D3：全表對帳與具名裁決

- [ ] 對基準與當前來源各執行一次，保存 source commit/hash、分類計數與關聯清單。初始計數應與共用設計的 17,972 關聯分解對上；計數是觀測基準，不寫死成永遠正確的數字。
- [ ] 把全部 37 個無讀音項核實為注音符號文字，以結構規則處理而非新增 37 個假正音。保留 4,644 空組與既有來源 metadata。
- [ ] 對 3,738 個未列關聯再做機械結構分類，例如只有聲調不同/音節不同；這些只作審查分組，不自動改成「合法次讀音」或「官版刻意相容」。
- [ ] 「勒」的主要/次音、繁簡兩入口既有出字、候選字碼不入上屏列為回歸；另選報告中已列次音及疑似缺漏的代表，區分現行 query 契約與讀音資料。
- [ ] 真正的正音裁決引用教育部官方字典或已固定的上游 Unihan 證據，記錄條目識別/來源版本/理由；不使用第三方輸入法未授權資料，不自動 scrape 全站。
- [ ] 未找到真值的項目維持 unresolved。不能把全部剩餘關聯寫成 legacy_compatibility 就當全表核對完成；也不將殘餘搬到新 issue 後關原單。
- [ ] 若出現新且已證實的資料錯漏，先列出最小修正與影響範圍供審閱。未核准前不改主要音/候選資產；若之後核准，仍由原 `gen-readings` 單一 producer 修復，確有多個已證實 override 才抽成一份小表。

此任務允許交付「完整集合核對已完成，但部分正音未判定」；這不是 issue 19 全部驗收通過。

## Task D4：門禁與既有輸入回歸

- [ ] 現有 root Python unittest discovery 應自動包含新測試；Windows installer job 也明確執行 audit `--check` 並保存完整 report artifact。不要在 Android/Windows 各複製一個 audit 工具。
- [ ] 原生 C++ 跑既有 homophone/phonetic/readings 測試，確保兩入口仍採主要音、傳統候選順序與純漢字上屏不變；此輪沒有理由改 query runtime。
- [ ] 如果後續核准了 generator 或 Kotlin 資料程式改動，先以已找到的 JDK 17 設定 `JAVA_HOME`，再跑 `gradlew.bat --no-daemon --max-workers=1 :core:check :cli:check`；記錄實際輸出，不因 JDK 存在就宣稱 Gradle/JVM 通過。
- [ ] Windows 沙盒的代表字端到端可與安裝計畫共用同一 guest 串行安排；不新開第二 guest、不啟動 Android AVD、不碰 WSL。不宣稱 Android runtime 已測。
- [ ] 更新 issue 19 的已修「勒」、已核對分類、未裁決數及證據。只有全部驗收需求真有證據，才提議關閉；否則保持開啟。
- [ ] canonical preflight/agy Neon review 在實作完成時執行，本機提交包含測試；不 push、不建正式 release、不擅自改產品語意。

## 明確不做

- 不把直接注音擴成所有次讀音，不調整字碼同音排序。
- 不把候選觀察表當正音來源，不將集合差全部自動補進 readings。
- 不為未知正音做大量 CI 豁免，不追求虛假的零差異。
- 不加入資料庫、常駐分析器、雲端服務或另一個讀音產生引擎。
