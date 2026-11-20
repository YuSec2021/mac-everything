import AppKit
import Foundation
import Observation

/// Semantic version parsed from a stable release tag or `CFBundleShortVersionString`.
/// Accepts an optional leading `v`/`V` and dot-separated non-negative integer components.
/// Rejects prerelease or build-metadata suffixes (e.g. `1.5.0-beta.1`).
struct ReleaseVersion: Comparable, Hashable, Codable {
    let components: [Int]

    var normalized: String {
        components.map(String.init).joined(separator: ".")
    }

    init?(parsing raw: String) {
        var trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.hasPrefix("v") || trimmed.hasPrefix("V") {
            trimmed.removeFirst()
        }
        guard !trimmed.isEmpty else { return nil }

        let parts = trimmed.split(separator: ".", omittingEmptySubsequences: false)
        guard !parts.isEmpty else { return nil }

        var parsed: [Int] = []
        parsed.reserveCapacity(parts.count)
        for part in parts {
            guard !part.isEmpty, part.allSatisfy({ $0 >= "0" && $0 <= "9" }),
                  let value = Int(part) else {
                return nil
            }
            parsed.append(value)
        }
        self.components = parsed
    }

    static func < (lhs: ReleaseVersion, rhs: ReleaseVersion) -> Bool {
        let maxCount = max(lhs.components.count, rhs.components.count)
        for index in 0..<maxCount {
            let l = index < lhs.components.count ? lhs.components[index] : 0
            let r = index < rhs.components.count ? rhs.components[index] : 0
            if l != r { return l < r }
        }
        return false
    }

    static func == (lhs: ReleaseVersion, rhs: ReleaseVersion) -> Bool {
        let maxCount = max(lhs.components.count, rhs.components.count)
        for index in 0..<maxCount {
            let l = index < lhs.components.count ? lhs.components[index] : 0
            let r = index < rhs.components.count ? rhs.components[index] : 0
            if l != r { return false }
        }
        return true
    }

    func hash(into hasher: inout Hasher) {
        var trimmed = components
        while trimmed.count > 1 && trimmed.last == 0 {
            trimmed.removeLast()
        }
        hasher.combine(trimmed)
    }
}

enum MacArchitecture: String, Codable, Equatable {
    case arm64
    case x86_64

    var displayName: String {
        switch self {
        case .arm64: return "Apple Silicon"
        case .x86_64: return "Intel"
        }
    }

    /// Detects the host CPU architecture, preferring native Apple Silicon even under Rosetta.
    static var current: MacArchitecture {
        var isARM64: Int32 = 0
        var size = MemoryLayout<Int32>.size
        if sysctlbyname("hw.optional.arm64", &isARM64, &size, nil, 0) == 0, isARM64 == 1 {
            return .arm64
        }
        #if arch(arm64)
        return .arm64
        #else
        return .x86_64
        #endif
    }
}

struct GitHubReleaseAsset: Decodable, Equatable {
    let name: String
    let browserDownloadURL: URL
    let size: Int64?

    enum CodingKeys: String, CodingKey {
        case name
        case browserDownloadURL = "browser_download_url"
        case size
    }
}

struct GitHubRelease: Decodable, Equatable {
    let tagName: String
    let name: String?
    let draft: Bool
    let prerelease: Bool
    let htmlURL: URL?
    let assets: [GitHubReleaseAsset]

    enum CodingKeys: String, CodingKey {
        case tagName = "tag_name"
        case name
        case draft
        case prerelease
        case htmlURL = "html_url"
        case assets
    }
}

struct AvailableRelease: Codable, Equatable {
    let tagName: String
    let version: String
    let architecture: MacArchitecture
    let assetName: String?
    let downloadURL: URL?
    let expectedBytes: Int64?
    let releasePageURL: URL?

    var displayTag: String {
        "v\(version)"
    }

    var expectedAssetFilename: String {
        "MacEverything-\(version)-\(architecture.rawValue).dmg"
    }

    var hasMatchingAsset: Bool {
        assetName != nil && downloadURL != nil
    }
}

enum ReleaseUpdateResolver {
    static let repositoryOwner = "YuSec2021"
    static let repositoryName = "mac-everything"

