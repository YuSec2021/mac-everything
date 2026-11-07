# 151：发布 1.4.0

日期：2026-11-07

应用版本更新为 1.4.0（Build 4），并同步 README 和 About 中的版本信息。此前 `Info.plist` 一直写的是 1.2（Build 3），v1.3.0 的 DMG 版本号由发布脚本按标签写入，本次起源码中的版本与发布标签一致。

自 v1.3.0 以来的主要变更：

- 应用改为常驻菜单栏，搜索面板支持分页、快捷键冲突检查和图标不可见时的浮动面板；新增 Quick Look、品牌图标和 About 窗口（见 [148](148-current-version-sync.md)）。
- 搜索面板改为液态玻璃风格（见 [149](149-liquid-glass-prototype.md)、[150](150-liquid-glass-search-ui.md)）。
- 搜索界面采用 Observation 和原生列表操作，修复使用 Command 键定位 Finder 时的事件读取。
- 每次 PR 更新时分别构建 arm64 和 x86_64 DMG 进行验证。

目录重扫与索引写入修复、大索引内存与查询优化（[146](146-resident-memory-optimization.md)、[147](147-query-and-prefix-removal.md)）已包含在 v1.3.0 中。

合并后推送 `v1.4.0` 标签，由 Release 工作流构建两种架构的 DMG 并发布到 GitHub Releases。
