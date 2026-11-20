// Standalone unit tests for ReleaseUpdateManager & version/asset resolution.
// Run with `make test-release-update`.
import AppKit
import Foundation

private final class DummyDownloadTask: ReleaseDownloadTasking {
    var cancelled = false
    func cancel() { cancelled = true }
}

private final class MockDownloader: ReleaseDownloadHandling {
    struct Request {
        let url: URL
        let expectedBytes: Int64?
        let onProgress: @Sendable (Double?, Int64, Int64) -> Void
        let onCompletion: @Sendable (Result<URL, Error>) -> Void
    }

    var requests: [Request] = []

    func download(
        from url: URL,
        expectedBytes: Int64?,
        onProgress: @escaping @Sendable (Double?, Int64, Int64) -> Void,
        onCompletion: @escaping @Sendable (Result<URL, Error>) -> Void
    ) -> ReleaseDownloadTasking {
        requests.append(Request(
            url: url,
            expectedBytes: expectedBytes,
            onProgress: onProgress,
            onCompletion: onCompletion
        ))
        return DummyDownloadTask()
    }
}

private enum MockError: LocalizedError {
    case offline
    case interrupted

    var errorDescription: String? {
        switch self {
        case .offline: return "网络不可用"
        case .interrupted: return "连接已中断"
        }
    }
}

@main
@MainActor
struct ReleaseUpdateTests {
    static var failures = 0

    static func check(_ condition: @autoclosure () -> Bool, _ message: String) {
        if condition() {
            print("PASS: \(message)")
        } else {
            failures += 1
            print("FAIL: \(message)")
        }
        fflush(stdout)
    }

    static func settle(_ seconds: TimeInterval = 0.05) {
        RunLoop.main.run(until: Date().addingTimeInterval(seconds))
    }

    nonisolated static func makeRelease(
        tag: String,
        draft: Bool = false,
        prerelease: Bool = false,
        assets: [GitHubReleaseAsset] = []
    ) -> GitHubRelease {
        GitHubRelease(
            tagName: tag,
            name: "MacEverything \(tag)",
            draft: draft,
            prerelease: prerelease,
            htmlURL: URL(string: "https://github.com/YuSec2021/mac-everything/releases/tag/\(tag)"),
            assets: assets
        )
    }

    nonisolated static func makeAsset(_ name: String, size: Int64 = 1024) -> GitHubReleaseAsset {
        GitHubReleaseAsset(
            name: name,
            browserDownloadURL: URL(string: "https://github.com/YuSec2021/mac-everything/releases/download/test/\(name)")!,
            size: size
        )
    }