    static var releasesAPIURL: URL {
        URL(string: "https://api.github.com/repos/\(repositoryOwner)/\(repositoryName)/releases?per_page=20")!
    }

    /// Picks the highest stable (non-draft, non-prerelease) release strictly newer than `currentVersion`.
    static func selectLatestUpdate(
        from releases: [GitHubRelease],
        currentVersion: String,
        architecture: MacArchitecture
    ) -> AvailableRelease? {
        guard let installed = ReleaseVersion(parsing: currentVersion) else { return nil }

        var best: (version: ReleaseVersion, release: GitHubRelease)?
        for release in releases {
            guard !release.draft, !release.prerelease else { continue }
            guard let candidate = ReleaseVersion(parsing: release.tagName) else { continue }
            guard candidate > installed else { continue }
            if let currentBest = best, candidate <= currentBest.version {
                continue
            }
            best = (candidate, release)
        }

        guard let selected = best else { return nil }
        let versionString = selected.version.normalized
        let expectedFilename = "MacEverything-\(versionString)-\(architecture.rawValue).dmg"
        let matchedAsset = selected.release.assets.first {
            $0.name.compare(expectedFilename, options: [.caseInsensitive]) == .orderedSame
        }

        return AvailableRelease(
            tagName: selected.release.tagName,
            version: versionString,
            architecture: architecture,
            assetName: matchedAsset?.name,
            downloadURL: matchedAsset?.browserDownloadURL,
            expectedBytes: matchedAsset?.size,
            releasePageURL: selected.release.htmlURL
        )
    }

    /// Returns a non-colliding file URL in `directory` for `filename` (e.g. `Name (1).dmg`).
    static func uniqueDestinationURL(
        in directory: URL,
        filename: String,
        fileManager: FileManager = .default
    ) -> URL {
        let initial = directory.appendingPathComponent(filename)
        if !fileManager.fileExists(atPath: initial.path) {
            return initial
        }

        let nsName = filename as NSString
        let ext = nsName.pathExtension
        let stem = ext.isEmpty ? filename : nsName.deletingPathExtension
        for index in 1...999 {
            let candidateName = ext.isEmpty ? "\(stem) (\(index))" : "\(stem) (\(index)).\(ext)"
            let candidate = directory.appendingPathComponent(candidateName)
            if !fileManager.fileExists(atPath: candidate.path) {
                return candidate
            }
        }
        return directory.appendingPathComponent("\(stem)-\(UUID().uuidString)\(ext.isEmpty ? "" : ".\(ext)")")
    }
}

enum UpdateFailureReason: Equatable {
    case missingAsset(architecture: MacArchitecture, expectedFilename: String)
    case networkError(String)
    case downloadFailed(String)

    var shortTitle: String {
        switch self {
        case .missingAsset(let arch, _):
            return "缺少 \(arch.rawValue) 安装包 · 重试"
        case .networkError:
            return "检查失败 · 重试"
        case .downloadFailed:
            return "下载失败 · 重试"
        }
    }

    var detailMessage: String {
        switch self {
        case .missingAsset(let arch, let filename):
            return "未在 Release 中找到适用于 \(arch.displayName) 的安装包（\(filename)），点击重试。"
        case .networkError(let detail):
            return "检查更新失败：\(detail)。点击重试。"
        case .downloadFailed(let detail):
            return "下载安装包失败：\(detail)。点击重试。"
        }
    }
}

enum UpdatePhase: Equatable {
    case hidden
    case available(AvailableRelease)
    case downloading(AvailableRelease, progress: Double?, bytesWritten: Int64, totalBytes: Int64)
    case downloaded(AvailableRelease, fileURL: URL)
    case failed(AvailableRelease?, reason: UpdateFailureReason)
}

struct CachedUpdateCheck: Codable, Equatable {
    let checkedAt: Date
    let checkedVersion: String
    let architecture: MacArchitecture
    let availableRelease: AvailableRelease?
}

protocol ReleaseDownloadTasking: AnyObject {
    func cancel()
}

