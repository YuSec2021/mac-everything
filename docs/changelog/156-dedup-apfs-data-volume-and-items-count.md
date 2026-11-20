# 156：修复 APFS Data 卷重复索引与状态栏计数文案

日期：2026-11-20

对应 [Issue #14](https://github.com/YuSec2021/mac-everything/issues/14)。版本保持 1.4.1（Build 5）。

## 问题背景

在 macOS APFS 卷组下，索引总数存在两类问题：

1. **APFS Data 卷重复索引**：`/System/Volumes/Data` 与 `/` 共享相同的 `st_dev`，而 `/` 下的 APFS firmlink（如 `/Users`、`/Applications`、`/Library`、`/private` 等）在 `getattrlistbulk(..., FSOPT_NOFOLLOW)` 下返回的是系统卷 stub 的 `fileid`，与 `/System/Volumes/Data/...` 返回的数据卷真实 `fileid` 不同，导致扫描器跨设备过滤与 inode 去重均未能阻止重复遍历 `/System/Volumes/Data`，同一目录树同时出现两份路径或与 FSEvents 路径并存。
2. **状态栏文案不准确**：状态栏原先显示 `files indexed`，但统计口径是全部索引条目（包含普通文件、文件夹、符号链接、`.app` 包等），容易误解为纯文件数。

## 主要改动

- **扫描与监听跳过 `/System/Volumes/Data`**：
  - `PathUtils::isAPFSDataVolumePath` 统一识别 `/System/Volumes/Data` 及其子路径。
  - `DirectoryScanner` 从根路径或上级目录扫描时直接跳过 `/System/Volumes/Data`，不再进入数据卷挂载点重复扫描。
  - `FileSystemWatcher`、`ServiceEngine::collectFSEventMutations` 和 `ServiceEngine::rescanDirectoryInto` 过滤 `/System/Volumes/Data` 路径，防止实时事件或子树重扫再次写入重复路径。
- **旧缓存自动后台重扫去重**：
  - `SearchEngine::hasPathPrefix` 检测已加载缓存中是否残留 `/System/Volumes/Data` 记录；启动检测到旧缓存含重复路径时，先秒开展示已有索引，同时在后台自动执行全量重扫以完成去重替换。
- **状态栏文案更正**：
  - `ContentView.swift` 状态栏由 `files indexed` 改为 `items indexed`（与扫描中的 `items scanned` 保持一致），并同步更新 UI 测试断言。
- About 版本说明与变更日志同步更新。
