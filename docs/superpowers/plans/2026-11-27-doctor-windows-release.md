# Windows Doctor 發行驗證

關聯議題：https://github.com/hamanpaul/liu-kai/issues/11

Android 已由同一議題先行發布 0.1.1。Windows 以 Android 合併提交為基礎，只整合 Windows 修改，目標產品版號 0.1.1、MSI／Burn 70.1.3、repo 批次 0.1.4。

- [x] 保留既有 TDD、八輪 review、原生測試與單一沙盒候選驗證。
- [x] 一小時穩定性、故障恢復、容量、清除及候選安裝／卸載驗證，範圍見 Windows 驗證紀錄。
- [x] 原版未增加 QPC metadata 的測試工具完成三對 OFF／ON；每輪 775 對按鍵、260 字、2322 個樣本。p99 差為 −39.4／−106.8／+335.9 µs，各小於初始 500 µs 目標。舊高負載反例保留。
- [x] 額外插樁的單對 ETL 只作診斷，不混入正式三對或推定唯一根因。
- [x] 最新來源的本機發行檢查、canonical preflight、GitHub CI 與 review 修正。
- [x] 從實際合併提交產出正式包，下載核對，再以該成品驗證安裝、升級、保留及卸載。
- [x] 補齊可用環境的 Search、Store、Terminal／WSL 與實體輸入驗證；範圍與 Unknown 限制如實記錄。
- [x] 通過後發布平台標籤、EXE、SOURCE.txt、SHA256SUMS.txt，回讀下載雜湊並更新共用議題。

正式來源 `a3f2c65cea6db99e1a4cf3e7e468d6296621296f`，EXE SHA-256 `ba32e8e62dd4eddad057ca44218ea96860f94afaec86d026a7471515baef43ae`。本機 R70 升級成功、回傳 3010；未重開主機，核對新測試宿主實際載入新版 DLL。使用者已完成兩個 Terminal 分頁及 Search／Store 的實體測試並回覆正常；輸入收據與匿名事件另行核對。完整範圍見 [正式版驗證紀錄](../../../windows/VALIDATION.md)，公開附件見 [Windows 0.1.1](https://github.com/hamanpaul/liu-kai/releases/tag/windows-v0.1.1)。

本計畫不將候選包的 SHA 冒稱為正式包，不以一次插樁診斷或安靜環境結果取代其他負載的反例。八小時記憶體觀察維持非阻擋，已完成的一小時不改寫為八小時。
