import Foundation
import Observation

struct FileItem: Identifiable {
    let id: String      // path-based stable ID
    let index: UInt32   // engine index for record lookup
    let name: String
    let path: String
    let type: UInt8
    let size: UInt64
    let modTime: time_t
}

struct ContentFileItem: Identifiable {
    let id: String      // stable ID: filePath + ":" + matchOffset
    let fileName: String
    let filePath: String
    let snippet: String
    let matchOffset: UInt32
    let fileType: UInt8
}

/// Observation tracks each property separately, so a view only re-renders for the
/// properties it actually read (e.g. scan progress no longer redraws the result rows).
@MainActor
@Observable
final class SearchViewModel {
    static let shared = SearchViewModel()
    var searchText: String = ""
    private(set) var displayItems: [FileItem] = []
    var totalMatches: Int = 0
    var isScanning: Bool = false
    var scanComplete: Bool = false
    var totalRecords: UInt32 = 0
    var queryTimeMs: Double = 0
    var isMonitoring: Bool = false
    var scannedCount: UInt64 = 0
    @ObservationIgnored var isLoadingMore: Bool = false
    var isContentSearch: Bool = false {
        didSet { if isContentSearch != oldValue { rebuildResultIndex() } }
    }
    private(set) var contentResults: [ContentFileItem] = []
    var isContentIndexing: Bool = false
    var contentIndexProgress: (indexed: UInt32, total: UInt32)?
    var contentIndexedCount: UInt32 = 0
    var isSyncing: Bool = false
    var ghostSuggestion: String? = nil
    /// `id` of the selected row in whichever result list is showing (file or content).
    var selectedResultID: String? {
        didSet { if selectedResultID != oldValue { onSelectionChange?() } }
    }
    /// Called after `selectedResultID` changes; Quick Look follows the selection with it.
    @ObservationIgnored var onSelectionChange: (() -> Void)?

    /// Structured highlight hints extracted from the C++ query AST.
    /// Replaces the old keyword-based approach with field-aware, mode-aware hints.
    var highlightHints: [HighlightHint] {
        let query = searchOptions.buildQuery(searchText)
        guard !query.isEmpty else { return [] }
        if query != cachedHighlightQuery {
            cachedHighlightQuery = query
            cachedHighlightHints = bridge.parseHighlightHints(query).map { HighlightHint(from: $0) }
        }
        return cachedHighlightHints
    }

    // Written while a view reads `highlightHints`, so it must not be observed.
    @ObservationIgnored private var cachedHighlightQuery: String?
    @ObservationIgnored private var cachedHighlightHints: [HighlightHint] = []

    @ObservationIgnored private let bridge = MacSearchBridge.shared()
    @ObservationIgnored private let historyStore = SearchHistoryStore()
    @ObservationIgnored private let searchOptions = SearchOptions.shared
    @ObservationIgnored private var searchTask: Task<Void, Never>?
    @ObservationIgnored private var settledTask: Task<Void, Never>?
    @ObservationIgnored private var resultSnapshot: MEQuerySnapshot?
    private var loadedCount: Int = 0
    @ObservationIgnored private var searchGeneration: UInt64 = 0
    /// Row position by `id` for the list that is showing, so selection lookups are O(1).
    @ObservationIgnored private var resultIndexByID: [String: Int] = [:]
    private static let guiSessionId: UInt64 = 1

    private static let pageSize: Int = 100
    private static let maxResults: UInt32 = 10000
    private static let indexChangeThrottleNs: UInt64 = 5_000_000_000 // 5 seconds

    @ObservationIgnored private var indexChangeTask: Task<Void, Never>?
    @ObservationIgnored let refreshThrottle = IndexRefreshThrottle()

    static var cacheDir: String {
        let base = NSSearchPathForDirectoriesInDomains(
            .cachesDirectory, .userDomainMask, true
        ).first ?? NSTemporaryDirectory()
        let appCache = (base as NSString).appendingPathComponent("com.maceverything.app")
        try? FileManager.default.createDirectory(
            atPath: appCache, withIntermediateDirectories: true
        )
        return appCache
    }

    static var cachePath: String {
        return (cacheDir as NSString).appendingPathComponent("index.bin")
    }

    static var walPath: String {
        return (cacheDir as NSString).appendingPathComponent("index.wal")
    }

    static var pagesPath: String {
        return (cacheDir as NSString).appendingPathComponent("index.pages")
    }

    static var ptablePath: String {
        return (cacheDir as NSString).appendingPathComponent("index.ptable")
    }

    init() {
        observeSearchOptions()
        startIncremental()
    }

