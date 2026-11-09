import AppKit

enum MCPClient: String, CaseIterable {
    case claudeCode
    case codex
    case cursor
    case claudeDesktop

    var displayName: String {
        switch self {
        case .claudeCode: return "Claude Code"
        case .codex: return "Codex"
        case .cursor: return "Cursor"
        case .claudeDesktop: return "Claude Desktop"
        }
    }

    var configFileURL: URL {
        configFileURL(home: FileManager.default.homeDirectoryForCurrentUser)
    }

    func configFileURL(home: URL) -> URL {
        switch self {
        case .claudeCode:
            return home.appendingPathComponent(".claude.json")
        case .codex:
            return home.appendingPathComponent(".codex/config.toml")
        case .cursor:
            return home.appendingPathComponent(".cursor/mcp.json")
        case .claudeDesktop:
            return home
                .appendingPathComponent("Library/Application Support/Claude/claude_desktop_config.json")
        }
    }
}

enum MCPConfigError: LocalizedError {
    case unreadable(URL, Error)
    case invalidContent(URL, String)
    case unwritable(URL, Error)

    var errorDescription: String? {
        switch self {
        case .unreadable(let url, let error):
            return "Could not read \(url.path): \(error.localizedDescription)"
        case .invalidContent(let url, let reason):
            return "\(url.path) was left unchanged: \(reason) Fix the file and try again."
        case .unwritable(let url, let error):
            return "Could not write \(url.path): \(error.localizedDescription)"
        }
    }
}

struct MCPConfigManager {
    private static let serverName = "maceverything"
    private static let tomlServerPath = ["mcp_servers", serverName]

    static var mcpBinaryPath: String {
        guard let execURL = Bundle.main.executableURL else { return "" }
        return execURL.deletingLastPathComponent()
            .appendingPathComponent("MacEverythingMCP").path
    }

    private static var userHome: URL { FileManager.default.homeDirectoryForCurrentUser }

    static func isEnabled(for client: MCPClient) -> Bool {
        isEnabled(for: client, home: userHome)
    }

    static func isEnabled(for client: MCPClient, home: URL) -> Bool {
        let url = client.configFileURL(home: home)
        if client == .codex {
            guard let bytes = try? readBytes(at: url),
                  let statements = TOMLScanner.statements(of: bytes) else { return false }
            return statements.contains { isServerEntry($0) }
        }
        guard let root = try? readJSON(at: url),
              let servers = root["mcpServers"] as? [String: Any] else { return false }
        return servers[serverName] != nil
    }

    /// Applies the change and tells the user when the config could not be updated.
    static func setEnabled(_ enabled: Bool, for client: MCPClient) {
        do {
            try apply(enabled, for: client, home: userHome)
        } catch {
            let alert = NSAlert()
            alert.alertStyle = .warning
            alert.messageText = "Could not \(enabled ? "enable" : "disable") MCP for \(client.displayName)"
            alert.informativeText = error.localizedDescription
            alert.runModal()
        }
    }

    static func apply(_ enabled: Bool, for client: MCPClient, home: URL) throws {
        let url = client.configFileURL(home: home)
        switch (client == .codex, enabled) {
        case (true, true): try enableTOML(at: url)
        case (true, false): try disableTOML(at: url)
        case (false, true): try enableJSON(at: url)
        case (false, false): try disableJSON(at: url)
        }
    }

    // MARK: - JSON clients

    private static func enableJSON(at url: URL) throws {
        var root = try readJSON(at: url) ?? [:]
        var servers = try serverTable(in: root, at: url)
        servers[serverName] = [
            "command": mcpBinaryPath,
            "args": [String]()
        ] as [String: Any]
        root["mcpServers"] = servers
        try writeJSON(root, to: url)
    }

    private static func disableJSON(at url: URL) throws {
        guard var root = try readJSON(at: url) else { return }
        var servers = try serverTable(in: root, at: url)
        guard servers.removeValue(forKey: serverName) != nil else { return }
        if servers.isEmpty {
            root.removeValue(forKey: "mcpServers")
        } else {
            root["mcpServers"] = servers
        }
        try writeJSON(root, to: url)
    }

    private static func serverTable(in root: [String: Any], at url: URL) throws -> [String: Any] {
        guard let value = root["mcpServers"] else { return [:] }
        guard let servers = value as? [String: Any] else {
            throw MCPConfigError.invalidContent(url, "\"mcpServers\" is not a JSON object.")
        }
        return servers
    }