protocol ReleaseDownloadHandling {
    func download(
        from url: URL,
        expectedBytes: Int64?,
        onProgress: @escaping @Sendable (Double?, Int64, Int64) -> Void,
        onCompletion: @escaping @Sendable (Result<URL, Error>) -> Void
    ) -> ReleaseDownloadTasking
}

enum ReleaseUpdateError: LocalizedError {
    case invalidHTTPStatus(Int)
    case emptyFile
    case sizeMismatch(expected: Int64, actual: Int64)
    case missingDownloadsDirectory

    var errorDescription: String? {
        switch self {
        case .invalidHTTPStatus(let code):
            return "服务器返回状态码 \(code)"
        case .emptyFile:
            return "下载的文件为空"
        case .sizeMismatch(let expected, let actual):
            return "安装包大小不完整（已下载 \(actual) / 预期 \(expected) 字节）"
        case .missingDownloadsDirectory:
            return "无法访问用户的下载目录"
        }
    }
}

private final class URLSessionDownloadTaskHandle: ReleaseDownloadTasking {
    private let task: URLSessionDownloadTask
    private let session: URLSession

    init(task: URLSessionDownloadTask, session: URLSession) {
        self.task = task
        self.session = session
    }

    func cancel() {
        task.cancel()
        session.invalidateAndCancel()
    }
}

private final class ReleaseDownloadDelegate: NSObject, URLSessionDownloadDelegate {
    private let expectedBytes: Int64?
    private let onProgress: @Sendable (Double?, Int64, Int64) -> Void
    private let onCompletion: @Sendable (Result<URL, Error>) -> Void
    private var finished = false
    private let lock = NSLock()

    init(
        expectedBytes: Int64?,
        onProgress: @escaping @Sendable (Double?, Int64, Int64) -> Void,
        onCompletion: @escaping @Sendable (Result<URL, Error>) -> Void
    ) {
        self.expectedBytes = expectedBytes
        self.onProgress = onProgress
        self.onCompletion = onCompletion
    }

    private func completeOnce(_ result: Result<URL, Error>) {
        lock.lock()
        let shouldFire = !finished
        finished = true
        lock.unlock()
        if shouldFire {
            onCompletion(result)
        }
    }

    func urlSession(
        _ session: URLSession,
        downloadTask: URLSessionDownloadTask,
        didWriteData bytesWritten: Int64,
        totalBytesWritten: Int64,
        totalBytesExpectedToWrite: Int64
    ) {
        let total = totalBytesExpectedToWrite > 0 ? totalBytesExpectedToWrite : (expectedBytes ?? -1)
        let progress: Double? = total > 0 ? min(max(Double(totalBytesWritten) / Double(total), 0), 1) : nil
        onProgress(progress, totalBytesWritten, total)
    }

    func urlSession(
        _ session: URLSession,
        downloadTask: URLSessionDownloadTask,
        didFinishDownloadingTo location: URL
    ) {
        if let httpResponse = downloadTask.response as? HTTPURLResponse,
           !(200...299).contains(httpResponse.statusCode) {
            try? FileManager.default.removeItem(at: location)
            completeOnce(.failure(ReleaseUpdateError.invalidHTTPStatus(httpResponse.statusCode)))
            session.finishTasksAndInvalidate()
            return
        }

        // Move out of URLSession's ephemeral location before returning from this callback.
        let stagingURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("MacEverything-update-\(UUID().uuidString).dmg.part")
        do {
            let attributes = try FileManager.default.attributesOfItem(atPath: location.path)
            let fileSize = (attributes[.size] as? NSNumber)?.int64Value ?? 0
            guard fileSize > 0 else {
                try? FileManager.default.removeItem(at: location)
                completeOnce(.failure(ReleaseUpdateError.emptyFile))
                session.finishTasksAndInvalidate()
                return
            }
            if let expectedBytes, expectedBytes > 0, fileSize != expectedBytes {
                try? FileManager.default.removeItem(at: location)
                completeOnce(.failure(ReleaseUpdateError.sizeMismatch(expected: expectedBytes, actual: fileSize)))
                session.finishTasksAndInvalidate()
                return
            }
            try FileManager.default.moveItem(at: location, to: stagingURL)
            completeOnce(.success(stagingURL))
        } catch {
            try? FileManager.default.removeItem(at: stagingURL)
            completeOnce(.failure(error))
        }
        session.finishTasksAndInvalidate()
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error {
            completeOnce(.failure(error))
        }
        session.finishTasksAndInvalidate()
    }
}