    /// Re-runs the search when a search option changes. `onChange` fires before the new
    /// value is stored, so the search runs on the next main-actor turn; by then the
    /// options' mutually exclusive resets have landed too, and they cost one search.
    private func observeSearchOptions() {
        withObservationTracking {
            _ = searchOptions.buildQuery(" ")
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                self?.onSearchOptionsChanged()
                self?.observeSearchOptions()
            }
        }
    }

    private func onSearchOptionsChanged() {
        guard scanComplete, !searchText.isEmpty, !isContentSearch else { return }
        searchTask?.cancel()
        searchGeneration &+= 1
        bridge.cancelSession(Self.guiSessionId)
        performSearch(searchText)
    }

    func startIncremental() {
        isScanning = true
        scannedCount = 0

        bridge.onScanProgress = { [weak self] fileCount, dirCount in
            Task { @MainActor in
                self?.scannedCount = fileCount + dirCount
            }
        }

        bridge.onContentIndexProgress = { [weak self] indexed, total in
            Task { @MainActor in
                guard let self = self else { return }
                self.isContentIndexing = true
                self.contentIndexProgress = (indexed, total)
            }
        }

        bridge.onContentIndexComplete = { [weak self] totalIndexed in
            Task { @MainActor in
                guard let self = self else { return }
                self.isContentIndexing = false
                self.contentIndexProgress = nil
                self.contentIndexedCount = totalIndexed
                // Auto-refresh content search results after indexing completes
                if self.isContentSearch && !self.contentKeyword.isEmpty {
                    self.performContentSearch(self.contentKeyword)
                }
            }
        }

        // Set up FSEvents change callback before starting
        bridge.onIndexChanged = { [weak self] in
            Task { @MainActor in
                self?.onIndexChanged()
            }
        }

        bridge.startIncremental(from: "/",
                                cachePath: Self.cachePath,
                                walPath: Self.walPath) { [weak self] count, didFullScan in
            Task { @MainActor in
                guard let self = self else { return }
                self.totalRecords = count
                self.isScanning = false
                self.scanComplete = true
                self.isMonitoring = self.bridge.isMonitoring
                self.isSyncing = self.bridge.isSyncing

                if !self.searchText.isEmpty {
                    self.performSearch(self.searchText)
                }
            }
        }
    }

    func rebuildIndex() {
        guard !isScanning else { return }
        searchTask?.cancel()
        indexChangeTask?.cancel()
        searchGeneration &+= 1
        resultSnapshot = nil
        scanComplete = false
        setDisplayItems([])
        totalMatches = 0
        queryTimeMs = 0
        setContentResults([])
        isContentSearch = false
        contentKeyword = ""

        // Delete cached index files so startIncremental does a full scan
        try? FileManager.default.removeItem(atPath: Self.cachePath)
        try? FileManager.default.removeItem(atPath: Self.walPath)
        try? FileManager.default.removeItem(atPath: Self.pagesPath)
        try? FileManager.default.removeItem(atPath: Self.ptablePath)

        startIncremental()
    }

    func onSearchTextChanged() {
        searchTask?.cancel()
        searchGeneration &+= 1
        isLoadingMore = false
        let text = searchText

        if text.isEmpty {
            totalMatches = 0
            queryTimeMs = 0
            resultSnapshot = nil
            loadedCount = 0
            isContentSearch = false
            setContentResults([])
            contentKeyword = "" // H-9: reset cached keyword
            ghostSuggestion = nil
            selectedResultID = nil
            settledTask?.cancel()
            // Cancel any in-flight queries for this GUI session
            bridge.cancelSession(Self.guiSessionId)
            setDisplayItems([])
            return
        }

        let lowerText = text.lowercased()
        if lowerText.hasPrefix("infile:") {
            isContentSearch = true
            setDisplayItems([])
            resultSnapshot = nil
            loadedCount = 0

            let keyword = String(text.dropFirst(7))
            contentKeyword = keyword // H-9: cache computed keyword
            guard !keyword.isEmpty else {
                setContentResults([])
                totalMatches = 0
                queryTimeMs = 0
                return
            }

            searchTask = Task { @MainActor in
                // 300ms debounce for content search (heavier)
                try? await Task.sleep(nanoseconds: 300_000_000)
                guard !Task.isCancelled else { return }
                performContentSearch(keyword)
            }
        } else {
            isContentSearch = false
            setContentResults([])
            contentKeyword = "" // H-9: reset cached keyword

            searchTask = Task { @MainActor in
                // 80ms debounce
                try? await Task.sleep(nanoseconds: 80_000_000)
                guard !Task.isCancelled else { return }
                performSearch(text)
            }
        }

        updateGhostSuggestion()
        scheduleHistoryRecord()
    }

    private func performSearch(_ keyword: String) {
        searchGeneration &+= 1
        isLoadingMore = false
        let bridge = self.bridge
        let maxResults = Self.maxResults
        let pageSize = Self.pageSize
        let gen = searchGeneration
        let query = searchOptions.buildQuery(keyword)
        Task.detached { [weak self] in
            let start = CFAbsoluteTimeGetCurrent()
            var snapshot: MEQuerySnapshot?
            var page: [MEFileResult]?
            for _ in 0..<2 {
                guard let candidate = bridge.querySnapshot(query, maxResults: maxResults, sessionId: Self.guiSessionId),
                      let firstPage = candidate.results(atOffset: 0, count: pageSize) else { continue }
                snapshot = candidate
                page = firstPage
                break
            }
            guard let snapshot, let page else { return }
            let items = page.map { r in
                FileItem(id: "\(r.path)/\(r.name)", index: 0, name: r.name, path: r.path,
                         type: r.type, size: r.size, modTime: r.modTime)
            }
            let elapsed = (CFAbsoluteTimeGetCurrent() - start) * 1000
            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else { return }
                self.resultSnapshot = snapshot
                self.loadedCount = min(snapshot.count, pageSize)
                self.setDisplayItems(items)
                self.dropStaleSelection()
                self.totalMatches = snapshot.count
                self.queryTimeMs = elapsed
            }
        }
    }

    private func performContentSearch(_ keyword: String) {
        let bridge = self.bridge
        let gen = searchGeneration
        Task.detached { [weak self] in
            let start = CFAbsoluteTimeGetCurrent()
            let results = bridge.queryContent(keyword, maxResults: 200)
            let elapsed = (CFAbsoluteTimeGetCurrent() - start) * 1000

            var items: [ContentFileItem] = []
            items.reserveCapacity(results.count)
            for r in results {
                items.append(ContentFileItem(
                    id: "\(r.filePath):\(r.matchOffset)",
                    fileName: r.fileName,
                    filePath: r.filePath,
                    snippet: r.snippet,
                    matchOffset: r.matchOffset,
                    fileType: r.fileType
                ))
            }

            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else { return }
                self.setContentResults(items)
                self.dropStaleSelection()
                self.totalMatches = items.count
                self.queryTimeMs = elapsed
            }
        }
    }

    func loadMore() {
        guard !isLoadingMore, let snapshot = resultSnapshot, loadedCount < snapshot.count else { return }
        isLoadingMore = true
        let currentLoaded = loadedCount
        let pageSize = Self.pageSize
        let gen = searchGeneration
        Task.detached { [weak self] in
            let page = snapshot.results(atOffset: currentLoaded, count: pageSize)
            let items = page?.map { r in
                FileItem(id: "\(r.path)/\(r.name)", index: 0, name: r.name, path: r.path,
                         type: r.type, size: r.size, modTime: r.modTime)
            }
            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen, self.resultSnapshot === snapshot else { return }
                self.isLoadingMore = false
                guard let items else {
                    self.performSearch(self.searchText)
                    return
                }
                self.appendDisplayItems(items)
                self.loadedCount = min(currentLoaded + pageSize, snapshot.count)
            }
        }
    }

    private func onIndexChanged() {
        if refreshThrottle.indexChanged() {
            performIndexRefresh()
            scheduleCooldown()
        }
    }

    func onWindowFocusChanged(_ focused: Bool) {
        if !focused {
            // Record search text to history when window loses focus
            let text = searchText
            if text.count >= 2 && !text.lowercased().hasPrefix("infile:") {
                historyStore.recordQuery(text)
            }
        }
        if refreshThrottle.focusChanged(focused) {
            performIndexRefresh()
            scheduleCooldown()
        }
    }

    private func scheduleCooldown() {
        indexChangeTask?.cancel()
        indexChangeTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: Self.indexChangeThrottleNs)
            guard !Task.isCancelled, let self else { return }
            self.indexChangeTask = nil
            if self.refreshThrottle.cooldownExpired() {
                self.performIndexRefresh()
                self.scheduleCooldown()
            }
        }
    }

    private func performIndexRefresh() {
        totalRecords = bridge.liveRecordCount()
        isMonitoring = bridge.isMonitoring
        isSyncing = bridge.isSyncing
        contentIndexedCount = bridge.contentIndexedFileCount()

        // Skip expensive search/query when app is not focused.
        // Results will refresh on focus regain via onWindowFocusChanged.
        guard refreshThrottle.isFocused else { return }

        if !searchText.isEmpty && !isContentSearch {
            performSearch(searchText)
        } else if isContentSearch && !contentKeyword.isEmpty {
            performContentSearch(contentKeyword)
        }
    }

    // MARK: - Selection

    // Only one list shows at a time, and the other one is always empty, so a single
    // index covers both.
    private func setDisplayItems(_ items: [FileItem]) {
        displayItems = items
        if !isContentSearch { rebuildResultIndex() }
    }

    private func appendDisplayItems(_ items: [FileItem]) {
        let start = displayItems.count
        displayItems.append(contentsOf: items)
        for (offset, item) in items.enumerated() { resultIndexByID[item.id] = start + offset }
    }

    private func setContentResults(_ items: [ContentFileItem]) {
        contentResults = items
        if isContentSearch { rebuildResultIndex() }
    }

    private func rebuildResultIndex() {
        let ids = isContentSearch ? contentResults.map(\.id) : displayItems.map(\.id)
        resultIndexByID = Dictionary(ids.enumerated().map { ($1, $0) }, uniquingKeysWith: { first, _ in first })
    }

    private var resultCount: Int {
        isContentSearch ? contentResults.count : displayItems.count
    }

    private func resultID(at index: Int) -> String {
        isContentSearch ? contentResults[index].id : displayItems[index].id
    }

    var firstResultID: String? {
        isContentSearch ? contentResults.first?.id : displayItems.first?.id
    }

    /// The file a result row stands for, and whether it opens as an application.
    func resultTarget(for id: String) -> (url: URL, isApp: Bool)? {
        guard let index = resultIndexByID[id], index < resultCount else { return nil }
        if isContentSearch {
            return (URL(fileURLWithPath: contentResults[index].filePath), false)
        }
        let item = displayItems[index]
        return (URL(fileURLWithPath: item.path + "/" + item.name), item.type == 5)
    }

    var selectedResultURL: URL? {
        selectedResultID.flatMap { resultTarget(for: $0)?.url }
    }

    func moveSelection(by delta: Int) {
        let count = resultCount
        guard count > 0 else { return }
        guard let current = selectedResultID, let index = resultIndexByID[current] else {
            selectedResultID = resultID(at: 0)
            return
        }
        selectedResultID = resultID(at: min(max(index + delta, 0), count - 1))
        if !isContentSearch, index + delta >= count - 1 { loadMore() }
    }

    private func dropStaleSelection() {
        if let id = selectedResultID, resultIndexByID[id] == nil { selectedResultID = nil }
    }

    var hasMoreResults: Bool {
        loadedCount < (resultSnapshot?.count ?? 0)
    }

    // H-9: Cached to avoid recomputing lowercased() + hasPrefix on every access
    private(set) var contentKeyword: String = ""

    // MARK: - Ghost text autocomplete

    private static let systemKeywords: [String] = [
        // Basic filters (most common first)
        "ext:", "size:", "file:", "folder:", "path:", "nopath:", "parent:", "depth:", "len:",
        // Date filters
        "dm:", "dc:", "da:", "datemodified:", "datecreated:", "dateaccessed:",
        // Modifiers
        "case:", "nocase:", "regex:", "ww:", "wfn:", "wholeword:", "wholefilename:",
        // Macros
        "audio:", "video:", "pic:", "doc:", "exe:", "zip:",
        // Other
        "content:", "type:",
    ]

    private static func systemKeywordMatch(for text: String) -> String? {
        let lower = text.lowercased()
        // Only match single partial words: no spaces, no colon already present
        guard !lower.isEmpty, !lower.contains(" "), !lower.contains(":") else { return nil }

        let matches = systemKeywords.filter { $0.hasPrefix(lower) }
        // Prefer shorter keywords, then alphabetical
        return matches.min { a, b in
            if a.count != b.count { return a.count < b.count }
            return a < b
        }
    }

    private func updateGhostSuggestion() {
        let text = searchText
        guard !text.isEmpty, !text.lowercased().hasPrefix("infile:") else {
            ghostSuggestion = nil
            return
        }
        // Priority 1: search history match
        if let match = historyStore.bestMatch(for: text),
           match.lowercased() != text.lowercased() {
            ghostSuggestion = text + match.dropFirst(text.count)
            return
        }
        // Priority 2: system keyword match
        if let keyword = Self.systemKeywordMatch(for: text) {
            ghostSuggestion = text + keyword.dropFirst(text.count)
            return
        }
        ghostSuggestion = nil
    }

    private func scheduleHistoryRecord() {
        settledTask?.cancel()
        let text = searchText
        guard text.count >= 2, !text.lowercased().hasPrefix("infile:") else { return }
        settledTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 2_000_000_000) // 2 seconds
            guard !Task.isCancelled, let self, self.searchText == text else { return }
            self.historyStore.recordQuery(text)
        }
    }

    func acceptGhostSuggestion() {
        guard let suggestion = ghostSuggestion else { return }
        searchText = suggestion
        ghostSuggestion = nil
        historyStore.recordQuery(suggestion)
    }
}
