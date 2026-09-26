---
type: feat
---
新增 TestPilot plugin（`testpilot/`，54 個 YAML 案例、JSON／Markdown 報告，以及與官方嘸蝦米對照的 `liu-kai-diff`），以 TDD 重構並補齊測試：core／cli／plugin／app 覆蓋率門檻皆為行＋分支 100%；IME 決策邏輯移入 core 的 `ImeController`，app 改為 minSdk 30，IME 狀態由 `dump()` 診斷輸出，淘汰 androidTest。
