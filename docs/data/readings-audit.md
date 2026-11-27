# 讀音與候選資料核對

狀態：2026-11-27 原生 Windows 已通過全部 14 個資料稽核測試及完整資料檢查。Neon 指出的五個格式／診斷缺口均先重現失敗，再修正至通過，round 6 複審通過。Windows 沙盒的既有完整原生回歸亦通過，包含同音／注音、主要音、候選字碼與英文組字；新的 LF 資產已在同一沙盒重跑 14 個稽核測試、完整 audit 與既有原生同音／注音測試並通過。新安裝成品的代表字端到端驗證仍待完成。正音仍有 3,738 筆 unresolved，`pronunciation_review_complete=false`。

這份核對是資料集合與同步狀態的證據，不是正音結論。兩份 readings 僅統一為 LF；讀音欄值、列序、generator、dictionary、候選資料、主要音規則與 runtime 均維持原樣。[issue 19](https://github.com/hamanpaul/liu-kai/issues/19) 保持 open。

## 輸入與邊界

核對工具只讀以下 repo 內路徑：

- canonical：`core/src/main/resources/readings.tsv`
- Windows copy：`windows/assets/readings.tsv`
- 候選觀察：`windows/assets/phonetic-candidates.tsv`
- active dictionary：`windows/assets/dictionary.tsv`
- 逐項裁決：`docs/data/readings-decisions.tsv`

canonical 與 Windows copy 必須 byte-identical；BOM、換行或列序差異不能靠解析正規化消除。候選表是既有觀察與輸出排序資料，不是正音真值；工具不改 generator、readings、dictionary、候選資產或 runtime 的主要音規則。

字典只投影 `TRADITIONAL` section 中與候選 `text` 完全相同的文字。它表示目前 active dictionary 是否包含該文字，不做簡繁轉換，也不把可用性當成讀音真偽。

## 目前 Windows worktree 實際結果

目前原生 Windows checkout 的完整 audit 為：

```text
invariants=PASS; unresolved_relations=3738; pronunciation_review_complete=false
```

候選表有 5,975 個讀音組、17,972 個非空候選關聯及 4,644 個明示空組：

| 分類 | 實際關聯數 | 意義 |
| --- | ---: | --- |
| `primary_match` | 14,172 | 現行主要音相符 |
| `listed_secondary` | 25 | 讀音列有此次讀音，不改 query 行為 |
| `bopomofo_literal` | 37 | 單一 U+3105–U+3129 符號，且 reading 與 text 相同；37 項恰好逐一覆蓋整個區間 |
| `unresolved_relation` | 3,738 | 候選觀察未列為該字讀音，仍待具名裁決 |
| `unexpected_missing_character` | 0 | 缺 readings 的非注音 literal；任何新增都會使 `--check` 失敗 |
| `explicit_empty` | 4,644 | 保留候選資產的明示空組 |

非空分類合計仍是 17,972；數字是本次觀測，不寫入程式常數。`unexpected_missing_character=0` 不代表 3,738 個未知關聯已正確，它只代表沒有額外的非注音候選缺 readings。

代表關聯：

- `勒 / ㄌㄟ` 是 `listed_secondary`：canonical 已有主要音 `ㄌㄜˋ` 與次音 `ㄌㄟ`，候選表也觀察到 `ㄌㄟ / 勒`；runtime 仍只使用主要音。
- `阿 / ㄜ` 是 `unresolved_relation`：canonical 只列 `ㄚ`，候選表另有 `ㄜ / 阿`；這是待查證的資料差異，不可直接改成真值或刪除。

2026-11-27 另以教育部官方條目抽查三字，將「有此讀音」與「產品主要音／來源如何修正」分開：

| 代表字 | 目前 canonical 列 | 官方來源所列讀音 | 本輪處理 |
| --- | --- | --- | --- |
| 勒 | `ㄌㄜˋ ㄌㄟ` | [《國語辭典簡編本》2021，ID 13223](https://dict.concised.moe.edu.tw/dictView.jsp?ID=13223&la=0&powerMode=1) 列兩音 | 讀音集合有官方依據；不推論所有次音都應可直接查字 |
| 都 | `ㄉㄨ ㄉㄡ` | [《國語小字典》，D0003811](https://dict.mini.moe.edu.tw/SearchIndex/word_detail?breadcrumbs=Search_%E9%83%BD_one&dictSearchField=%E9%83%BD&wordID=D0003811) 列 `ㄉㄡ`、`ㄉㄨ` | 官方條目的列序不能代替本產品主要音政策；維持既有 `ㄉㄨ` 主要音 |
| 阿 | `ㄚ` | [《國語小字典》，D0001869](https://dict.mini.moe.edu.tw/SearchIndex/word_detail?breadcrumbs=Search_%E9%98%BF_one&dictSearchField=%E9%98%BF&wordID=D0001869) 列 `ㄜ`、`ㄚ` | `ㄜ` 並非無來源的讀音；canonical 的來源涵蓋與修正方式仍待裁決，不把缺列自動視為刻意相容 |

這是具名來源核對，沒有新增裁決列或改寫資料。「阿／ㄜ」仍留在原有 3,738 筆未裁決集合，不能因查到單一官方讀音就聲稱全表正音完成。代表字的已註冊輸入法端到端驗收仍須與新安裝成品一起執行。

## 完整未裁決集合

Root 的 `data-full-analysis.json` 保存全部 3,738 筆未裁決關聯與基準／目前比較。機械分組是：

| 分組 | 筆數 | 機械規則 |
| --- | ---: | --- |
| tone-only | 1,283 | 僅移除 `ˊˇˋ˙` 後，候選 reading 與某個已列 reading 相同 |
| syllable-difference | 2,455 | 不是上一組，移除 `ˊˇˋ˙` 後仍與所有已列 reading 不同 |

移除的範圍只包含上述四個字元，分類只用於檢視順序；它不是正音判斷、相容性 waiver，也不會自動新增或改寫 readings。基準與目前的六分類及逐項關聯相同，未判定總數仍是 3,738。

## 基準與目前的差別

不可把兩者都說成 byte-identical：

- 基準 git source `a3f2c65` 的 canonical 為 LF，SHA-256 `f2f5d289ccec051d2f7185b49ed4e9c7ce885f179bf4b781a5dba3a69a7c9da8`；Windows copy 為 CRLF，SHA-256 `b09b8f4b2a520908aeb63465ef7b2effbd580c61b7f883f76a8341cdf74fdffb`。
- Root 對該基準的實際 audit 是 **FAIL / `copy_byte_mismatch`**。兩端文字關聯分類仍完全相同，這次沒有資料語意差異。
- `autocrlf` 曾讓目前 checkout bytes 掩蓋基準的歷史差異；這不能倒推基準 raw bytes 一致。
- 兩個 readings 路徑已用 `.gitattributes` 固定為 `text eol=lf`，並實際完成 CRLF→LF。逐列與欄值比較一致；兩份檔案均為 614,553 bytes、SHA-256 `f2f5d289ccec051d2f7185b49ed4e9c7ce885f179bf4b781a5dba3a69a7c9da8`。格式修正後全部 14 個測試與完整 audit 通過。

## 報告與裁決

完整 report 為 deterministic UTF-8 JSON，包含輸入 SHA-256、diagnostics、分類筆數與逐項關聯，不含時間、絕對路徑或 host 資訊。`--check` 只驗證資料不變量；它可以在未知關聯仍存在時回 0。stdout 固定回報：

```text
invariants=PASS; unresolved_relations=N; pronunciation_review_complete=false|true
```

`pronunciation_review_complete=true` 只表示所有未知關聯都已有 `consistent` 或 `intentional_difference` 的具名裁決，且沒有格式、缺字或 `confirmed_error`／`unresolved` 阻擋。未知關聯沒有消失、只是仍待裁決時必須保持 false。

`docs/data/readings-decisions.tsv` 目前只建立欄位，不預填 3,738 條免審。可接受狀態只有 `consistent`、`confirmed_error`、`intentional_difference`、`unresolved`；每筆需要非空 source，且必須對應 canonical 或候選輸入中的 `(char, reading)`。決策不能遮掉格式或缺字錯誤。

## Root 證據與重跑

Root 授權的 synthetic/project 證據位於 `installer-readings-implementation-20261127`：

- `data-green-native.json`：初始 9 個合成 cases GREEN 及當時完整 audit。
- `data-full-analysis.json`：基準／目前完整比較與 3,738 筆 unresolved 清單及機械分組。
- `data-baseline-report.json`：基準 `copy_byte_mismatch` report。
- `data-green-native-report.json`：目前 PASS／review incomplete report。
- `data-review-red.json` 與 log：round 5 新契約的 14-method 實際 RED 證據。
- `readings-format-normalization.json`：格式修正前後 SHA-256、bytes 與欄值／列序等價證明。
- `data-green-native-3.json` 與 `data-green-native-3-report.json`：格式修正後 14 個測試及完整 audit 通過；3,738 未裁決項目保持不變。
- `validation/sandbox-original/output/native-regression-2-result.json`：沙盒既有 Windows 完整原生測試、TLS、retired-files 與 legacy 安全回歸通過；該輪使用基準的 CRLF 資產。
- `validation/sandbox-original/output/data-guest-result.json`：同一沙盒使用最終 LF 資產，14 個資料測試、完整 audit 與既有原生同音／注音測試通過；未將此結果冒稱已驗證實際註冊輸入法 UI。

重跑命令：

```powershell
python -m unittest discover -s scripts/tests -p test_audit_readings.py -v
python scripts/audit-readings.py --repo . --report build/readings-audit.json --check
```

當前資料 gate 以第二個命令的 invariant exit code 為準；仍須另讀完整 report 與 `pronunciation_review_complete`。Windows CI 另外直接比較 `HEAD` 中兩個 readings blob object ID，阻止 checkout EOL 轉換掩蓋 committed bytes 漂移。已知 3,738 unresolved、review incomplete 仍是預期，不可用資料 gate 關閉 issue 19。最終提交的 blob 檢查、新 LF 資產與安裝成品的沙盒驗證完成後，才補上對應結果。