    static func makeTempDir() -> URL {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("maceverything-update-test-\(UUID().uuidString)")
        try! FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    static func makeIsolatedDefaults() -> UserDefaults {
        let suite = "com.maceverything.test.update.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defaults.removePersistentDomain(forName: suite)
        return defaults
    }

    static func main() {
        testVersionParsingAndComparison()
        testReleaseSelectionAndArchitecture()
        testCacheAndSilentFailure()
        testDownloadFlowAndRecovery()
        testMissingAssetRetryRecovery()

        print(failures == 0 ? "ALL PASSED" : "FAILED: \(failures)")
        exit(failures == 0 ? 0 : 1)
    }

    static func testVersionParsingAndComparison() {
        let v140 = ReleaseVersion(parsing: "1.4.0")
        let v140Prefixed = ReleaseVersion(parsing: "v1.4.0")
        let v150 = ReleaseVersion(parsing: "V1.5.0")
        let v15Short = ReleaseVersion(parsing: "1.5")
        let v190 = ReleaseVersion(parsing: "v1.9.0")
        let v1100 = ReleaseVersion(parsing: "v1.10.0")

        check(v140 != nil && v140 == v140Prefixed, "v prefix equals bare version")
        check(v150 == v15Short, "1.5 equals 1.5.0")
        check(v140! < v150!, "1.4.0 < 1.5.0")
        check(v190! < v1100!, "1.9.0 < 1.10.0 numeric comparison")
        check(!(v1100! < v190!), "1.10.0 is not < 1.9.0")
        check(ReleaseVersion(parsing: "v1.5.0-beta.1") == nil, "prerelease suffix tag rejected")
        check(ReleaseVersion(parsing: "v1.5.0-rc1") == nil, "rc suffix tag rejected")
        check(ReleaseVersion(parsing: "") == nil, "empty version rejected")
    }

    static func testReleaseSelectionAndArchitecture() {
        let releases = [
            makeRelease(tag: "v1.11.0", draft: true, assets: [makeAsset("MacEverything-1.11.0-arm64.dmg")]),
            makeRelease(tag: "v1.10.1-beta.1", prerelease: true, assets: [makeAsset("MacEverything-1.10.1-arm64.dmg")]),
            makeRelease(tag: "v1.10.0", assets: [
                makeAsset("MacEverything-1.10.0-arm64.dmg", size: 2048),
                makeAsset("MacEverything-1.10.0-x86_64.dmg", size: 4096),
                makeAsset("SHA256SUMS.txt", size: 128),
            ]),
            makeRelease(tag: "v1.9.0", assets: [
                makeAsset("MacEverything-1.9.0-arm64.dmg"),
                makeAsset("MacEverything-1.9.0-x86_64.dmg"),
            ]),
        ]

        let armUpdate = ReleaseUpdateResolver.selectLatestUpdate(
            from: releases,
            currentVersion: "1.9.0",
            architecture: .arm64
        )
        check(armUpdate?.version == "1.10.0", "skips draft/prerelease and picks 1.10.0 over 1.9.0")
        check(armUpdate?.assetName == "MacEverything-1.10.0-arm64.dmg", "arm64 selects arm64 DMG")
        check(armUpdate?.expectedBytes == 2048, "captures expected asset byte size")

        let intelUpdate = ReleaseUpdateResolver.selectLatestUpdate(
            from: releases,
            currentVersion: "1.9.0",
            architecture: .x86_64
        )
        check(intelUpdate?.assetName == "MacEverything-1.10.0-x86_64.dmg", "x86_64 selects x86_64 DMG")
        check(intelUpdate?.expectedBytes == 4096, "captures Intel asset byte size")

        let sameVersion = ReleaseUpdateResolver.selectLatestUpdate(
            from: releases,
            currentVersion: "1.10.0",
            architecture: .arm64
        )
        check(sameVersion == nil, "current version does not show update")

        let olderRemote = ReleaseUpdateResolver.selectLatestUpdate(
            from: releases,
            currentVersion: "2.0.0",
            architecture: .arm64
        )
        check(olderRemote == nil, "older remote release does not show update")

        let missingIntel = ReleaseUpdateResolver.selectLatestUpdate(
            from: [makeRelease(tag: "v1.6.0", assets: [makeAsset("MacEverything-1.6.0-arm64.dmg")])],
            currentVersion: "1.5.0",
            architecture: .x86_64
        )
        check(missingIntel?.version == "1.6.0" && missingIntel?.hasMatchingAsset == false,
              "detects newer release with missing architecture asset")
    }

    static func testCacheAndSilentFailure() {
        let defaults = makeIsolatedDefaults()
        var now = Date(timeIntervalSince1970: 1_700_000_000)
        var fetchCount = 0
        var shouldFail = false
        var installedVersion = "1.4.0"

        let releaseList = [
            makeRelease(tag: "v1.5.0", assets: [makeAsset("MacEverything-1.5.0-arm64.dmg")])
        ]

        let manager = ReleaseUpdateManager(
            currentVersionProvider: { installedVersion },
            architectureProvider: { .arm64 },
            fetcher: { _, _ in
                fetchCount += 1
                if shouldFail { throw MockError.offline }
                return releaseList
            },
            downloader: MockDownloader(),
            defaults: defaults,
            nowProvider: { now },
            cacheTTL: 3600,
            failureCooldown: 300
        )

        manager.checkForUpdatesIfNeeded()
        settle()
        check(fetchCount == 1, "first check fetches from remote")
        if case .available(let release) = manager.phase {
            check(release.displayTag == "v1.5.0", "phase becomes available v1.5.0")
        } else {
            check(false, "expected available phase, got \(manager.phase)")
        }

        // Second check within TTL should use cache and not hit network.
        now = now.addingTimeInterval(600)
        manager.checkForUpdatesIfNeeded()
        settle()
        check(fetchCount == 1, "within cache TTL does not refetch")

        // New manager instance after app upgrade to 1.5.0 should clear stale cache and stay hidden.
        installedVersion = "1.5.0"
        let upgradedManager = ReleaseUpdateManager(
            currentVersionProvider: { installedVersion },
            architectureProvider: { .arm64 },
            fetcher: { _, _ in
                fetchCount += 1
                return releaseList
            },
            downloader: MockDownloader(),
            defaults: defaults,
            nowProvider: { now },
            cacheTTL: 3600
        )
        check(upgradedManager.phase == .hidden, "upgraded app ignores stale lower/equal cache")

        // Silent network error should keep UI hidden and respect cooldown.
        let silentDefaults = makeIsolatedDefaults()
        var silentFetchCount = 0
        shouldFail = true
        let silentManager = ReleaseUpdateManager(
            currentVersionProvider: { "1.4.0" },
            architectureProvider: { .arm64 },
            fetcher: { _, _ in
                silentFetchCount += 1
                throw MockError.offline
            },
            downloader: MockDownloader(),
            defaults: silentDefaults,
            nowProvider: { now },
            cacheTTL: 3600,
            failureCooldown: 300
        )
        silentManager.checkForUpdatesIfNeeded()
        settle()
        check(silentManager.phase == .hidden && silentFetchCount == 1, "silent check failure stays hidden")
        silentManager.checkForUpdatesIfNeeded()
        settle()
        check(silentFetchCount == 1, "silent failure cooldown prevents immediate refetch")
    }

    static func testDownloadFlowAndRecovery() {
        let downloadsDir = makeTempDir()
        defer { try? FileManager.default.removeItem(at: downloadsDir) }

        // Pre-create an existing file with the same name to verify unique naming.
        let existingFile = downloadsDir.appendingPathComponent("MacEverything-1.6.0-arm64.dmg")
        try! Data("existing".utf8).write(to: existingFile)

        let downloader = MockDownloader()
        var openedURL: URL?
        var revealedURL: URL?
        let releaseList = [
            makeRelease(tag: "v1.6.0", assets: [makeAsset("MacEverything-1.6.0-arm64.dmg", size: 100)])
        ]

        let manager = ReleaseUpdateManager(
            currentVersionProvider: { "1.5.0" },
            architectureProvider: { .arm64 },
            fetcher: { _, _ in releaseList },
            downloader: downloader,
            defaults: makeIsolatedDefaults(),
            downloadsDirectoryProvider: { downloadsDir },
            openFileHandler: { openedURL = $0 },
            revealFileHandler: { revealedURL = $0 }
        )

        manager.checkForUpdatesIfNeeded()
        settle()
        manager.startDownload()
        manager.startDownload() // duplicate click should be ignored
        check(downloader.requests.count == 1, "prevents duplicate concurrent downloads")

        // Emit progress.
        downloader.requests[0].onProgress(0.5, 50, 100)
        settle()
        if case .downloading(_, let progress, let written, let total) = manager.phase {
            check(progress == 0.5 && written == 50 && total == 100, "updates download progress")
        } else {
            check(false, "expected downloading phase")
        }

        // Simulate interrupted download: must NOT be treated as completed.
        downloader.requests[0].onCompletion(.failure(MockError.interrupted))
        settle()
        if case .failed(_, .downloadFailed) = manager.phase {
            check(true, "interrupted download enters failed state")
        } else {
            check(false, "expected failed phase after interruption")
        }

        // Retry and succeed.
        manager.retry()
        check(downloader.requests.count == 2, "retry starts a fresh download")
        let staged = FileManager.default.temporaryDirectory
            .appendingPathComponent("staged-\(UUID().uuidString).dmg")
        try! Data(repeating: 0x41, count: 100).write(to: staged)
        downloader.requests[1].onCompletion(.success(staged))
        settle()

        if case .downloaded(let release, let fileURL) = manager.phase {
            check(release.version == "1.6.0", "enters downloaded state on success")
            check(fileURL.lastPathComponent == "MacEverything-1.6.0-arm64 (1).dmg",
                  "avoids overwriting existing file in Downloads")
            check(FileManager.default.fileExists(atPath: fileURL.path), "installer exists in Downloads")
            manager.openDownloadedInstaller()
            manager.revealDownloadedInstallerInFinder()
            check(openedURL == fileURL && revealedURL == fileURL, "open and reveal actions target downloaded DMG")
        } else {
            check(false, "expected downloaded phase, got \(manager.phase)")
        }
    }

    static func testMissingAssetRetryRecovery() {
        let downloadsDir = makeTempDir()
        defer { try? FileManager.default.removeItem(at: downloadsDir) }

        let downloader = MockDownloader()
        var attempt = 0
        let manager = ReleaseUpdateManager(
            currentVersionProvider: { "1.5.0" },
            architectureProvider: { .x86_64 },
            fetcher: { _, _ in
                attempt += 1
                if attempt == 1 {
                    return [makeRelease(tag: "v1.6.0", assets: [makeAsset("MacEverything-1.6.0-arm64.dmg")])]
                }
                return [makeRelease(tag: "v1.6.0", assets: [
                    makeAsset("MacEverything-1.6.0-arm64.dmg"),
                    makeAsset("MacEverything-1.6.0-x86_64.dmg", size: 64),
                ])]
            },
            downloader: downloader,
            defaults: makeIsolatedDefaults(),
            downloadsDirectoryProvider: { downloadsDir }
        )

        manager.checkForUpdatesIfNeeded()
        settle()
        manager.startDownload()
        if case .failed(_, .missingAsset(let arch, let expected)) = manager.phase {
            check(arch == .x86_64 && expected == "MacEverything-1.6.0-x86_64.dmg",
                  "reports missing Intel installer clearly")
        } else {
            check(false, "expected missingAsset failure, got \(manager.phase)")
        }

        // Retry refetches releases and automatically starts download once asset appears.
        manager.retry()
        settle()
        check(attempt == 2 && downloader.requests.count == 1,
              "retry refetches release metadata and downloads once matching asset exists")
    }
}
