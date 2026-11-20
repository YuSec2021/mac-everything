# 155：Rebuild Index 丢弃全部索引缓存并完整扫描

日期：2026-11-20

对应 [Issue #16](https://github.com/YuSec2021/mac-everything/issues/16)。版本号与构建号更新为 **1.4.1（Build 5）**，以便与不包含并发压缩修复和重建修复的 `v1.4.0（Build 4）` 明确区分。

修复 Rebuild Index 无法恢复损坏索引的问题。

原因：`SearchViewModel.rebuildIndex` 只删除 `index.bin`、`index.wal`、`index.pages`、`index.ptable`，没有删除当前实际使用的 `index.v6`。引擎重新启动后读到 `index.v6`，判定缓存有效，直接加载原来的数据，不做全盘扫描。旧的持久化对象在重建期间也仍然存活，其定时落盘可能写回旧数据；同时，全量扫描完成后调用 `flush(..., force: true)` 在空 WAL 状态下会被跳过，且尚未收到 FSEvents 时 `lastEventId=0`，导致新缓存未能立即落盘供重启命中。

- 新增 `ServiceEngine::rebuildIndex`，按顺序执行：停止监听，等待排队中的目录重扫结束，停止并释放旧持久化，清空内容索引，删除全部索引缓存，再走无缓存启动的全盘扫描。
- 全量扫描与后台全量重扫完成后改用 `IndexPersistence::fullCompact`，并在 `buildMetadata()` 中对 `lastEventId == 0` 兜底取 `FileSystemWatcher::getCurrentSystemEventId()`，确保重建完成后新的 `index.v6` 立即落盘且下次启动直接命中缓存。
- 内容索引按记录编号关联文件，全盘扫描会重新分配编号，因此一并清空并重新生成；内容搜索设置保留。
- 扫描或启动同步进行时拒绝重建，并写入日志；菜单、搜索面板、HTTP 接口和 daemon 都走同一入口。
- `POST /api/index/rebuild` 在 App 中此前只发送无人监听的通知，返回 202 但不执行，现在会真正重建；daemon 也不再绕过持久化直接扫描。
- 启动同步的 `isSyncing` 改为在持久化就绪后才清除，避免重建与启动收尾交错。
- 将 `MacEverything/Info.plist` 版本号递增为 `1.4.1`（Build 5），并同步更新 About 版本说明。

测试：Part 40 新增 13 项，用一份损坏的 `index.v6` 启动，确认重建后执行全盘扫描、损坏记录消失、磁盘文件可搜索、新的 `index.v6` 立即写入磁盘、内容缓存被丢弃、重启直接加载新缓存（无需再次全量扫描）且损坏记录不会回来，并且重建进行中第二次请求被拒绝；同时验证 Part 3d 并发压缩防误删测试通过、Debug 构建成功。
