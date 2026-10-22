# 分配器缓存、32 位路径 hash 与列收窄

日期：2026-10-22。基线为 win-inspired 一轮的最终版本。v6 磁盘格式不变，未新增依赖。

## 起因

在同一份固定快照上测得：索引就绪时物理足迹约 1260 MiB，但 malloc 实际在用只有 860.5 MiB。vmmap（vmmap-before.txt）显示 Malloc Large (empty) 有 361 MiB 脏页：加载和 Phase 2 期间的大块临时缓冲释放后，被 libmalloc 的 large cache 留在进程内。macOS 27.0.1 上，`malloc_zone_pressure_relief` 无法归还这部分内存，只有在进程启动时设置 `MallocLargeCache=0` 才能归还（envcheck.txt：释放 722 MiB 后，足迹 722 → 1 MiB）。

## 改动

- **分配器**：Info.plist 增加 `LSEnvironment`，内容为 `MallocLargeCache=0`。daemon 不经 LaunchServices 启动，因此在 main 开头调用 `me::ensureMallocLargeCacheDisabled`：没有设置时写入环境变量并 re-exec 一次；用户显式设置的值保持不变。用最小测试 bundle 经 `open` 启动，确认 LaunchServices 会把该变量传给 libmalloc。该开关未写入 man 手册；如果以后失效，只会退回原来的内存占用，不影响正确性。
- **32 位路径 hash**：`CompactPathIndex` 改为 `btree_multimap<uint32_t, uint32_t>`，统一通过 `me::PathHash` 把 64 位 hash 折叠为 32 位。改动覆盖 `pathIndex_`、两张目录查找表和目录树的边。碰撞仍由完整路径比对来区分；三处直接 emplace 的调用点也改用同一个 hash，避免插入和查找的键不一致。
- **列收窄**（`NarrowColumn.h`）：
  - `sizes_` 和 `modTimes_` 改用 `uint32_t` 存储。装不下的值（以及等于转义值的值）写入稀疏 B-tree 旁表，按记录号保存精确值。
  - `devIds_` 改用 1 字节字典编码；超过 255 种取值后写入旁表。
  - 保存时以 8192 条为一块展开成宽类型写出，CRC 逐块累计，所以磁盘字节与原来相同。
  - `inodes_` 本轮未改。

## 固定快照对照

输入：/tmp/maceverything-memory-2/index.v6，只读。共 6,460,706 个记录位置，其中 6,381,948 条有效记录，707,177 个目录。SHA-256 前缀 46b8eb2f7e52153d。三组为顺序单次运行。单位 MiB。

| 指标 | 修改前 | 仅结构改动（默认分配器） | 修改后（部署配置） |
|---|---:|---:|---:|
| 索引就绪物理足迹 | 1266.88 | 1145.74 | 708.25 |
| 加载完成（Phase 2 前）足迹 | 865.0 | — | 473.2 |
| malloc 在用 | 860.5 | — | 703.7 |

就绪足迹减少 558.6 MiB（44.1%）。其中结构改动约 121 MiB，其余来自分配器。

| 结构（malloc 在用） | 前 | 后 |
|---|---:|---:|
| pathIndex_ | 130.4 | 63.9 |
| 目录查找表 ×2 | 28.9 | 14.2 |
| pathPool_（含边 B-tree） | 46.7 | 39.3 |
| 每条记录的固定字段（SoA） | 203.4 | 135.6 |
| 其中 sizes / modTimes / devIds | 49.3 / 49.3 / 24.6 | 24.6 / 24.6 / 6.2 |

旁表实际占用：75 个超过 4 GiB 的文件；修改时间 0 条；设备号字典 10 项（含墓碑的 0），旁表 0 条。

## 查询

每条查询预热一次、再测五次取中位数，结果上限 10,000，单位 ms。16 条查询的结果数量和有序 ID 哈希，在三组运行中全部一致；14 项快速路径与完整扫描的对照全部相等。

