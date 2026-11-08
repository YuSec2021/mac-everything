// Part 49 helper: exercises MCPConfigManager against a throwaway home directory.
// Every scenario passes `home:` explicitly because homeDirectoryForCurrentUser ignores $HOME,
// so nothing here can reach the real user configs. Prints "ok <name>" / "not ok <name>".

import Foundation

let fm = FileManager.default
let root = URL(fileURLWithPath: CommandLine.arguments[1])
var scenarioCount = 0

func freshHome() -> URL {
    scenarioCount += 1
    let home = root.appendingPathComponent("home\(scenarioCount)")
    try! fm.createDirectory(at: home, withIntermediateDirectories: true)
    return home
}

func put(_ text: String, at url: URL) {
    try! fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try! Data(text.utf8).write(to: url)
}

func text(at url: URL) -> String {
    String(decoding: (try? Data(contentsOf: url)) ?? Data(), as: UTF8.self)
}

func json(at url: URL) -> NSDictionary {
    (try? JSONSerialization.jsonObject(with: Data(contentsOf: url))) as? NSDictionary ?? [:]
}

func report(_ condition: Bool, _ name: String) {
    print("\(condition ? "ok" : "not ok") \(name)")
}

func throwsAndKeeps(_ enabled: Bool, _ client: MCPClient, _ home: URL) -> Bool {
    let url = client.configFileURL(home: home)
    let before = text(at: url)
    do {
        try MCPConfigManager.apply(enabled, for: client, home: home)
        return false
    } catch {
        return text(at: url) == before
    }
}

let jsonClients: [MCPClient] = [.claudeCode, .cursor, .claudeDesktop]

