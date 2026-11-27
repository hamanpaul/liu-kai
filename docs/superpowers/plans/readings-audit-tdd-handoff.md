# Readings Audit TDD 交接

初始 TDD 階段只固定測試契約；後續 production stage 已新增 `scripts/audit-readings.py`。所有測試 fixture 均為 temp repo 內的合成資料，位置照正式 repo：

1. `core/src/main/resources/readings.tsv`
2. `windows/assets/readings.tsv`
3. `windows/assets/phonetic-candidates.tsv`
4. `windows/assets/dictionary.tsv`
5. `docs/data/readings-decisions.tsv`

## 最小報告 schema v1

頂層只有 `schema_version`、`inputs`、`invariants`、`classification_counts`、`relations`、`dictionary_projection`、`decisions`、`unresolved_relations`、`pronunciation_review_complete`。

- `inputs` 只含上述五個 role；每項只有 `sha256`（讀取失敗時為 `null`），不含路徑。
- `invariants` 含 `passed` 與 `diagnostics`。diagnostic 固定為 `code`、`source`、`line`、`message`；code 包含 `read_error`、`invalid_utf8`、`missing_required_field`、`duplicate_character`、`duplicate_relation`、`empty_readings`、`mixed_empty_group`、`copy_byte_mismatch`、`unexpected_missing_character`、`invalid_decision_status`、`empty_decision_source`、`duplicate_decision`、`orphan_decision`。
- `classification_counts` 固定包含 `primary_match`、`listed_secondary`、`bopomofo_literal`、`unresolved_relation`、`unexpected_missing_character`、`explicit_empty`。
- `relations` 的每項只有 `char`、`reading`、`classification`、`in_dictionary`，依 Unicode 的 `(reading, char)` 排序；明示空組以空 `char` 表示。
- `dictionary_projection` 固定為 `{"section":"TRADITIONAL","candidate_texts":[...]}`，只收錄候選文字的 exact match；不得做簡繁轉換或把 exact match 當正音真值。
- `decisions` 固定欄位 `char`、`reading`、`status`、`source`、`reason`，依 Unicode 的 `(char, reading)` 排序。
- `unresolved_relations` 是 `unresolved_relation` 的分類筆數，不因裁決而改分類；`pronunciation_review_complete` 則受裁決與錯誤狀態約束。
- JSON 固定為 UTF-8、`ensure_ascii=false`、`sort_keys=true`、2-space indent、單一結尾換行；不得含絕對路徑、時間或 host 資訊。

## 主要契約

- canonical 與 Windows copy 必須 byte-identical；BOM、換行、列順序或一 byte 差異都失敗。
- 主要音仍優先，次讀音只分類為 `listed_secondary`；單一 U+3105–U+3129 且 `reading == text` 才是 `bopomofo_literal`。其他無 readings 的候選固定為 `unexpected_missing_character`。
- 格式、UTF-8、重複、讀取失敗、明示空組混用及候選缺字使 `--check` 非零；裁決不能遮掉這些錯誤。
- decisions 只接受 `consistent`、`confirmed_error`、`intentional_difference`、`unresolved`；source 必須非空，重複及 orphan 失敗。`confirmed_error` 未移除或任何仍未裁決狀態都使 `pronunciation_review_complete=false`。
- `--check` 是不變量門禁，不是全表正音證明。僅有未知關聯時可回 0，stdout 必須是 `invariants=PASS; unresolved_relations=N; pronunciation_review_complete=false|true`。
- `--report` 先拒絕 resolved path 與任一 input 相同；核對與寫報告不得改任何 input bytes。

## Root 命令

初始 RED 命令（歷史 gate）：

```powershell
python -m unittest discover -s scripts/tests -p test_audit_readings.py -v
```

當時 `audit-readings.py` 尚不存在，因此 `setUpClass` 應明示因 module 缺失而 RED。Root 已保存此實際 RED；production 存在後仍使用同一測試命令確認 GREEN。

production 授權後的正式資料命令仍為：

```powershell
python scripts\audit-readings.py --repo . --report build\readings-audit.json --check
```