struct URLSessionReleaseDownloader: ReleaseDownloadHandling {
    func download(
        from url: URL,
        expectedBytes: Int64?,
        onProgress: @escaping @Sendable (Double?, Int64, Int64) -> Void,
        onCompletion: @escaping @Sendable (Result<URL, Error>) -> Void
    ) -> ReleaseDownloadTasking {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 30
        config.timeoutIntervalForResource = 60 * 30
        let delegate = ReleaseDownloadDelegate(
            expectedBytes: expectedBytes,
            onProgress: onProgress,
            onCompletion: onCompletion
        )
        let session = URLSession(configuration: config, delegate: delegate, delegateQueue: nil)
        let task = session.downloadTask(with: url)
        task.resume()
        return URLSessionDownloadTaskHandle(task: task, session: session)
    }
}

@Observable
@MainActor
final class ReleaseUpdateManager {
    static let shared = ReleaseUpdateManager()
    nonisolated static let cacheKey = "MacEverything.ReleaseUpdateCache.v1"
    nonisolated static let defaultCacheTTL: TimeInterval = 6 * 60 * 60
    nonisolated static let defaultFailureCooldown: TimeInterval = 5 * 60

    typealias ReleasesFetcher = (URL, String) async throws -> [GitHubRelease]

    private(set) var phase: UpdatePhase = .hidden
    private(set) var isChecking = false

    private let currentVersionProvider: () -> String
    private let architectureProvider: () -> MacArchitecture
    private let fetcher: ReleasesFetcher
    private let downloader: ReleaseDownloadHandling
    private let defaults: UserDefaults
    private let downloadsDirectoryProvider: () -> URL?
    private let nowProvider: () -> Date
    private let cacheTTL: TimeInterval
    private let failureCooldown: TimeInterval
    private let openFileHandler: (URL) -> Void
    private let revealFileHandler: (URL) -> Void
    var logHandler: (String) -> Void

    private var activeDownloadTask: ReleaseDownloadTasking?
    private var lastSilentFailureAt: Date?
    private var checkTask: Task<Void, Never>?