    /// Returns nil only when the file does not exist.
    private static func readJSON(at url: URL) throws -> [String: Any]? {
        guard let bytes = try readBytes(at: url) else { return nil }
        let object = try? JSONSerialization.jsonObject(with: Data(bytes))
        guard let root = object as? [String: Any] else {
            throw MCPConfigError.invalidContent(url, "it does not contain a valid JSON object.")
        }
        return root
    }

    private static func writeJSON(_ dict: [String: Any], to url: URL) throws {
        let data: Data
        do {
            data = try JSONSerialization.data(
                withJSONObject: dict,
                options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
            )
        } catch {
            throw MCPConfigError.unwritable(url, error)
        }
        try write(Array(data), to: url)
    }

    // MARK: - TOML clients

    private static func enableTOML(at url: URL) throws {
        var bytes = try readBytes(at: url) ?? []
        let statements = try tomlStatements(of: bytes, at: url)
        // A `[mcp_servers.x]` header after an inline `mcp_servers = {...}` is invalid TOML.
        if statements.contains(where: { $0.kind == .key && $0.path == ["mcp_servers"] }) {
            throw MCPConfigError.invalidContent(url, "mcp_servers is defined as an inline table.")
        }
        removeServerEntry(from: &bytes, statements: statements)
        if let last = bytes.last {
            if last != UInt8(ascii: "\n") { bytes.append(UInt8(ascii: "\n")) }
            bytes.append(UInt8(ascii: "\n"))
        }
        let command = mcpBinaryPath
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        bytes += Array("[mcp_servers.\(serverName)]\ncommand = \"\(command)\"\nargs = []\n".utf8)
        try write(bytes, to: url)
    }

    private static func disableTOML(at url: URL) throws {
        guard var bytes = try readBytes(at: url) else { return }
        let statements = try tomlStatements(of: bytes, at: url)
        guard statements.contains(where: { isServerEntry($0) }) else { return }
        removeServerEntry(from: &bytes, statements: statements)
        try write(bytes, to: url)
    }

    private static func tomlStatements(of bytes: [UInt8], at url: URL) throws -> [TOMLStatement] {
        guard let statements = TOMLScanner.statements(of: bytes) else {
            throw MCPConfigError.invalidContent(url, "it is not valid TOML.")
        }
        return statements
    }

    private static func isServerEntry(_ statement: TOMLStatement) -> Bool {
        (statement.kind == .table || statement.kind == .key)
            && statement.path.starts(with: tomlServerPath)
    }

    /// Removes the entry's headers and keys plus the blank line separating it from
    /// what comes before. Every other byte of the document is kept.
    private static func removeServerEntry(from bytes: inout [UInt8], statements: [TOMLStatement]) {
        for index in statements.indices.reversed() where isServerEntry(statements[index]) {
            var range = statements[index].range
            if index > 0, statements[index - 1].kind == .blank {
                range = statements[index - 1].range.lowerBound..<range.upperBound
            }
            bytes.removeSubrange(range)
        }
    }

    // MARK: - Files

    /// Returns nil only when the file does not exist.
    private static func readBytes(at url: URL) throws -> [UInt8]? {
        do {
            return Array(try Data(contentsOf: url))
        } catch CocoaError.fileReadNoSuchFile {
            return nil
        } catch {
            throw MCPConfigError.unreadable(url, error)
        }
    }

    private static func write(_ bytes: [UInt8], to url: URL) throws {
        // Follow symlinks so an atomic replace doesn't turn a linked dotfile into a copy.
        let target = url.resolvingSymlinksInPath()
        do {
            try FileManager.default.createDirectory(
                at: target.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            try Data(bytes).write(to: target, options: .atomic)
        } catch {
            throw MCPConfigError.unwritable(url, error)
        }
    }
}

// MARK: - TOML scanning

struct TOMLStatement {
    enum Kind { case blank, comment, table, key }

    let kind: Kind
    /// Full dotted path: a key's path includes its enclosing table.
    let path: [String]
    let range: Range<Int>
}

/// Splits a TOML document into top-level statements so one entry can be removed
/// without reserializing the rest. Values are skipped rather than interpreted.
struct TOMLScanner {
    private let bytes: [UInt8]
    private var position = 0

    private static let newline = UInt8(ascii: "\n")
    private static let carriageReturn = UInt8(ascii: "\r")
    private static let hash = UInt8(ascii: "#")
    private static let openBracket = UInt8(ascii: "[")
    private static let closeBracket = UInt8(ascii: "]")
    private static let equals = UInt8(ascii: "=")
    private static let doubleQuote = UInt8(ascii: "\"")
    private static let singleQuote = UInt8(ascii: "'")
    private static let backslash = UInt8(ascii: "\\")