// Claude Code: top-level mcpServers of ~/.claude.json, everything else kept.
do {
    let home = freshHome()
    let url = MCPClient.claudeCode.configFileURL(home: home)
    report(url.path == home.appendingPathComponent(".claude.json").path, "claude code targets ~/.claude.json")
    put(#"{"numStartups":7,"oauthAccount":{"email":"a/b"},"projects":{"/x":{"mcpServers":{"p":{}}}},"mcpServers":{"other":{"command":"o"}},"list":[1,null,true]}"#, at: url)
    let original = json(at: url)
    try MCPConfigManager.apply(true, for: .claudeCode, home: home)
    let enabled = json(at: url).mutableCopy() as! NSMutableDictionary
    let servers = enabled["mcpServers"] as? NSDictionary
    report((servers?["maceverything"] as? NSDictionary)?["command"] as? String == MCPConfigManager.mcpBinaryPath, "claude code entry written")
    report(servers?["other"] != nil, "claude code keeps other servers")
    report(MCPConfigManager.isEnabled(for: .claudeCode, home: home), "claude code isEnabled after enable")
    report(!fm.fileExists(atPath: home.appendingPathComponent(".claude/settings.json").path), "claude code leaves settings.json alone")
    report(!text(at: url).contains("\\/"), "slashes are not escaped")
    enabled["mcpServers"] = original["mcpServers"]
    report(enabled == original, "claude code keeps unrelated keys on enable")
    try MCPConfigManager.apply(false, for: .claudeCode, home: home)
    report(json(at: url) == original, "claude code disable restores config")
    report(!MCPConfigManager.isEnabled(for: .claudeCode, home: home), "claude code isEnabled after disable")
}

// A stale entry in the old location no longer counts as enabled.
do {
    let home = freshHome()
    put(#"{"mcpServers":{"maceverything":{"command":"x"}}}"#, at: home.appendingPathComponent(".claude/settings.json"))
    report(!MCPConfigManager.isEnabled(for: .claudeCode, home: home), "settings.json entry is not enabled")
}

// Missing files are created for every client.
do {
    for client in MCPClient.allCases {
        let home = freshHome()
        try MCPConfigManager.apply(true, for: client, home: home)
        report(MCPConfigManager.isEnabled(for: client, home: home), "\(client.rawValue) created from missing file")
    }
}

// Unparseable or unexpectedly shaped JSON is never overwritten.
for client in jsonClients {
    for (name, body) in [
        ("malformed", #"{"keep": tru"#),
        ("empty", ""),
        ("array root", "[1,2]"),
        ("non-object mcpServers", #"{"mcpServers":[1],"keep":1}"#),
    ] {
        let home = freshHome()
        put(body, at: client.configFileURL(home: home))
        report(throwsAndKeeps(true, client, home), "\(client.rawValue) \(name) refused on enable")
        report(throwsAndKeeps(false, client, home), "\(client.rawValue) \(name) refused on disable")
    }
}

// An unreadable file is an error, not "no config yet".
do {
    let home = freshHome()
    let url = MCPClient.cursor.configFileURL(home: home)
    put(#"{"keep":1}"#, at: url)
    try fm.setAttributes([.posixPermissions: 0o000], ofItemAtPath: url.path)
    var refused = false
    do { try MCPConfigManager.apply(true, for: .cursor, home: home) } catch { refused = true }
    try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: url.path)
    report(refused && json(at: url) == ["keep": 1], "unreadable file refused")
}

// Atomic replacement keeps symlinks and permissions.
do {
    let home = freshHome()
    let url = MCPClient.claudeCode.configFileURL(home: home)
    let real = home.appendingPathComponent("dotfiles/claude.json")
    put("{}", at: real)
    try fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: real.path)
    try fm.createSymbolicLink(at: url, withDestinationURL: real)
    try MCPConfigManager.apply(true, for: .claudeCode, home: home)
    report((try? fm.destinationOfSymbolicLink(atPath: url.path)) != nil, "symlink survives")
    report(MCPConfigManager.isEnabled(for: .claudeCode, home: home) && text(at: real).contains("maceverything"), "symlink target updated")
    let mode = (try fm.attributesOfItem(atPath: real.path))[.posixPermissions] as? Int
    report(mode == 0o600, "file mode survives")
}

// Codex: appended as its own table; every other byte kept, including text that looks like headers.
do {
    let original = #"""
    # top comment
    model = "gpt-5"
    prompt = """
    [mcp_servers.maceverything]
    command = "not a real entry"
    """
    matrix = [
      ["a", "b"], # comment [mcp_servers.maceverything]
      ["c"],
    ]
    path = 'C:\tmp\[x]'

    [mcp_servers.other]
    command = "/bin/other" # trailing
    args = ["--flag=[x]"]

    """#.replacingOccurrences(of: "\n    ", with: "\n").replacingOccurrences(of: "    # top", with: "# top")
    let home = freshHome()
    let url = MCPClient.codex.configFileURL(home: home)
    put(original, at: url)
    report(!MCPConfigManager.isEnabled(for: .codex, home: home), "codex ignores header-like text in values")
    try MCPConfigManager.apply(true, for: .codex, home: home)
    let enabled = text(at: url)
    report(enabled.hasPrefix(original), "codex keeps all existing content")
    report(enabled.contains("[mcp_servers.maceverything]\ncommand = \"\(MCPConfigManager.mcpBinaryPath)\"\nargs = []\n"), "codex entry written")
    report(MCPConfigManager.isEnabled(for: .codex, home: home), "codex isEnabled after enable")
    try MCPConfigManager.apply(false, for: .codex, home: home)
    report(text(at: url) == original, "codex enable then disable is byte-identical")
    report(!MCPConfigManager.isEnabled(for: .codex, home: home), "codex isEnabled after disable")
}

// Re-enabling replaces an existing entry in the middle of the file.
do {
    let home = freshHome()
    let url = MCPClient.codex.configFileURL(home: home)
    put("[a]\nx = 1\n\n[mcp_servers.maceverything]\ncommand = \"/old\"\n[mcp_servers.maceverything.env]\nK = \"v\"\n\n# about b\n[b]\ny = 2\n", at: url)
    try MCPConfigManager.apply(true, for: .codex, home: home)
    let updated = text(at: url)
    report(updated.components(separatedBy: "[mcp_servers.maceverything").count == 2 && !updated.contains("/old"), "codex replaces stale entry")
    report(updated.hasPrefix("[a]\nx = 1\n\n# about b\n[b]\ny = 2\n"), "codex keeps neighbours when replacing")
}

// Other spellings of the same entry are recognised and removable.
for (name, body) in [
    ("quoted header", "[mcp_servers.\"maceverything\"]\ncommand = \"/x\"\n"),
    ("inline value", "[mcp_servers]\nmaceverything = { command = \"/x\" }\n"),
    ("dotted key", "mcp_servers.maceverything.command = \"/x\"\n"),
] {
    let home = freshHome()
    let url = MCPClient.codex.configFileURL(home: home)
    put("keep = 1\n" + body, at: url)
    report(MCPConfigManager.isEnabled(for: .codex, home: home), "codex detects \(name)")
    try MCPConfigManager.apply(false, for: .codex, home: home)
    report(!text(at: url).contains("maceverything") && text(at: url).hasPrefix("keep = 1\n"), "codex removes \(name)")
}

// CRLF files and files without a trailing newline stay valid.
do {
    let home = freshHome()
    let url = MCPClient.codex.configFileURL(home: home)
    put("a = 1\r\n# note\r\nb = 2", at: url)
    try MCPConfigManager.apply(true, for: .codex, home: home)
    report(text(at: url).hasPrefix("a = 1\r\n# note\r\nb = 2\n\n[mcp_servers.maceverything]\n"), "codex handles CRLF and missing newline")
}

// Documents the scanner cannot follow are left alone.
for (name, body) in [
    ("unterminated string", "a = \"oops\n"),
    ("unterminated multi-line string", "a = \"\"\"oops\n"),
    ("unterminated array", "a = [1,\n"),
    ("missing equals", "not toml at all\n"),
    ("inline mcp_servers", "mcp_servers = { other = { command = \"/x\" } }\n"),
] {
    let home = freshHome()
    put(body, at: MCPClient.codex.configFileURL(home: home))
    report(throwsAndKeeps(true, .codex, home), "codex \(name) refused")
}

print("done")