只有 `invariants=PASS` 不代表正音審查完成；必須同時讀 `pronunciation_review_complete` 與報告內容。

## 最小 production 交接

Root 已於 2026-11-27 09:21:54 UTC 保存真實 RED：test 檔 SHA-256 為 `965296fc94feba311c9b85adcf4a98dd66278b3a4ccd93038c75ded13d6df4aa`，exit 1，`setUpClass` 因缺少 `scripts/audit-readings.py` 明示 `EXPECTED RED`。這是 feature gate，0 個 individual method 被執行；不能據此宣稱各情境已 RED 或 GREEN。

此輪已依固定測試加入最小單一腳本、header-only decisions、基準觀測文件、changelog fragment 與既有 Windows job 的資料檢查／artifact。實作只使用 Python 標準庫、raw byte 比對與禁用 quote interpretation 的 TSV parser，不讀 repo 外路徑，也不修改任何 readings、dictionary、候選、generator 或 runtime。

**本輪未執行任何測試、build、install 或實際 audit。** Root 應先重跑既定 unittest 命令確認完整 GREEN，再在真實 repo 執行 `--check` 並保留 report。若 GREEN 成功，才可把 `docs/data/readings-audit.md` 的基準改寫成實際結果；在此之前不得宣稱 CI 或所有不變量已通過。

以上兩段是 production 授權前的狀態交接，後續 Root 已依同一契約執行實際驗證。

## Task D3 Root 實證

2026-11-27 Root 原生 Windows 證據為：

- 9 個 readings audit 合成 test cases GREEN；root `scripts/tests` Python discovery 另獨立通過 17 個 test methods。
- 完整真實資料 audit：`invariants=PASS; unresolved_relations=3738; pronunciation_review_complete=false`。
- 分類：14,172 primary、25 secondary、37 literal、3,738 unresolved、0 missing、4,644 explicit empty。37 個 literal 恰好覆蓋 U+3105..U+3129。
- 完整 unresolved 分析為 1,283 tone-only 與 2,455 syllable-difference；機械規則只移除 `ˊˇˋ˙` 並比較已列 readings，不是正音真值或 waiver。
- `data-full-analysis.json` 保存全部 3,738 筆；代表 secondary 為 `勒 / ㄌㄟ`，代表 unresolved 為 `阿 / ㄜ`。

基準 commit `a3f2c65` 是實際 structural FAIL，不是 invariant PASS：canonical LF SHA-256 `f2f5d289ccec051d2f7185b49ed4e9c7ce885f179bf4b781a5dba3a69a7c9da8`，Windows CRLF copy SHA-256 `b09b8f4b2a520908aeb63465ef7b2effbd580c61b7f883f76a8341cdf74fdffb`，產生 `copy_byte_mismatch`。六分類與逐項資料關聯與目前相同，故沒有資料語意改變；目前 Windows checkout 因 `autocrlf` 掩蓋 raw line-ending 差異。等待 Neon review 後的最小明確修正，此階段不改 assets、不標 resolved。

Root 證據 bundle 的 `data-green-native.json`、`data-full-analysis.json`、`data-baseline-report.json` 與 `data-green-native-report.json` 是本次來源；本文件更新沒有重跑程式或改寫證據。原生 C++ 與 Sandbox 回歸仍 pending，issue 19 保持 open；primary-only runtime、generator、讀音、dictionary 與候選資料均未改。

## Round 5 TESTS FIRST

Neon 接受的五個 bounded data-audit findings 已只以測試固定；本輪 worker 未執行測試、build 或 install，也未修改 production／assets／CI。Root 原先的 9 個 audit cases 與 17 個 root Python methods GREEN 是既有獨立證據，不包含這五筆新 regressions；不能把該舊 GREEN 延伸到下列新增契約。

新增的 exact contracts：

