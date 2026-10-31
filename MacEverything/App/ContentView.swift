import SwiftUI

struct ContentView: View {
    @ObservedObject private var viewModel = SearchViewModel.shared
    @ObservedObject private var searchOptions = SearchOptions.shared
    @FocusState private var isSearchFieldFocused: Bool
    @FocusState private var isResultsFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            PermissionView()
            searchBar
            Divider()
            resultsHeader
            results
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(Color(nsColor: .textBackgroundColor))
            Divider()
            statusBar
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .frame(minWidth: 600, minHeight: 400)
        .onAppear { isSearchFieldFocused = true }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            viewModel.onWindowFocusChanged(true)
            isSearchFieldFocused = true
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didResignActiveNotification)) { _ in
            viewModel.onWindowFocusChanged(false)
        }
    }

    private var searchBar: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 18, weight: .medium))
                    .foregroundStyle(.secondary)
                    .accessibilityHidden(true)
                HighlightedSearchField(
                    text: $viewModel.searchText,
                    placeholder: "Search files and folders",
                    ghostSuggestion: viewModel.ghostSuggestion,
                    isFocused: $isSearchFieldFocused,
                    onTab: {
                        guard viewModel.ghostSuggestion != nil else { return false }
                        viewModel.acceptGhostSuggestion()
                        return true
                    },
                    onDownArrow: focusResults
                )
                .frame(height: 26)
                .onChange(of: viewModel.searchText) {
                    viewModel.onSearchTextChanged()
                }
                if !viewModel.searchText.isEmpty {
                    Button {
                        viewModel.searchText = ""
                        isSearchFieldFocused = true
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundStyle(.secondary)
                    }
                    .buttonStyle(.plain)
                    .help("Clear search")
                    .accessibilityLabel("Clear search")
                    .accessibilityIdentifier("clearButton")
                }
                Divider().frame(height: 20)
                Menu {
                    Toggle("Case Sensitive", isOn: $searchOptions.isCaseSensitive)
                    Toggle("Regex", isOn: $searchOptions.isRegex)
                    Toggle("Whole Word", isOn: $searchOptions.isWholeWord)
                    Toggle("Match Filename", isOn: $searchOptions.isMatchFilename)
                } label: {
                    Image(systemName: "line.3.horizontal.decrease")
                        .font(.system(size: 15, weight: .medium))
                        .foregroundStyle(searchOptions.hasActiveOptions ? Color.accentColor : .secondary)
                        .frame(width: 26, height: 26)
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
                .help("Search options")
                .accessibilityLabel("Search options")
                .accessibilityIdentifier("searchOptionsMenu")
                Button {
                    SearchSyntaxHelpWindowController.shared.showWindow()
                } label: {
                    Image(systemName: "questionmark.circle")
                        .font(.system(size: 15))
                        .foregroundStyle(.secondary)
                        .frame(width: 24, height: 26)
                }
                .buttonStyle(.plain)
                .help("Search syntax help (⇧⌘/)")
                .accessibilityLabel("Search syntax help")
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 10))
            .overlay {
                RoundedRectangle(cornerRadius: 10)
                    .strokeBorder(isSearchFieldFocused ? Color.accentColor.opacity(0.55) : Color.primary.opacity(0.12), lineWidth: 1)
            }

            if searchOptions.hasActiveOptions {
                SearchOptionBadges(options: searchOptions)
            }
        }
        .padding(16)
    }

    private var resultsHeader: some View {
        HStack(spacing: 8) {
            Label(viewModel.isContentSearch ? "File contents" : "Search results",
                  systemImage: viewModel.isContentSearch ? "doc.text.magnifyingglass" : "doc.on.doc")
                .font(.system(size: 12, weight: .semibold))
            if viewModel.totalMatches > 0 {
                Text("\(viewModel.totalMatches) matches")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .accessibilityIdentifier("matchCount")
            }
            Spacer()
            if !viewModel.isContentSearch {
                Text("Size")
                    .font(.system(size: 11))
                    .foregroundStyle(.tertiary)
            }
        }
        .padding(.horizontal, 22)
        .frame(height: 36)
    }

    @ViewBuilder
    private var results: some View {
        if viewModel.isScanning {
            emptyState(symbol: "externaldrive", title: "Building your file index",
                       detail: "\(viewModel.scannedCount) items scanned", isLoading: true)
        } else if viewModel.searchText.isEmpty {
            emptyState(symbol: "magnifyingglass", title: "Find anything on your Mac",
                       detail: "Search by file name or path. Use infile: to search file contents.")
        } else if viewModel.isContentSearch {
            if viewModel.contentKeyword.isEmpty {
                emptyState(symbol: "doc.text.magnifyingglass", title: "Search inside your files",
                           detail: "Type a keyword after infile: to find matching content.")
            } else if viewModel.contentResults.isEmpty && viewModel.scanComplete {
                if viewModel.isContentIndexing {
                    emptyState(symbol: "doc.text.magnifyingglass", title: "Content index is building",
                               detail: contentProgressText, isLoading: true)
                } else {
                    emptyState(symbol: "doc.text.magnifyingglass", title: "No content matches found",
                               detail: viewModel.contentIndexedCount == 0
                               ? "Choose file extensions in Content Settings to get started."
                               : "Try a different keyword or check your content settings.")
                }
            } else {
                ScrollViewReader { proxy in
                    List(selection: $viewModel.selectedResultID) {
                        ForEach(viewModel.contentResults) { item in
                            ContentResultRow(item: item, keyword: viewModel.contentKeyword)
                                .listRowInsets(EdgeInsets())
                                .listRowSeparator(.hidden)
                                .id(item.id)
                        }
                    }
                    .listStyle(.plain)
                    .scrollContentBackground(.hidden)
                    .padding(8)
                    .modifier(ResultListKeys(proxy: proxy, isFocused: $isResultsFocused, onExitTop: focusSearchField))
                    .accessibilityIdentifier("contentResultsList")
                }
            }
        } else if viewModel.displayItems.isEmpty && !viewModel.searchText.isEmpty && viewModel.scanComplete {
            emptyState(symbol: "magnifyingglass", title: "No results found",
                       detail: "Try a different name, or broaden your search filters.")
                .accessibilityIdentifier("noResultsLabel")
        } else {
            ScrollViewReader { proxy in
                List(selection: $viewModel.selectedResultID) {
                    ForEach(viewModel.displayItems) { item in
                        ResultRow(item: item, hints: viewModel.highlightHints)
                            .listRowInsets(EdgeInsets())
                            .listRowSeparator(.hidden)
                            .id(item.id)
                    }
                    if viewModel.hasMoreResults {
                        HStack(spacing: 8) {
                            ProgressView().controlSize(.small)
                            Text("Loading more results…")
                                .font(.system(size: 11))
                                .foregroundStyle(.secondary)
                        }
                        .padding(12)
                        .listRowSeparator(.hidden)
                        .onAppear { viewModel.loadMore() }
                    }
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
                .padding(8)
                .modifier(ResultListKeys(proxy: proxy, isFocused: $isResultsFocused, onExitTop: focusSearchField))
                .accessibilityIdentifier("fileResultsList")
            }
        }
    }

    /// ↓ in the search field moves focus to the results, selecting the first one.
    private func focusResults() -> Bool {
        guard let first = viewModel.firstResultID else { return false }
        if viewModel.selectedResultID == nil { viewModel.selectedResultID = first }
        isSearchFieldFocused = false
        isResultsFocused = true
        return true
    }

    private func focusSearchField() {
        QuickLookPreview.shared.close()
        isResultsFocused = false
        isSearchFieldFocused = true
    }

    private var contentProgressText: String {
        if let progress = viewModel.contentIndexProgress {
            return "Indexed \(progress.indexed) of \(progress.total) files. Results will appear when ready."
        }
        return "Results will appear after indexing completes."
    }

    private func emptyState(symbol: String, title: String, detail: String, isLoading: Bool = false) -> some View {
        VStack(spacing: 10) {
            if isLoading {
                ProgressView().controlSize(.regular).padding(.bottom, 6)
            } else {
                Image(systemName: symbol)
                    .font(.system(size: 30, weight: .light))
                    .foregroundStyle(.tertiary)
                    .padding(.bottom, 6)
            }
            Text(title).font(.system(size: 14, weight: .medium))
            Text(detail)
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .padding(32)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var statusBar: some View {
        HStack(spacing: 7) {
            if viewModel.isScanning || viewModel.isSyncing {
                ProgressView().controlSize(.mini)
                Text(viewModel.isScanning ? "Scanning…" : "Syncing…")
            } else if viewModel.isMonitoring {
                Circle().fill(.green).frame(width: 5, height: 5)
                Text("Live")
            }
            if viewModel.scanComplete {
                Text("\(viewModel.totalRecords) files indexed")
                    .accessibilityIdentifier("indexedCount")
            }
            if viewModel.isContentIndexing, let progress = viewModel.contentIndexProgress {
                Text("· Content \(progress.indexed)/\(progress.total)")
                    .lineLimit(1)
            }
            Spacer(minLength: 12)
            if viewModel.totalMatches > 0 {
                let count = viewModel.isContentSearch ? viewModel.contentResults.count : viewModel.displayItems.count
                Text("\(count) shown · \(String(format: "%.1f ms", viewModel.queryTimeMs))")
            } else {
                Text("Double-click to open · ⌘ click to reveal")
            }
        }
        .font(.system(size: 10))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 16)
        .frame(height: 30)
        .accessibilityIdentifier("statusBar")
    }
}

/// Keyboard handling shared by both result lists: Space toggles Quick Look, Esc closes it,
/// and ↑ on the first row returns to the search field. These only fire while the list has
/// focus, so Space still types normally in the search field.
private struct ResultListKeys: ViewModifier {
    @ObservedObject private var viewModel = SearchViewModel.shared
    let proxy: ScrollViewProxy
    var isFocused: FocusState<Bool>.Binding
    let onExitTop: () -> Void

    func body(content: Content) -> some View {
        content
            .focused(isFocused)
            .onKeyPress(.space) {
                QuickLookPreview.shared.toggle()
                return .handled
            }
            .onKeyPress(.escape) {
                guard QuickLookPreview.shared.isVisible else { return .ignored }
                QuickLookPreview.shared.close()
                return .handled
            }
            .onKeyPress(.upArrow) {
                guard viewModel.selectedResultID == nil || viewModel.selectedResultID == viewModel.firstResultID else {
                    return .ignored
                }
                onExitTop()
                return .handled
            }
            .onChange(of: viewModel.selectedResultID) {
                // Keeps the row visible when the selection moves from the Quick Look panel.
                if let id = viewModel.selectedResultID { proxy.scrollTo(id) }
            }
    }
}
