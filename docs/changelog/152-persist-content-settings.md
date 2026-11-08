# 152：内容搜索设置在重启后保留

日期：2026-11-09

修复 Content Settings 中的扩展名列表和最大文件大小只保存在内存中的问题。此前应用重启后扩展名列表变回空，内容索引不再运行，`infile:` 搜索也随之失效，需要重新设置。

- `ServiceEngine::setContentConfig` 应用设置并写入缓存目录下的 `content_config.txt`（第一行为大小上限，其后每行一个扩展名），先写临时文件再改名，避免写到一半时留下损坏的文件。
- `ServiceEngine` 创建时读取该文件恢复设置；文件不存在或格式错误时沿用默认值（无扩展名、1 MB）。
- 应用的 Content Settings、应用内 HTTP 配置回调以及守护进程的 `POST /api/content/config` 都改为经过 `setContentConfig`，三处设置都会保存。

测试：Part 40 新增重启后恢复扩展名和大小上限、清空列表同样保存、格式错误时回退默认值的检查；`./test_all --fast` 全部通过。另用守护进程验证：通过 HTTP 设置后重启，`GET /api/content/config` 返回相同设置。

版本号和构建号未修改（仍为 1.4.0，Build 4）。
