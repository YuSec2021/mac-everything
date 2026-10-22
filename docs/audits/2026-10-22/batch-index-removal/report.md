# 批量删除时按倒排表一次性维护索引

日期：2026-10-22。基线为 84c26b7（前缀删除改为按目录查找）。只改了删除大量记录时 trigram 索引和扩展名索引的维护方式，内存布局和磁盘格式都没有变。

## 问题

`removeByPathPrefix` 和 `batchRescanPrefix` 的第一阶段已经能在约 50 ms 内找到要删的记录，但之后要逐条调用 `removeTrigramsForRecord` 和 `removeExtensionForRecord`。每删一个记录号，`PackedPostingList::erase` 都要定位、移动整张表后面的字节，还要遍历全部检查点。一张表上删 k 个记录号，就要把这张表搬动 k 次。删除 `/System/Library/Frameworks`（41,562 条）共需约 1.4 秒，平均每条约 33 µs。

## 做法

- `PackedPostingList::eraseSorted(first, last)`：传入有序的记录号，效果与逐个执行 `erase(lower_bound(id))` 相同，每个记录号只删一次出现。
  - 从第一个待删记录号之前的检查点开始，边解码边就地重写差分：删掉一项就把前后两段差分合并。合并后的 varint 不会比两段之和更长，所以写入位置始终落在读取位置之后，原地重写是安全的。
  - 最后一个待删记录号处理完后，把下一项按新的前驱重新编码，后面的字节整体前移一次；后面的检查点只调整 rank 和偏移。
  - 重写区间内的检查点按步长重建，最后调用 `boundSeeks()`，保证相邻检查点间隔不超过上限。表删空时清空检查点，并把 `last_` 置 0，与逐条删除的结果一致。
- `SearchEngine::removeIndexEntriesForRecords(sortedIdx)`：先为每条记录取 (trigram, 记录号) 对，排序后按 trigram 分组，每张倒排表调用一次 `eraseSorted`；扩展名按扩展名分组，用一次 `remove_if` 完成删除。表删空时删除对应的键，与逐条删除一致。
- `removeByPathPrefix` 和 `batchRescanPrefix` 在命中记录不少于 `kBatchIndexRemoval`（64）条时，先批量清理这两个索引，再执行原来的逐条循环（写 WAL、`removePathRecord`、`tombstoneAt`、`liveCount_`、最近文件缓存），只是循环里不再处理这两个索引。少于 64 条时完全走原路径。这两个索引与循环里的其他结构互不依赖，并且都在 `tombstoneAt` 清空小写名之前完成，所以 WAL 的顺序和内容、`liveCount_`、最近文件缓存都不受影响。
- 新增只供测试使用的 `postingListsForTesting()`，按键排序返回全部 trigram 和扩展名倒排表。

## 固定快照耗时

输入为 /tmp/maceverything-memory-2/index.v6（只读）。两个版本都用 `MallocLargeCache=0` 运行 prefix-remove-dirs/mutbench.cpp（只调用公开 API，未改动），交替各跑两次。原始输出见 mutations.txt。

| 操作 | 84c26b7 | 修改后 |
|---|---:|---:|
| `prefix_remove_ms`：删除 `/System/Library/Frameworks` | 1396.9 / 1430.1 | **245.7 / 252.8** |
| 删除条数 / 删除后有效记录数 | 41,562 / 6,340,356 | 41,562 / 6,340,356 |
| `small_prefix_remove_ms`：28 条，低于阈值，仍走原路径 | 51.29 / 50.45 | 54.08 / 51.87 |
| `rescan_prefix_ms`：`/Library/Fonts`，2 条 | 56.9 / 54.8 | 60.4 / 56.3 |
| updateByPath（µs） | 107.6 / 106.2 | 109.0 / 109.9 |

大前缀的写锁时长从约 1.4 秒降到约 0.25 秒，其中约 50 ms 是 `pathRecordsUnder` 扫描目录的固定开销。小前缀和重扫没有走新路径，差异在两次运行之间的波动范围内。两个版本的删除条数和删除后的有效记录数完全一致。

## 验证

- 快速回归：`./test_all --fast` 共 12,227 项通过，0 失败（tests.log）。新增 7 项：
  - 300 轮随机测试：先用单条插入打乱检查点，再在稠密、稀疏、含重复记录号的表上批量删除（全删、随机删、删一段连续区间、少量删除，并混入不存在的记录号）。比较返回的删除数、逐项内容、随机位置读取，以及删除后继续 `push_back` 的结果，全部与逐条 `erase` 一致。
  - 两个引擎写入相同的 400 条记录：一个调用 `removeByPathPrefix`（实际删除 ≥ 64 条，走批量路径），另一个对每条路径逐个调用 `removeByPath`。比较结果：每张 trigram 和扩展名倒排表逐张相等，`liveRecordCount` 相等，`recentIndices` 相等，两份 WAL 的操作类型和路径按顺序逐条相等。
  - `batchRescanPrefix` 第一阶段的倒排表与逐条删除逐张相等，删除后的查询结果也一致。
- 变异测试（每次强制完整重新编译；Makefile 不跟踪头文件，单纯运行 `make` 不会重新编译）：
  - 删到表尾时不更新 `last_`：随机测试第 19 轮失败。
  - 遇到重复记录号时全部删除：测试进程 abort，退出码 134。
  - 去掉 `boundSeeks()`：只放宽了定位时的步数上限，内容不变，测试不会失败，符合预期。
  - 以上修改均已还原，还原后测试通过。
- AddressSanitizer（`make test-asan` 的编译参数）：运行 `--part 3 --part 3d --part 21`，181 项通过，没有报告错误（asan.log）。

## 范围与限制

- 没有重启桌面应用复测。
- 其他逐条删除的路径（单路径删除、更新、WAL 回放、压缩）没有改动。
- 剩下约 200 ms 没有进一步拆分。可能的来源有：常见 trigram 的长倒排表各要重写一次，以及逐条执行的 `erasePathRecord` 和 `tombstoneAt`。
