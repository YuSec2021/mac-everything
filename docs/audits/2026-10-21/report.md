# MacEverything 代码、安全与后台资源检查

检查日期：2026-10-21（Asia/Shanghai）。基于提交 `26f0332c0576ff5b3c73f4efdc5fc8a1da6493c4` 和本次未提交修改。
使用 ponytail 默认 full 工作方式，针对已证实的问题做局部修复。

## 结论

当前仓库的应用代码中未发现公网上传、遥测、下载执行、反向 shell、挖矿、远程命令控制或隐藏启动项逻辑。运行期短时网络快照也未发现外连。这个结论仅覆盖所审源码和本机构建的程序，不能证明其他下载版本或整台主机没有恶意软件。

**仍有一项重要风险：本地 HTTP API 没有客户端身份认证。** 绑定 `127.0.0.1` 只能限制远端直接连接；本机其他进程（包括其他登录用户的进程）仍可能查询文件路径、最近文件、已开启内容索引的片段，以及调用重建和配置接口。本次加固了网页访问边界，未实现本机进程间的权限隔离。进一步处理需要为 API 和 MCP 同步增加鉴权，或改用受文件权限限制的 Unix socket。

## 已修复的问题

| 位置 | 原有问题 | 本次处理 |
|---|---|---|
| `ServiceEngine+Content.cpp` | 默认未开启内容索引也会遍历整个文件索引、分配全部路径、调度逐文件工作；空内容索引仍构建全量清理集合 | 禁用时直接完成；空内容索引跳过无用清理 |
| `HttpServer.cpp/.h` | 空闲每 500ms poll 唤醒；退出时监听描述符存在并发读写 | 阻塞 poll，由 pipe 唤醒退出，线程结束后关闭描述符 |
| `SearchViewModel.swift` | 每行重复解析同一查询的高亮 AST | 按完整查询缓存高亮结果，选项变化自然失效 |
| `SearchViewModel.swift` | 新刷新定时任务覆盖旧任务，取消后的 sleep 仍可能进入刷新逻辑 | 先取消旧任务，唤醒后检查取消状态 |
| `HttpServer.cpp` | 无效或溢出的 Content-Length、配置数字可能抛异常终止进程；请求分包处理不完整；超大请求和慢连接边界不足 | 支持分包；限制 8KiB 头、64KiB 正文；严格长度解析；拒绝重复长度和 Transfer-Encoding；读取总期限和发送超时；防 SIGPIPE；数字异常返回 400 |
| `HttpServer.cpp` | 未验证 Host 和浏览器来源，存在网页跨站修改与 DNS rebinding 访问风险 | 仅允许本机 Host；拒绝 Origin 和 Sec-Fetch-Site 请求头；不开放 CORS |
| `ServiceEngine.cpp` | 持有引擎锁等待 HTTP 线程退出，而请求可能正在等待同一把锁；服务启动后再设置管理回调存在竞争 | 锁外停止服务；启动前安装回调，已运行时直接返回 |
| `Makefile` | 新版 Homebrew RE2 依赖 Abseil 头文件，但 CLI 构建找不到 | 补齐 Homebrew include 路径 |

## CPU / GPU

隔离测量使用与 GUI 共用的 ServiceEngine 守护进程，索引 **1000 个合成文件**，内容索引关闭。缓存和日志都放在临时目录，测量后终止进程并清理；没有启动全盘扫描或安装登录项。

- 测量时长：20.30 秒。
- 累计 CPU 时间：0.145 ms，折合单核平均 **0.000716%（低于 0.01%）**。
- 计时采用 `proc_pid_rusage` 的 user/system ticks，并按本机 `mach_timebase_info` 的 125/3 转为纳秒；不是把 ticks 直接当纳秒。
- 结束时 RSS：28.03 MiB。
- 2 秒线程采样中，HTTP 线程一直停在 `poll`，未见忙循环。正常 SIGTERM 退出，退出码 0。
- 源码未发现自定义 GPU 计算、Metal 渲染循环或永久动画；SwiftUI、材质效果和 ProgressView 仍可能使用系统 GPU 合成。
- **未测量 GUI 隐藏/最小化后的 GPU 占用，也未测量全盘索引、大量 FSEvents 或开启内容索引时的资源上限。不能据此宣称实际 GUI 后台 GPU 为 0。** 初次扫描使用 4–32 个线程，内容索引采用并行处理，工作期间 CPU 升高有明确代码路径。文件监控在后台继续运行，焦点节流只限制 UI 搜索刷新。

原始指标见 [runtime.json](runtime.json)，线程栈见 [idle-stacks.txt](idle-stacks.txt)。可从仓库根目录运行 `make daemon` 后执行 `python3 docs/audits/2026-10-21/measure_runtime.py` 复测；脚本使用测试端口 19889，并将结果写到 `/tmp/maceverything-runtime-audit.json`。

## 网络与启动行为

- 应用 HTTP 服务只绑定 `127.0.0.1:19860`；测试改用 `19889`。MCP 代理仅连接 `127.0.0.1`，结果经 stdio 返回调用方。
- 四次 `lsof -nP -a -p PID -i` 快照均仅有测试端口的 loopback LISTEN，没有已建立的公网 TCP 或 UDP socket。这是快照观察，不是覆盖全部时间和所有网络机制的抓包证明。
- 默认内容索引扩展名为空，需配置后才读取文件内容做索引。文件索引、WAL、搜索历史、日志写入本地；本地存储不等于经过权限隔离或加密。
- 开启 MCP 后，文件路径和内容片段可返回 Claude/Cursor 等客户端。依据代码的数据流，这些客户端可能再把结果交给其模型服务；这是外部客户端行为的风险判断，本次没有审计它们或证明它们实际上传了数据。
- 开机启动通过用户菜单操作调用 `SMAppService.mainApp.register/unregister`；未发现自动安装 LaunchAgent、LaunchDaemon、cron 或额外 helper 的代码。
- 全局快捷键使用 Carbon 的 `RegisterEventHotKey`，未见全键盘记录逻辑。打开文件、打开应用和 Finder 定位均来自结果行的用户操作。
- 应用 sandbox 关闭，并引导用户授予完全磁盘访问权限；这些权限使 API 泄漏的影响范围较大。

## 验证与边界

- `make test_all daemon lint-bridge` 构建通过。
- `./test_all --fast`：**12,013 项通过，0 失败**；包含 HTTP、ServiceEngine、daemon、MCP 和索引回归。
- HTTP / ServiceEngine / daemon / MCP 定向测试：71 项通过，0 失败（已包含在上面的快速测试中）。新增覆盖非法/溢出/重复 Content-Length、正文截断、超大请求、浏览器请求、非法 Host、分包、服务重启和空闲退出。
- Swift 刷新状态机测试：44 项通过，0 失败。
- Xcode Release 构建通过，使用 `CODE_SIGNING_ALLOWED=NO`；这是本机测试构建，没有生产签名或公证。现有 CompiledGlob 初始化警告仍在。
- 为构建已安装仓库声明使用的 Homebrew `re2` 及其依赖 `abseil`；这些构建依赖下载属于本次开发操作，不是应用后台上传。
- 未进行下载发行包比对、依赖源码完整审计、全盘恶意软件扫描、GUI 自动化、长时间网络抓包、GPU 性能录制或 ASan/TSan 检查；本次结果不构成无木马或绝对安全保证。
