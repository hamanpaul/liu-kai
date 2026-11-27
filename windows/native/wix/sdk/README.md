# WiX 6.0.2 BAFunctions SDK headers

僅保留本安裝擴充需要的四個官方 ABI header，內容未修改；不包含 WiX 引擎、BA 實作或其他 SDK。
來源固定為 `wixtoolset/wix` 的 `v6.0.2`，逐檔 upstream 路徑、Git blob 與 SHA-256 見 [SOURCE.json](SOURCE.json)。
完整 MS-RL 授權文字見 [LICENSE.TXT](LICENSE.TXT)，原始 copyright 標示保留在每個 header。

使用原宣告，禁止另抄 enum、vtable 或 struct。MinGW 的舊 SAL 註記相容層放在本專案呼叫端，以 macro push/pop 隔離；不修改這些 header。
建置前應驗證 SOURCE.json 列出的四個 SHA-256。`.gitattributes` 固定此目錄的 LF，避免 Windows checkout 改動已核對的來源 bytes。
升級 WiX 時一併重新核對官方來源、版本、ABI、建置及正常／重入／中斷／相關 bundle 的實測。
