# 157：搜索面板右上角新版本提示与 Release 安装包下载

日期：2026-11-20

关联需求：[#18](https://github.com/YuSec2021/mac-everything/issues/18)

- 新增 `MacEverything/App/ReleaseUpdateManager.swift`：
  - 启动后异步请求 `YuSec2021/mac-everything` 的 GitHub Releases 接口并按 TTL（6 小时）缓存检查结果，不阻塞启动、索引与搜索。
  - 实现 `ReleaseVersion` 数值版本比较，兼容 `v` 前缀及 `1.9.0 → 1.10.0` 等多段比较，自动跳过 `draft`、`prerelease` 及预发布后缀 tag；当前版本或远端旧版不显示更新提示。
  - 自动识别 Apple Silicon（`arm64`，含 Rosetta 宿主检测）与 Intel（`x86_64`），按 `MacEverything-<版本>-<架构>.dmg` 从 Release asset 获取真实下载地址。
  - 使用 `URLSessionDownloadTask` 下载安装包到用户下载目录（`~/Downloads`），实时汇报进度并防止重复下载；中断、非 2xx 响应、空文件或字节数不匹配不会标记为完成，下载成功后才原子移入下载目录并避免覆盖同名文件。
- 新增 `MacEverything/App/UpdateActionView.swift` 并接入 `MacEverything/App/AppDelegate.swift` 的 `MenuBarSearchView` 顶部右侧（齿轮设置按钮左侧），菜单栏弹出搜索面板与备用浮动搜索面板共用同一视图：
  - 无更新时隐藏；有新正式版本时以液态玻璃胶囊按钮显示目标版本（如「下载更新 v1.5.0」），附带提示和可访问性标签。
  - 下载中展示进度并禁止重复触发；完成后提供「打开安装包」与「在 Finder 中显示」入口；网络错误、下载失败或缺少当前架构安装包时展示明确原因并支持一键重试。
- 新增 `tests/test_release_update.swift` 与 `Makefile` 的 `test-release-update` 目标，覆盖版本比较、Release 筛选、双架构资产匹配、缓存与静默失败冷却、防重复下载、中断恢复及缺包重试。
- 按 `AGENTS.md` 递增版本：`MacEverything/Info.plist` 更新为 `1.5.0`（Build `6`），并同步更新 `MacEverything/App/AboutView.swift` 与 changelog 索引。

验证：
- 运行 `make test-release-update`，32 项断言全部通过。
- 运行 `make app` 验证完整 Xcode Release 构建通过。