    private init(_ bytes: [UInt8]) { self.bytes = bytes }

    /// Returns nil when the document is malformed.
    static func statements(of bytes: [UInt8]) -> [TOMLStatement]? {
        var scanner = TOMLScanner(bytes)
        return scanner.scan()
    }

    private var current: UInt8? { position < bytes.count ? bytes[position] : nil }

    private mutating func scan() -> [TOMLStatement]? {
        var statements: [TOMLStatement] = []
        var table: [String] = []
        while position < bytes.count {
            let start = position
            skipSpaces()
            let kind: TOMLStatement.Kind
            var path: [String] = []
            switch current {
            case nil, Self.newline?, Self.carriageReturn?:
                kind = .blank
            case Self.hash?:
                kind = .comment
            case Self.openBracket?:
                kind = .table
                guard let header = scanHeader() else { return nil }
                table = header
                path = header
            default:
                kind = .key
                guard let key = scanKeyPath(until: Self.equals) else { return nil }
                position += 1
                guard scanValue() else { return nil }
                path = table + key
            }
            guard finishLine() else { return nil }
            statements.append(TOMLStatement(kind: kind, path: path, range: start..<position))
        }
        return statements
    }

    private mutating func skipSpaces() {
        while current == UInt8(ascii: " ") || current == UInt8(ascii: "\t") { position += 1 }
    }

    /// Consumes an optional comment and the line ending; anything else is malformed.
    private mutating func finishLine() -> Bool {
        skipSpaces()
        if current == Self.hash { skipToLineEnd() }
        if current == Self.carriageReturn { position += 1 }
        guard let byte = current else { return true }
        guard byte == Self.newline else { return false }
        position += 1
        return true
    }

    private mutating func skipToLineEnd() {
        while let byte = current, byte != Self.newline { position += 1 }
    }

    private mutating func scanHeader() -> [String]? {
        position += 1
        let isArray = current == Self.openBracket
        if isArray { position += 1 }
        guard let path = scanKeyPath(until: Self.closeBracket) else { return nil }
        position += 1
        if isArray {
            guard current == Self.closeBracket else { return nil }
            position += 1
        }
        return path
    }

    /// Stops on the terminator without consuming it.
    private mutating func scanKeyPath(until terminator: UInt8) -> [String]? {
        var path: [String] = []
        var part: [UInt8] = []
        while let byte = current {
            switch byte {
            case terminator:
                path.append(String(decoding: part, as: UTF8.self))
                return path
            case Self.newline:
                return nil
            case UInt8(ascii: "."):
                path.append(String(decoding: part, as: UTF8.self))
                part = []
                position += 1
            case UInt8(ascii: " "), UInt8(ascii: "\t"):
                position += 1
            case Self.doubleQuote, Self.singleQuote:
                let start = position + 1
                guard scanString(quote: byte), position - 1 >= start else { return nil }
                part += bytes[start..<(position - 1)]
            default:
                part.append(byte)
                position += 1
            }
        }
        return nil
    }

    /// Skips a value, which may span lines inside arrays and multi-line strings.
    private mutating func scanValue() -> Bool {
        var depth = 0
        while let byte = current {
            switch byte {
            case Self.newline:
                if depth == 0 { return true }
                position += 1
            case Self.hash:
                if depth == 0 { return true }
                skipToLineEnd()
            case Self.doubleQuote, Self.singleQuote:
                guard scanString(quote: byte) else { return false }
            case Self.openBracket, UInt8(ascii: "{"):
                depth += 1
                position += 1
            case Self.closeBracket, UInt8(ascii: "}"):
                depth -= 1
                guard depth >= 0 else { return false }
                position += 1
            default:
                position += 1
            }
        }
        return depth == 0
    }

    private func isTripleQuote(_ quote: UInt8) -> Bool {
        position + 2 < bytes.count && bytes[position...(position + 2)].allSatisfy { $0 == quote }
    }

    private mutating func scanString(quote: UInt8) -> Bool {
        let hasEscapes = quote == Self.doubleQuote
        let isMultiline = isTripleQuote(quote)
        position += isMultiline ? 3 : 1
        while let byte = current {
            if hasEscapes && byte == Self.backslash {
                position += 2
            } else if isMultiline && isTripleQuote(quote) {
                position += 3
                return true
            } else if !isMultiline && byte == quote {
                position += 1
                return true
            } else if !isMultiline && byte == Self.newline {
                return false
            } else {
                position += 1
            }
        }
        return false
    }
}
