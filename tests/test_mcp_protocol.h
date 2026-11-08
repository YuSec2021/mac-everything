#pragma once
// Part 49: MCP Protocol Tests
// Tests the MCP server binary via subprocess stdio communication.

#include <arpa/inet.h>
#include <cstdio>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

/// Send JSON-RPC messages to the MCP binary via pipe and capture all responses.
/// Uses printf rather than echo: /bin/sh's echo would decode \\uXXXX itself.
static std::string mcpExec(const std::string& input, int port = 0) {
    // Use the built binary path (relative to build dir)
    std::string cmd = "printf '%s\\n' '" + input + "' | ./build/Release/MacEverythingMCP";
    if (port != 0) cmd += " --port " + std::to_string(port);
    cmd += " 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "";

    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        output += buf;
    }
    pclose(fp);
    return output;
}

/// Split newline-delimited responses into individual JSON strings.
static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream stream(s);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim trailing \r
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

/// Check if a JSON string contains a given substring.
static bool jsonContains(const std::string& json, const std::string& needle) {
    return json.find(needle) != std::string::npos;
}

/// Minimal stand-in for the app's HTTP API: records request lines, answers with a fixed response.
class FakeHttpServer {
public:
    FakeHttpServer(int status, std::string body) : status_(status), body_(std::move(body)) {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        listen(listenFd_, 8);
        socklen_t len = sizeof(addr);
        getsockname(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~FakeHttpServer() {
        shutdown(listenFd_, SHUT_RDWR);
        close(listenFd_);
        thread_.join();
    }

    int port() const { return port_; }

    std::string lastRequestLine() {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastRequestLine_;
    }

private:
    void serve() {
        for (;;) {
            int fd = accept(listenFd_, nullptr, nullptr);
            if (fd < 0) return;
            std::string request;
            char buf[1024];
            ssize_t n;
            while (request.find("\r\n\r\n") == std::string::npos &&
                   (n = recv(fd, buf, sizeof(buf), 0)) > 0) {
                request.append(buf, n);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                lastRequestLine_ = request.substr(0, request.find("\r\n"));
            }
            std::string response = "HTTP/1.1 " + std::to_string(status_) + " Status\r\n"
                "Content-Length: " + std::to_string(body_.size()) + "\r\n"
                "Connection: close\r\n\r\n" + body_;
            send(fd, response.data(), response.size(), 0);
            close(fd);
        }
    }

    int status_;
    std::string body_;
    int listenFd_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::string lastRequestLine_;
};

/// Build a tools/call request for the given tool with raw JSON arguments.
static std::string mcpToolCall(const std::string& tool, const std::string& arguments) {
    return R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":")" + tool +
           R"(","arguments":)" + arguments + "}}";
}

/// Run a search_files call whose query is the given raw JSON string body;
/// return the request line the HTTP API received.
static std::string mcpRequestForQuery(const std::string& rawQuery) {
    FakeHttpServer server(200, "[]");
    mcpExec(mcpToolCall("search_files", R"({"query":")" + rawQuery + R"("})"), server.port());
    return server.lastRequestLine();
}

/// Run a shell command and return its stdout, or an empty string if it could not start.
static std::string shellOutput(const std::string& cmd) {
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "";
    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) output += buf;
    pclose(fp);
    return output;
}

/// Compile MCPConfigManager with its harness and report each scenario through check().
static void runMcpConfigScenarios() {
    if (std::system("command -v swiftc >/dev/null 2>&1") != 0) {
        std::cout << "  [SKIP] swiftc not found\n";
        return;
    }
    char tmpl[] = "/tmp/maceverything-mcp-config-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "config harness: temp dir created");
    if (!dir) return;
    std::string root = dir;

    int compiled = std::system(("swiftc MacEverything/App/MCPConfigManager.swift "
                                "tests/mcp_config_harness/main.swift -o " + root + "/harness").c_str());
    check(compiled == 0, "config harness: compiles");
    if (compiled == 0) {
        auto lines = splitLines(shellOutput(root + "/harness " + root + " 2>&1"));
        size_t scenarios = 0;
        bool finished = false;
        for (const auto& line : lines) {
            if (line == "done") { finished = true; continue; }
            scenarios++;
            check(line.rfind("ok ", 0) == 0, line.c_str());
        }
        check(finished, "config harness: ran to completion");
        check(scenarios >= 40, "config harness: reported all scenarios");
    }
    std::system(("rm -rf " + root).c_str());
}

