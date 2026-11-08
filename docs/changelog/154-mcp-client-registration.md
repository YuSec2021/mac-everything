# 154：修复 MCP 客户端注册并新增 Codex

日期：2026-11-09

修复 MCP Integration 菜单对 Claude Code 写错配置文件，以及 MCP Server 的参数解码和错误标记问题。此前 Claude Code 的配置写进 `~/.claude/settings.json`，菜单显示已启用，但 Claude Code 并不读取该文件的 `mcpServers`，实际连接不上。

**客户端配置（`MCPConfigManager.swift`）**

- Claude Code 改为写入 `~/.claude.json` 的顶层 `mcpServers`，其余内容全部保留，`isEnabled` 读同一位置。旧位置的条目不再算作启用，也不会被自动清理；已启用过的用户需要在菜单中重新启用一次，`~/.claude/settings.json` 里的旧条目可手动删除。
- 配置文件存在但无法读取、不是合法 JSON、根节点不是对象，或 `mcpServers` 不是对象时，不再写入，并弹窗说明原因。此前解析失败会用空配置覆盖原文件。
- 新增 Codex（`~/.codex/config.toml`）。只追加或删除 `[mcp_servers.maceverything]` 对应的语句，其余字节不变；启用再停用后文件与原来逐字节相同。扫描器识别多行字符串和跨行数组，写法无法识别或 `mcp_servers` 是内联表时不写入并提示。
- 写入时跟随符号链接，JSON 中的 `/` 不再转义为 `\/`。

**MCP Server（`mcp_main.cpp`）**

- `jsonGetString` 解码 JSON 转义，包括 `\"`、`\\`、`\uXXXX` 和代理对；未配对的代理项替换为 U+FFFD。此前查询写成 `\uXXXX` 时搜不到结果。
- HTTP 非 2xx、无法连接、空响应都标记 `isError=true`，错误响应体为空时返回状态码说明。不再靠匹配结果文本判断错误，搜索结果里出现 “Cannot connect” 字样时也不会被误判。

**测试**

- Part 49 新增假 HTTP 服务，校验转义解码后的实际请求路径和错误状态映射；新增 `tests/mcp_config_harness`，针对临时目录校验各客户端的配置读写与拒绝写入。
- `make -B test_all` 后 `make test-fast` 全部通过，Xcode Release 构建成功。
- 用临时 HOME 验证：`claude mcp list` 显示 `✔ Connected`，`codex mcp list` 显示 `enabled`。真实配置文件未被修改。对运行中的应用实测，`中文` 与直接发送 UTF-8 的查询返回相同数量的结果。

版本号和构建号未修改（仍为 1.4.0，Build 4）。