| 查询 | 前 | 后 |
|---|---:|---:|
| `readme` | 5.48 | 5.53 |
| `config` | 9.61 | 9.59 |
| `size:>1mb` | 2.19 | 2.01 |
| `type:folder` | 1.72 | 1.91 |
| `/Applications/` | 5.31 | 4.88 |
| `MacEverything/a` | 30.68 | 29.72 |
| `my_product/a` | 50.15 | 51.28 |
| `!MacEverything/a` | 37.41 | 38.25 |

差异在噪声范围内，既不能算加速，也不能算回退。完整数据见 before.txt 和 after.txt。加载到 Phase 2 完成的单次耗时为 9.92 → 9.30 s，不作为稳定结论。

## 验证

- 快速回归：12,243 项通过，0 失败（tests.log）。新增 12 项，覆盖：
  - 32 位边界、转义值、`UINT64_MAX`
  - 负数时间与超出 32 位范围的时间
  - 300 个不同设备号导致字典溢出
  - `set` 在窄存储和旁表之间迁移；`resize` 缩小时同步清理旁表；副本相互独立
  - 2 万条路径在 32 位键下全部可解析
  - v6 保存、重载、`size:>4gb` 查询
- AddressSanitizer + UndefinedBehaviorSanitizer：Part 3d、8、21、47、48、58、62、64、69、71、72、77，共 716 项通过，0 失败，没有报告错误（asan.log）。
- 用整份固定快照保存，再重载 6,460,706 条记录：size、modTime、devId、inode、type、名称均 0 差异（savecheck.cpp）。新文件 740,159,038 字节，与上一轮新版写出的大小一致。
- `make lint-bridge` 通过；Xcode Release 构建成功，产物 Info.plist 中含有 `LSEnvironment`。

## 桌面应用实测

先通过 AppleScript 正常退出旧进程（退出时保存索引），再用 `make app` 重新构建 Release 版，执行 `lsregister -f` 后经 `open` 启动。`ps eww` 确认新进程的环境中含有 `MallocLargeCache=0`。实时索引与固定快照的记录数和生命周期都不同，两组数字不能直接相减。

| 指标 | 旧版本（运行约 9 小时） | 新版本 |
|---|---:|---:|
| 记录位置 / 有效记录 | 约 674 万 / 641 万 | 6,747,137 / 6,407,800 |
| 物理足迹（`footprint`） | 1300 MB | Phase 2 完成时 734 MB；3.5 分钟后 800 MB |
| 启动历史峰值（vmmap） | 1.6G | 935.9M |
| Malloc Large 脏页 | 683 MB | 11.6 MB（其余 403.6 MB 已换出或归还） |

新版本时间线：索引加载 4.27 s，FSEvents 回放 0.13 s；Phase 2 在启动后约 11 s 完成，期间每秒采样的峰值为 920 MB，完成后回落到 734 MB。启动后 41–50 s 之间上涨约 52 MB，同一时段日志没有记录，原因未定位；之后基本稳定在 790–800 MB。

malloc 在用 798.8 MB，比固定快照（703.7 MiB）多出的部分来自更多的实时记录、GUI 和 Objective-C 对象。日志中没有 ERROR 或 WARN；空闲 CPU 单次采样为 0.2%。

通过本地 HTTP 接口执行了 `MacEverything/readme`、`size:>4gb`、`ext:swift` 三条查询，均正常返回。`size:>4gb` 第一条结果为 8,485,014,416 字节，与旁表保存的精确值一致。原始数据见 live-samples.txt、live-vmmap-before.txt、live-vmmap-after.txt、live-footprint-before.txt、live-queries.txt（已省略路径）。

## 范围与限制

- 旧版本的数字是运行约 9 小时后的单次采样，新版本只观察了启动后约 3.5 分钟，未做长期驻留测试。
- 没有测保存、压缩阶段的峰值。
- 从 Xcode 调试运行时不会读取 `LSEnvironment`，进程会沿用默认分配器；需要的话，可在 scheme 里加同名环境变量。
- 第二阶段（inode/devId 不再常驻内存、取消 pathIndex_、目录记录改按段存储、倒排表扁平化）未做。