static void runMcpProtocolTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 49: MCP Protocol Tests\n";
    std::cout << "========================================\n\n";

    // Skip if MCP binary not built
    if (std::system("test -x ./build/Release/MacEverythingMCP") != 0) {
        std::cout << "  [SKIP] MCP binary not found at ./build/Release/MacEverythingMCP\n";
        return;
    }

    // -- Test 1: Initialize --
    std::cout << "  --- Test 1: Initialize ---\n";
    {
        std::string input = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}})";
        auto output = mcpExec(input);
        auto lines = splitLines(output);
        check(lines.size() == 1, "initialize: exactly 1 response line");
        check(jsonContains(lines[0], "\"protocolVersion\":\"2025-03-26\""), "initialize: has protocol version");
        check(jsonContains(lines[0], "\"name\":\"MacEverything\""), "initialize: has server name");
        check(jsonContains(lines[0], "\"tools\":{}"), "initialize: has tools capability");
        check(jsonContains(lines[0], "\"id\":1"), "initialize: id matches");
    }

    // -- Test 2: tools/list --
    std::cout << "\n  --- Test 2: tools/list ---\n";
    {
        std::string input =
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}})"
            "\n"
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
            "\n"
            R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})";
        auto output = mcpExec(input);
        auto lines = splitLines(output);
        check(lines.size() == 2, "tools/list: 2 response lines (init + list)");
        auto& toolsResp = lines[1];
        check(jsonContains(toolsResp, "\"id\":2"), "tools/list: id matches");
        check(jsonContains(toolsResp, "search_files"), "tools/list: has search_files");
        check(jsonContains(toolsResp, "search_content"), "tools/list: has search_content");
        check(jsonContains(toolsResp, "recent_files"), "tools/list: has recent_files");
        check(jsonContains(toolsResp, "index_status"), "tools/list: has index_status");
        check(jsonContains(toolsResp, "inputSchema"), "tools/list: tools have inputSchema");
    }

    // -- Test 3: ping --
    std::cout << "\n  --- Test 3: ping ---\n";
    {
        std::string input =
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}})"
            "\n"
            R"({"jsonrpc":"2.0","id":99,"method":"ping"})";
        auto output = mcpExec(input);
        auto lines = splitLines(output);
        check(lines.size() == 2, "ping: 2 response lines");
        check(jsonContains(lines[1], "\"id\":99"), "ping: id matches");
        check(jsonContains(lines[1], "\"result\":{}"), "ping: result is empty object");
    }

    // -- Test 4: Unknown method returns error --
    std::cout << "\n  --- Test 4: Unknown method ---\n";
    {
        std::string input =
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}})"
            "\n"
            R"({"jsonrpc":"2.0","id":5,"method":"nonexistent/method"})";
        auto output = mcpExec(input);
        auto lines = splitLines(output);
        check(lines.size() == 2, "unknown method: 2 response lines");
        check(jsonContains(lines[1], "\"error\""), "unknown method: has error field");
        check(jsonContains(lines[1], "-32601"), "unknown method: error code is -32601");
    }

    // -- Test 5: notifications/initialized is silent --
    std::cout << "\n  --- Test 5: Notification is silent ---\n";
    {
        std::string input =
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}})"
            "\n"
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
        auto output = mcpExec(input);
        auto lines = splitLines(output);
        // Notification has no "id" so should produce no response
        check(lines.size() == 1, "notification: only 1 response (initialize), notification is silent");
    }

    // -- Test 6: tools/call with unknown tool --
    std::cout << "\n  --- Test 6: Unknown tool call ---\n";
    {
        std::string input =
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}})"
            "\n"
            R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"nonexistent_tool","arguments":{}}})";
        auto output = mcpExec(input);
        auto lines = splitLines(output);
        check(lines.size() == 2, "unknown tool: 2 response lines");
        check(jsonContains(lines[1], "\"isError\":true"), "unknown tool: isError is true");
        check(jsonContains(lines[1], "Unknown tool"), "unknown tool: error message mentions unknown tool");
    }

    // -- Test 7: tools/call search_files without query --
    std::cout << "\n  --- Test 7: search_files missing query ---\n";
    {
        std::string input =
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}})"
            "\n"
            R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"search_files","arguments":{}}})";
        auto output = mcpExec(input);
        auto lines = splitLines(output);
        check(lines.size() == 2, "missing query: 2 response lines");
        check(jsonContains(lines[1], "missing required parameter"), "missing query: error message mentions missing parameter");
    }

    // -- Test 8: Empty and whitespace lines are skipped --
    std::cout << "\n  --- Test 8: Empty lines skipped ---\n";
    {
        std::string input =
            "\n\n"
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}})"
            "\n\n\n"
            R"({"jsonrpc":"2.0","id":2,"method":"ping"})"
            "\n\n";
        auto output = mcpExec(input);
        auto lines = splitLines(output);
        check(lines.size() == 2, "empty lines: still 2 responses");
        check(jsonContains(lines[0], "\"id\":1"), "empty lines: first response has id 1");
        check(jsonContains(lines[1], "\"id\":2"), "empty lines: second response has id 2");
    }

    // -- Test 9: Invalid JSON returns parse error --
    std::cout << "\n  --- Test 9: Invalid JSON ---\n";
    {
        std::string input = "this is not json at all";
        auto output = mcpExec(input);
        auto lines = splitLines(output);
        check(lines.size() == 1, "invalid JSON: 1 response");
        check(jsonContains(lines[0], "-32700"), "invalid JSON: parse error code -32700");
    }

    // -- Test 10: JSON escapes in arguments are decoded before the HTTP call --
    std::cout << "\n  --- Test 10: JSON unescaping ---\n";
    {
        auto queryIs = [](const std::string& raw, const std::string& encoded) {
            return mcpRequestForQuery(raw) == "GET /api/search?q=" + encoded + " HTTP/1.1";
        };
        check(queryIs("plain", "plain"), "unescape: plain ASCII unchanged");
        check(queryIs("中文", "%E4%B8%AD%E6%96%87"), "unescape: raw UTF-8 unchanged");
        check(queryIs(R"(\u4e2d\u6587)", "%E4%B8%AD%E6%96%87"), "unescape: \\uXXXX matches raw UTF-8");
        check(queryIs(R"(\u00E9)", "%C3%A9"), "unescape: uppercase hex, 2-byte sequence");
        check(queryIs(R"(\u0041)", "A"), "unescape: 1-byte sequence");
        check(queryIs(R"(\uD83D\uDE00)", "%F0%9F%98%80"), "unescape: surrogate pair");
        check(queryIs(R"(\ud83d\ude00x)", "%F0%9F%98%80x"), "unescape: lowercase surrogate pair then text");
        check(queryIs(R"(a\"b)", "a%22b"), "unescape: escaped quote");
        check(queryIs(R"(a\\b)", "a%5Cb"), "unescape: escaped backslash");
        check(queryIs(R"(a\\\"b)", "a%5C%22b"), "unescape: backslash before quote");
        check(queryIs(R"(a\\u0041)", "a%5Cu0041"), "unescape: escaped backslash is not a \\u escape");
        check(queryIs(R"(a\/b)", "a%2Fb"), "unescape: escaped solidus");
        check(queryIs(R"(a\tb\nc\rd\be\ff)", "a%09b%0Ac%0Dd%08e%0Cf"), "unescape: control escapes");
        check(queryIs(R"(\uD83Dx)", "%EF%BF%BDx"), "unescape: lone high surrogate becomes U+FFFD");
        check(queryIs(R"(\uDE00x)", "%EF%BF%BDx"), "unescape: lone low surrogate becomes U+FFFD");
        check(queryIs(R"(\uD83D\u0041)", "%EF%BF%BDA"), "unescape: high surrogate before non-surrogate");
        check(queryIs(R"(\u12)", "%5Cu12"), "unescape: truncated \\u kept as written");
    }

    // -- Test 11: HTTP failures are reported as tool errors --
    std::cout << "\n  --- Test 11: HTTP error mapping ---\n";
    {
        auto callAgainst = [](int status, const std::string& body, const std::string& tool, const std::string& args) {
            FakeHttpServer server(status, body);
            return mcpExec(mcpToolCall(tool, args), server.port());
        };
        const std::string searchArgs = R"({"query":"x"})";

        auto ok = callAgainst(200, R"([{"path":"/a"}])", "search_files", searchArgs);
        check(jsonContains(ok, "\"isError\":false"), "http: 200 is not an error");

        auto serverError = callAgainst(500, "boom", "search_files", searchArgs);
        check(jsonContains(serverError, "\"isError\":true"), "http: 500 is an error");
        check(jsonContains(serverError, "boom"), "http: 500 keeps response body");

        auto notFound = callAgainst(404, "", "search_content", searchArgs);
        check(jsonContains(notFound, "\"isError\":true"), "http: 404 is an error");
        check(jsonContains(notFound, "HTTP 404"), "http: empty error body reports the status");

        check(jsonContains(callAgainst(503, "busy", "recent_files", "{}"), "\"isError\":true"), "http: recent_files maps errors");
        check(jsonContains(callAgainst(503, "busy", "index_status", "{}"), "\"isError\":true"), "http: index_status maps errors");

        auto lookalike = callAgainst(200, "/tmp/Cannot connect notes.txt", "search_files", searchArgs);
        check(jsonContains(lookalike, "\"isError\":false"), "http: 200 body mentioning a failure is not an error");

        int unusedPort;
        { FakeHttpServer server(200, ""); unusedPort = server.port(); }
        auto refused = mcpExec(mcpToolCall("search_files", searchArgs), unusedPort);
        check(jsonContains(refused, "\"isError\":true"), "http: connection refused is an error");
        check(jsonContains(refused, "Cannot connect"), "http: connection refused explains itself");
    }

    // -- Test 12: Client config files --
    std::cout << "\n  --- Test 12: Client config files ---\n";
    runMcpConfigScenarios();

    std::cout << "\n  Part 49 complete.\n\n";
}
