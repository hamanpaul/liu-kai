# Doctor v1 合成測試資料

JSONL 是固定白名單事件，僅使用合成時間、識別碼與狀態。`platform-result.jsonl` 包含 signed32 HRESULT，不包含錯誤訊息。

`export-manifest.json`、`export-mark.json` 定義空快照的共同欄位與 canonical bytes（ZIP 內 JSON 不帶結尾換行）。`export-contract.zip` 以這兩份 JSON 去除檔尾換行，加上空的 `doctor/events-v1.jsonl`，以 ZIP STORE、1980-01-01 固定 ZIP 時間建立。兩端 writer 需產生相同 metadata，reader 也必須能讀取這份相同 ZIP。

`export-unknown-field.zip` 僅在 manifest 額外加入固定合成 `text` 欄位，reader 必須拒絕預覽與儲存，避免匯出夾帶白名單以外內容。這些 ZIP 沒有實際使用者輸入。

`export-bad-crc.zip` 更動合法版本文字但保留舊 CRC；`export-deflated.zip` 使用契約以外的 DEFLATE。兩者必須拒絕，讀取前亦須檢查 entry 的方法及大小，避免接受損壞內容或無界解壓縮。