    init(
        currentVersionProvider: @escaping () -> String = {
            Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.0.0"
        },
        architectureProvider: @escaping () -> MacArchitecture = { MacArchitecture.current },
        fetcher: @escaping ReleasesFetcher = ReleaseUpdateManager.defaultFetchReleases,
        downloader: ReleaseDownloadHandling = URLSessionReleaseDownloader(),
        defaults: UserDefaults = .standard,
        downloadsDirectoryProvider: @escaping () -> URL? = {
            FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask).first
        },
        nowProvider: @escaping () -> Date = Date.init,
        cacheTTL: TimeInterval = ReleaseUpdateManager.defaultCacheTTL,
        failureCooldown: TimeInterval = ReleaseUpdateManager.defaultFailureCooldown,
        openFileHandler: @escaping (URL) -> Void = { NSWorkspace.shared.open($0) },
        revealFileHandler: @escaping (URL) -> Void = { NSWorkspace.shared.activateFileViewerSelecting([$0]) },
        logger: @escaping (String) -> Void = { _ in }
    ) {
        self.currentVersionProvider = currentVersionProvider
        self.architectureProvider = architectureProvider
        self.fetcher = fetcher
        self.downloader = downloader
        self.defaults = defaults
        self.downloadsDirectoryProvider = downloadsDirectoryProvider
        self.nowProvider = nowProvider
        self.cacheTTL = cacheTTL
        self.failureCooldown = failureCooldown
        self.openFileHandler = openFileHandler
        self.revealFileHandler = revealFileHandler
        self.logHandler = logger

        applyValidCacheIfAvailable()
    }

    nonisolated static func defaultFetchReleases(url: URL, currentVersion: String) async throws -> [GitHubRelease] {
        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 15)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("2022-11-28", forHTTPHeaderField: "X-GitHub-Api-Version")
        request.setValue("MacEverything/\(currentVersion)", forHTTPHeaderField: "User-Agent")

        let (data, response) = try await URLSession.shared.data(for: request)
        if let httpResponse = response as? HTTPURLResponse, !(200...299).contains(httpResponse.statusCode) {
            throw ReleaseUpdateError.invalidHTTPStatus(httpResponse.statusCode)
        }
        return try JSONDecoder().decode([GitHubRelease].self, from: data)
    }

    /// Checks GitHub Releases asynchronously if no fresh cached result is available.
    func checkForUpdatesIfNeeded(force: Bool = false, autoDownloadOnSuccess: Bool = false) {
        if case .downloading = phase { return }
        if case .downloaded(_, let url) = phase, FileManager.default.fileExists(atPath: url.path), !force {
            return
        }
        guard !isChecking else { return }

        if !force {
            if applyValidCacheIfAvailable() {
                return
            }
            if let lastFailure = lastSilentFailureAt,
               nowProvider().timeIntervalSince(lastFailure) < failureCooldown {
                return
            }
        }

        isChecking = true
        let currentVersion = currentVersionProvider()
        let architecture = architectureProvider()
        let fetcher = self.fetcher
        let url = ReleaseUpdateResolver.releasesAPIURL

        checkTask?.cancel()
        checkTask = Task { [weak self] in
            do {
                let releases = try await fetcher(url, currentVersion)
                guard !Task.isCancelled else { return }
                self?.handleCheckSuccess(
                    releases: releases,
                    currentVersion: currentVersion,
                    architecture: architecture,
                    autoDownload: autoDownloadOnSuccess
                )
            } catch {
                guard !Task.isCancelled else { return }
                self?.handleCheckFailure(error: error, isUserInitiated: force)
            }
        }
    }

    /// Starts downloading the installer for the currently available release.
    func startDownload() {
        switch phase {
        case .downloading:
            return
        case .downloaded(_, let url):
            if FileManager.default.fileExists(atPath: url.path) {
                return
            }
        case .available(let release):
            beginDownload(for: release)
        case .failed(let release?, _):
            beginDownload(for: release)
        case .hidden, .failed(nil, _):
            return
        }
    }

    /// Retries a failed update check, missing-asset lookup, or failed download.
    func retry() {
        switch phase {
        case .failed(_, .missingAsset), .failed(_, .networkError), .failed(nil, _):
            checkForUpdatesIfNeeded(force: true, autoDownloadOnSuccess: true)
        case .failed(let release?, .downloadFailed):
            beginDownload(for: release)
        case .available(let release):
            beginDownload(for: release)
        case .downloading, .downloaded, .hidden:
            break
        }
    }

    func openDownloadedInstaller() {
        guard case .downloaded(let release, let fileURL) = phase else { return }
        guard FileManager.default.fileExists(atPath: fileURL.path) else {
            phase = .failed(release, reason: .downloadFailed("安装包已被移动或删除"))
            return
        }
        openFileHandler(fileURL)
    }

    func revealDownloadedInstallerInFinder() {
        guard case .downloaded(let release, let fileURL) = phase else { return }
        guard FileManager.default.fileExists(atPath: fileURL.path) else {
            phase = .failed(release, reason: .downloadFailed("安装包已被移动或删除"))
            return
        }
        revealFileHandler(fileURL)
    }

    // MARK: - Internal helpers

    @discardableResult
    private func applyValidCacheIfAvailable() -> Bool {
        guard let cache = loadCache() else { return false }
        let currentVersion = currentVersionProvider()
        let currentArch = architectureProvider()

        guard cache.checkedVersion == currentVersion,
              cache.architecture == currentArch,
              nowProvider().timeIntervalSince(cache.checkedAt) < cacheTTL else {
            if cache.checkedVersion != currentVersion {
                defaults.removeObject(forKey: Self.cacheKey)
            }
            return false
        }

        if let release = cache.availableRelease,
           let cachedVersion = ReleaseVersion(parsing: release.version),
           let installedVersion = ReleaseVersion(parsing: currentVersion),
           cachedVersion > installedVersion {
            phase = .available(release)
        } else {
            phase = .hidden
        }
        return true
    }

    private func handleCheckSuccess(
        releases: [GitHubRelease],
        currentVersion: String,
        architecture: MacArchitecture,
        autoDownload: Bool
    ) {
        isChecking = false
        lastSilentFailureAt = nil

        let selected = ReleaseUpdateResolver.selectLatestUpdate(
            from: releases,
            currentVersion: currentVersion,
            architecture: architecture
        )
        saveCache(CachedUpdateCheck(
            checkedAt: nowProvider(),
            checkedVersion: currentVersion,
            architecture: architecture,
            availableRelease: selected
        ))

        if let release = selected {
            if autoDownload {
                beginDownload(for: release)
            } else {
                phase = .available(release)
            }
        } else {
            phase = .hidden
        }
    }

    private func handleCheckFailure(error: Error, isUserInitiated: Bool) {
        isChecking = false
        logHandler("Release check failed: \(error.localizedDescription)")
        if isUserInitiated {
            let existingRelease: AvailableRelease?
            if case .failed(let release, _) = phase {
                existingRelease = release
            } else if case .available(let release) = phase {
                existingRelease = release
            } else {
                existingRelease = nil
            }
            phase = .failed(existingRelease, reason: .networkError(error.localizedDescription))
        } else {
            lastSilentFailureAt = nowProvider()
        }
    }

    private func beginDownload(for release: AvailableRelease) {
        guard activeDownloadTask == nil else { return }
        guard let downloadURL = release.downloadURL, let assetName = release.assetName else {
            phase = .failed(
                release,
                reason: .missingAsset(
                    architecture: release.architecture,
                    expectedFilename: release.expectedAssetFilename
                )
            )
            return
        }

        phase = .downloading(release, progress: nil, bytesWritten: 0, totalBytes: release.expectedBytes ?? -1)
        activeDownloadTask = downloader.download(
            from: downloadURL,
            expectedBytes: release.expectedBytes,
            onProgress: { [weak self] progress, written, total in
                Task { @MainActor [weak self] in
                    guard let self, case .downloading(let activeRelease, _, _, _) = self.phase,
                          activeRelease == release else { return }
                    self.phase = .downloading(release, progress: progress, bytesWritten: written, totalBytes: total)
                }
            },
            onCompletion: { [weak self] result in
                Task { @MainActor [weak self] in
                    self?.handleDownloadCompletion(result, for: release, assetName: assetName)
                }
            }
        )
    }

    private func handleDownloadCompletion(
        _ result: Result<URL, Error>,
        for release: AvailableRelease,
        assetName: String
    ) {
        activeDownloadTask = nil

        switch result {
        case .success(let stagedFileURL):
            do {
                guard let downloadsDir = downloadsDirectoryProvider() else {
                    try? FileManager.default.removeItem(at: stagedFileURL)
                    throw ReleaseUpdateError.missingDownloadsDirectory
                }
                try FileManager.default.createDirectory(at: downloadsDir, withIntermediateDirectories: true)
                let destinationURL = ReleaseUpdateResolver.uniqueDestinationURL(
                    in: downloadsDir,
                    filename: assetName
                )
                try FileManager.default.moveItem(at: stagedFileURL, to: destinationURL)
                phase = .downloaded(release, fileURL: destinationURL)
            } catch {
                try? FileManager.default.removeItem(at: stagedFileURL)
                logHandler("Failed to finalize update download: \(error.localizedDescription)")
                phase = .failed(release, reason: .downloadFailed(error.localizedDescription))
            }

        case .failure(let error):
            logHandler("Update download failed: \(error.localizedDescription)")
            phase = .failed(release, reason: .downloadFailed(error.localizedDescription))
        }
    }

    private func loadCache() -> CachedUpdateCheck? {
        guard let data = defaults.data(forKey: Self.cacheKey) else { return nil }
        return try? JSONDecoder().decode(CachedUpdateCheck.self, from: data)
    }

    private func saveCache(_ cache: CachedUpdateCheck) {
        if let data = try? JSONEncoder().encode(cache) {
            defaults.set(data, forKey: Self.cacheKey)
        }
    }
}
