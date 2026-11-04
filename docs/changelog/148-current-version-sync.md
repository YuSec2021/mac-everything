# 148：同步当前版本说明与近期变更

日期：2026-11-04

当前应用版本：1.2（Build 3）

汇总截至当前分支在 2026-10-22 之后的变更，并更新 About 版本说明（搜索界面相关改动包含尚在审查的 PR #6）：

- 应用改为常驻菜单栏，搜索面板支持分页、快捷键冲突检查和图标不可见时的浮动面板；新增 Quick Look、品牌图标和 About 窗口。
- 搜索界面采用 Observation 和原生列表操作，并修复使用 Command 键定位 Finder 时的事件读取。
- 修复 FSEvents 目录变更及根目录重扫时的记录更新；按 WAL 增长限制全量索引重写，减少写放大。
- 推送版本标签时自动构建 Apple Silicon 和 Intel DMG，并发布 SHA256 校验和。
- 建立项目更新时同步维护 About 和 changelog 的流程。
- 新增 PR Build 工作流：每次 PR 更新时分别构建 arm64 和 x86_64 DMG，复用发布脚本验证二进制架构、系统库依赖及签名；仅验证构建，不发布 Release。构建任务限时 30 分钟，新提交会取消同一 PR 的旧任务。

此前的大索引内存和查询优化详见 [146](146-resident-memory-optimization.md) 和 [147](147-query-and-prefix-removal.md)。版本和构建号来自应用的 Info.plist，本次未修改。
