# Windows Doctor 發行驗證

關聯議題：https://github.com/hamanpaul/liu-kai/issues/11

Android 已由同一議題先行發布 0.1.1。Windows 以 Android 合併提交為基礎，只整合 Windows 修改，目標產品版號 0.1.1、MSI／Burn 70.1.3、repo 批次 0.1.4。

- [x] 保留既有 TDD、八輪 review、原生測試與單一沙盒候選驗證。
- [x] 一小時穩定性、故障恢復、容量、清除及候選安裝／卸載驗證，範圍見 Windows 驗證紀錄。
- [x] 原版未增加 QPC metadata 的測試工具完成三對 OFF／ON；每輪 775 對按鍵、260 字、2322 個樣本。p99 差為 −39.4／−106.8／+335.9 µs，各小於初始 500 µs 目標。舊高負載反例保留。
- [x] 額外插樁的單對 ETL 只作診斷，不混入正式三對或推定唯一根因。
- [ ] 最新來源的本機發行檢查、canonical preflight、GitHub CI 與 review 修正。
- [ ] 從實際合併提交產出正式包，下載核對，再以該成品驗證安裝、升級、保留及卸載。
- [ ] 補齊可用環境的 Search、Store、Terminal／WSL 與實體輸入驗證；未驗項目不勾選。
- [ ] 通過後發布平台標籤、EXE、SOURCE.txt、SHA256SUMS.txt，回讀下載雜湊並更新共用議題。

本計畫不將候選包的 SHA 冒稱為正式包，不以一次插樁診斷或安靜環境結果取代其他負載的反例。八小時記憶體觀察維持非阻擋，已完成的一小時不改寫為八小時。