- `test_header_only_sources_and_dictionary_without_traditional_are_invalid`：candidates 只有 header 產生 `empty_candidates` line 1；dictionary 只有 header 或沒有 `TRADITIONAL` 產生 `empty_dictionary` line 1；三者皆 invariant FAIL 且 review incomplete。decisions 只有 header 仍有效。
- `test_tsv_headers_require_the_exact_declared_order`：candidates、dictionary、decisions 三種 TSV 都必須完全符合既定欄位順序；重排 header 即使內容仍可由 name map 讀到，也必須在 line 1 以 `missing_required_field` 失敗。
- `test_readings_character_is_exactly_one_non_whitespace_scalar`：`AB`、純空白與 `A ` 都在 canonical 與 Windows copy 產生 `invalid_character` line 5 並 FAIL；`U+20000` extension Han 單一 scalar 可通過。
- `test_invalid_utf8_reports_its_physical_line`：第二個以上行中的 UTF-8 錯誤，diagnostic line 必須是實際行 3，不可只寫 `null`；message 不含 temp 絕對路徑或時間。
- `test_repository_readings_are_byte_identical_lf_assets`：直接讀 repo 內兩個真正 readings 檔，不查 HEAD/git blob；兩者都必須不含 `CR` 且 bytes 相同。此測試在目前 CRLF Windows checkout 應先 RED，後續 production step 只會固定這兩條路徑為 LF 並正規化換行，不改資料列／排序。

Root RED 命令：

```powershell
python -m unittest discover -s scripts/tests -p test_audit_readings.py -v
```

預期 RED 至少包含上述 new contracts；目前 task 僅建立測試與本 handoff，沒有執行此命令，也未宣稱新測試已通過。Production 修正、line-ending attributes、asset normalization 與 CI committed-blob guard 均待 Root 另行授權。

schema v1 的 diagnostic code 集合隨本次回歸補上 `empty_candidates`、`empty_dictionary`、`invalid_character`；`invalid_utf8.line` 從舊測試允許 `null` 收緊為錯誤所在的 1-based physical line。Report 頂層與 diagnostic 欄位形狀不變。

## Round 5 最小 production 修正

Root 已於 09:57 UTC 跑完目前 14 個 data test methods：exit 1、12 個 assertion/subtest failures，沒有 missing tool 或 syntax RED；既有初始 9 個 cases 先前為 GREEN。失敗集中於空表、header 順序、非法 char、UTF-8 行號及真實 readings CRLF，不涉及 runtime/query/候選順序語意。

此輪最小修正：

- `audit-readings.py` 為空 candidates／無 `TRADITIONAL` dictionary 加 line 1 explicit diagnostics；decisions header-only 不受影響。
- TSV header 改為 tuple exact order，不再使用 set equality。
- readings char 驗證為 Python `len == 1` 且非 whitespace，extension Han scalar 保留。
- `UnicodeDecodeError.start` 前的 LF count 決定 1-based physical line；diagnostic 不洩漏路徑／時間。
- 只對兩個 readings 路徑加入 `text eol=lf`。Working bytes 的 CRLF→LF 寫入因本 worker shell formatter 權限被拒絕而尚未完成；`apply_patch` touch 保留 CRLF，不能冒稱已轉。後續只移除 CRLF 的 `CR`，row/value/order/BOM 語意不變；Root 須證明兩份 LF SHA-256 都是 `f2f5d289ccec051d2f7185b49ed4e9c7ce885f179bf4b781a5dba3a69a7c9da8`。
- 既有 Windows data step 在 audit 前用 `git rev-parse HEAD:<path>` 比較兩個 committed blob ID，明確檢查 exit code、non-empty 與 equality；audit script 本身仍只用 Python 標準庫。
- 真實 LF test 改成 boolean assertion + 短 path/message，契約不變，只避免失敗時傾印 658KB asset。

本輪未執行任何 test/build/install，也未執行 `HEAD` blob guard。Root 應重新跑：

```powershell
python -m unittest discover -s scripts/tests -p test_audit_readings.py -v
python scripts/audit-readings.py --repo . --report build/readings-audit.json --check
```

Root 另外獨立證明 working bytes、兩檔 SHA-256、before/after 欄位等價及 final committed blobs。此文件不宣稱 post-fix GREEN；3,738 unresolved（1,283 tone-only／2,455 syllable-difference）及 issue 19 open 狀態均未改。
