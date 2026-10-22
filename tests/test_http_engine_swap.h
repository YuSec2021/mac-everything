#pragma once
// Part 39: HttpServer engine swap test
// Verifies that HttpServer uses getter functions to always fetch the latest
// engine/contentIndex, so that swapping the underlying engine is transparent.

// All Core headers included by test_all.cpp
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>

static std::string httpRaw(uint16_t port, const std::string& req, bool fragmented = false) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return "";
    }

    struct timeval timeout{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int noSigPipe = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
    if (fragmented) {
        ::send(fd, req.data(), 10, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ::send(fd, req.data() + 10, req.size() - 10, 0);
    } else {
        ::send(fd, req.data(), req.size(), 0);
    }
    ::shutdown(fd, SHUT_WR);

    std::string response;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        response += buf;
    }
    ::close(fd);
    return response;
}

static std::string httpGet(uint16_t port, const std::string& path) {
    return httpRaw(port, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
}

static bool runPart39() {
    std::cout << "\n=== Part 39: HttpServer engine swap ===\n";
    // Create two engines with different data
    auto engine1 = std::make_shared<SearchEngine>();
    { FileRecord rec; rec.name = "file1.txt"; rec.path = "/tmp"; rec.type = 1; rec.size = 100; rec.modTime = time(nullptr); engine1->addRecord(std::move(rec)); }

    auto engine2 = std::make_shared<SearchEngine>();
    { FileRecord rec; rec.name = "file2.txt"; rec.path = "/tmp"; rec.type = 1; rec.size = 200; rec.modTime = time(nullptr); engine2->addRecord(std::move(rec)); }
    { FileRecord rec; rec.name = "other.txt"; rec.path = "/tmp"; rec.type = 1; rec.size = 300; rec.modTime = time(nullptr); engine2->addRecord(std::move(rec)); }

    // Shared pointer that can be swapped
    auto currentEngine = std::make_shared<std::shared_ptr<SearchEngine>>(engine1);
    auto contentIndex = std::make_shared<ContentIndex>();

    HttpServer server;
    uint16_t port = 19870;

    // Start with getter functions
    server.start(port,
        [currentEngine]() -> std::shared_ptr<SearchEngine> { return *currentEngine; },
        [contentIndex]() -> std::shared_ptr<ContentIndex> { return contentIndex; });

    // Wait for server to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Test 1: Query should use engine1
    {
        auto resp = httpGet(port, "/api/status");
        check(resp.find("\"recordCount\":1") != std::string::npos,
              "Status with engine1 shows 1 record");
    }

    // Test 2: Swap engine, same server should reflect new engine
    *currentEngine = engine2;
    {
        auto resp = httpGet(port, "/api/status");
        check(resp.find("\"recordCount\":2") != std::string::npos,
              "Status after engine swap shows 2 records");
    }

    // Test 3: Search should use new engine
    {
        auto resp = httpGet(port, "/api/search?q=file2");
        check(resp.find("file2.txt") != std::string::npos,
              "Search on swapped engine finds file2.txt");
    }

    // Test 4: Health endpoint always works (no engine needed)
    {
        auto resp = httpGet(port, "/api/health");
        check(resp.find("\"status\":\"ok\"") != std::string::npos,
              "Health endpoint returns ok status");
    }

    auto request = [&](const std::string& headers, const std::string& body = "") {
        return httpRaw(port, "POST /api/content/config HTTP/1.1\r\nHost: localhost\r\n" + headers + "\r\n" + body);
    };
    for (const auto* length : {"-1", "garbage", "184467440737095516160", "3junk", ""}) {
        check(request(std::string("Content-Length: ") + length + "\r\n").find("400 Bad Request") != std::string::npos,
              "Malformed Content-Length rejected without crashing");
    }
    check(request("Content-Length: 65537\r\n").find("413 Payload Too Large") != std::string::npos,
          "Oversized body rejected");
    check(request("Content-Length: 5\r\n", "ab").find("400 Bad Request") != std::string::npos,
          "Truncated body rejected");
    check(request("Content-Length: 0\r\ncontent-length: 0\r\n").find("400 Bad Request") != std::string::npos,
          "Duplicate Content-Length rejected");
    check(request("Transfer-Encoding: chunked\r\n").find("400 Bad Request") != std::string::npos,
          "Ambiguous transfer framing rejected");
    check(request("Origin: https://untrusted.example\r\n").find("403 Forbidden") != std::string::npos,
          "Cross-origin browser request rejected");
    check(request("Sec-Fetch-Site: cross-site\r\n").find("403 Forbidden") != std::string::npos,
          "Browser fetch rejected");
    check(httpRaw(port, "GET /api/status HTTP/1.1\r\nHost: untrusted.example\r\n\r\n").find("403 Forbidden") != std::string::npos,
          "DNS rebinding Host rejected");
    check(httpRaw(port, "GET /api/health HTTP/1.1\r\nhOsT: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n", true).find("200 OK") != std::string::npos,
          "Fragmented headers and mixed-case header names accepted");
    check(request("X-Large: " + std::string(8200, 'x') + "\r\n").find("413 Payload Too Large") != std::string::npos,
          "Oversized headers rejected");
    HttpServer::AdminCallbacks callbacks;
    callbacks.onGetContentExtensions = [] { return std::vector<std::string>{}; };
    callbacks.onGetContentMaxFileSize = [] { return uint64_t{1024}; };
    callbacks.onSetContentConfig = [](const auto&, uint64_t) {};
    // Install callbacks only while the server is stopped.
    server.stop();
    server.setAdminCallbacks(callbacks);
    server.start(port, [engine1] { return engine1; }, [contentIndex] { return contentIndex; });
    std::string badConfig = "{\"maxFileSize\":184467440737095516160}";
    check(request("Content-Length: " + std::to_string(badConfig.size()) + "\r\n", badConfig).find("400 Bad Request") != std::string::npos,
          "Overflowing config number rejected");
    check(httpGet(port, "/api/health").find("200 OK") != std::string::npos,
          "Server remains healthy after malformed requests");
    auto stopStart = std::chrono::steady_clock::now();
    server.stop();
    check(std::chrono::steady_clock::now() - stopStart < std::chrono::milliseconds(250),
          "Idle server wakes immediately for shutdown");
    return true;
}
