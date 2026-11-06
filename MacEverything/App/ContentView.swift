import SwiftUI

struct ContentView: View {
    @Bindable private var viewModel = SearchViewModel.shared
    @Bindable private var searchOptions = SearchOptions.shared
    @FocusState private var isSearchFieldFocused: Bool
    @FocusState private var isResultsFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            PermissionView()
            searchBar
            Glass.hairline.frame(height: 1)
            resultsHeader
            Glass.hairline.frame(height: 1)
            results
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            Glass.hairline.frame(height: 1)
            ResultsStatusBar()
        }
        .foregroundStyle(Glass.text)
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
                    .font(.system(size: 18, weight: .regular))
                    .foregroundStyle(Glass.secondaryText)
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
                            .font(.system(size: 15))
                            .foregroundStyle(Glass.secondaryText)
                    }
                    .buttonStyle(.plain)
                    .help("Clear search")
                    .accessibilityLabel("Clear search")
                    .accessibilityIdentifier("clearButton")
                }
                Rectangle().fill(.white.opacity(0.15)).frame(width: 1, height: 22)
                Menu {
                    Toggle("Case Sensitive", isOn: $searchOptions.isCaseSensitive)
                    Toggle("Regex", isOn: $searchOptions.isRegex)
                    Toggle("Whole Word", isOn: $searchOptions.isWholeWord)
                    Toggle("Match Filename", isOn: $searchOptions.isMatchFilename)
                } label: {
                    Image(systemName: "line.3.horizontal.decrease")
                        .font(.system(size: 15, weight: .medium))
                        .foregroundStyle(searchOptions.hasActiveOptions ? Glass.accent : Glass.secondaryText)
                        .frame(width: 28, height: 28)
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
                    Image(systemName: "questionmark")
                        .font(.system(size: 11, weight: .semibold))
                }
                .buttonStyle(GlassIconButtonStyle(size: 22))
                .help("Search syntax help (⇧⌘/)")
                .accessibilityLabel("Search syntax help")
            }
            .padding(.leading, 18)
            .padding(.trailing, 12)
            .frame(height: 54)
            .glassSurface(RoundedRectangle(cornerRadius: 15, style: .continuous), isFocused: isSearchFieldFocused)

            if searchOptions.hasActiveOptions {
                SearchOptionBadges(options: searchOptions)
            }
        }
        .padding(.horizontal, 18)
        .padding(.top, 6)
        .padding(.bottom, 16)
    }

    private var resultsHeader: some View {
        HStack(spacing: 8) {
            Label(viewModel.isContentSearch ? "File contents" : "Search results",
                  systemImage: viewModel.isContentSearch ? "doc.text.magnifyingglass" : "square.stack")
                .font(.system(size: 12, weight: .semibold))
                .labelStyle(GlassHeaderLabelStyle())
            if viewModel.totalMatches > 0 {
                Text("\(viewModel.totalMatches) matches")
                    .font(.system(size: 11))
                    .foregroundStyle(Glass.secondaryText)
                    .accessibilityIdentifier("matchCount")
            }
            Spacer()
            if !viewModel.isContentSearch {
                Text("SIZE")
                    .font(.system(size: 9, weight: .medium))
                    .tracking(1)
                    .foregroundStyle(Glass.tertiaryText)
                    .padding(.trailing, 6)
            }
        }
        .padding(.horizontal, 24)
        .frame(height: 38)
    }

    @ViewBuilder
    private var results: some View {
        if viewModel.isScanning {
            emptyState(symbol: "externaldrive", title: "Building your file index",
                       detail: "\(viewModel.scannedCount) items scanned", isLoading: true)
        } else if viewModel.searchText.isEmpty {
            emptyState(symbol: "magnifyingglass", title: "Find anything on your Mac",
                       detail: "Search by file name or path. Use infile: to search file contents.",
                       showsKeyHints: true)
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
                let keyword = viewModel.contentKeyword
                ScrollViewReader { proxy in
                    List(selection: $viewModel.selectedResultID) {
                        ForEach(viewModel.contentResults) { item in
                            ContentResultRow(item: item, keyword: keyword)
                                .listRowInsets(EdgeInsets())
                                .listRowSeparator(.hidden)
                                .id(item.id)
                        }
                    }
                    .listStyle(.plain)
                    .scrollContentBackground(.hidden)
                    .background(ListSelectionHighlightRemover())
                    .padding(.horizontal, 8)
                    .padding(.vertical, 6)
                    .modifier(ResultListKeys(proxy: proxy, isFocused: $isResultsFocused, onExitTop: focusSearchField))
                    .accessibilityIdentifier("contentResultsList")
                }
            }
        } else if viewModel.displayItems.isEmpty && !viewModel.searchText.isEmpty && viewModel.scanComplete {
            emptyState(symbol: "magnifyingglass", title: "No results found",
                       detail: "Try a different name, or broaden your search filters.")
                .accessibilityIdentifier("noResultsLabel")
        } else {
            let hints = viewModel.highlightHints
            ScrollViewReader { proxy in
                List(selection: $viewModel.selectedResultID) {
                    ForEach(viewModel.displayItems) { item in
                        ResultRow(item: item, hints: hints)
                            .listRowInsets(EdgeInsets())
                            .listRowSeparator(.hidden)
                            .id(item.id)
                    }
                    if viewModel.hasMoreResults {
                        HStack(spacing: 8) {
                            ProgressView().controlSize(.small)
                            Text("Loading more results…")
                                .font(.system(size: 11))
                                .foregroundStyle(Glass.secondaryText)
                        }
                        .padding(12)
                        .listRowSeparator(.hidden)
                        .onAppear { viewModel.loadMore() }
                    }
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
                .background(ListSelectionHighlightRemover())
                .padding(.horizontal, 8)
                .padding(.vertical, 6)
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

    private func emptyState(symbol: String, title: String, detail: String, isLoading: Bool = false,
                            showsKeyHints: Bool = false) -> some View {
        VStack(spacing: 8) {
            Group {
                if isLoading {
                    ProgressView().controlSize(.regular)
                } else {
                    Image(systemName: symbol)
                        .font(.system(size: 28, weight: .light))
                        .foregroundStyle(Glass.accent.opacity(0.9))
                }
            }
            .frame(width: 64, height: 64)
            .glassSurface(RoundedRectangle(cornerRadius: 20, style: .continuous))
            .padding(.bottom, 10)
            Text(title)
                .font(.system(size: 17, weight: .bold))
                .tracking(-0.3)
            Text(detail)
                .font(.system(size: 12))
                .foregroundStyle(Glass.secondaryText)
                .multilineTextAlignment(.center)
                .lineSpacing(3)
            if showsKeyHints {
                HStack(spacing: 4) {
                    GlassKeyCap(key: "↓")
                    Text("Browse results").padding(.trailing, 10)
                    GlassKeyCap(key: "Space")
                    Text("Quick Look")
                }
                .font(.system(size: 11))
                .foregroundStyle(Glass.tertiaryText)
                .padding(.top, 14)
            }
        }
        .padding(32)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// Label with a dimmed icon, for the results header.
private struct GlassHeaderLabelStyle: LabelStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 8) {
            configuration.icon.foregroundStyle(Glass.secondaryText)
            configuration.title
        }
    }
}

/// Turns off the result tables' native selection highlight so rows can draw their own
/// glass selection. Selection itself, keyboard navigation and the primary action still
/// come from the List.
private struct ListSelectionHighlightRemover: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView { Probe() }

    func updateNSView(_ nsView: NSView, context: Context) {
        (nsView as? Probe)?.removeHighlights()
    }

    private final class Probe: NSView {
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            removeHighlights()
        }

        /// Deferred a turn so the List's table is in the window by the time we look.
        func removeHighlights() {
            DispatchQueue.main.async { [weak self] in
                guard let root = self?.window?.contentView else { return }
                for table in Self.tables(in: root) where table.selectionHighlightStyle != .none {
                    table.selectionHighlightStyle = .none
                }
            }
        }

        private static func tables(in view: NSView) -> [NSTableView] {
            if let table = view as? NSTableView { return [table] }
            return view.subviews.flatMap(tables(in:))
        }
    }
}

/// Its own view so index, sync and timing updates redraw only this bar, not the results.
private struct ResultsStatusBar: View {
    private let viewModel = SearchViewModel.shared

    var body: some View {
        HStack(spacing: 7) {
            if viewModel.isScanning || viewModel.isSyncing {
                ProgressView().controlSize(.mini)
                Text(viewModel.isScanning ? "Scanning…" : "Syncing…")
            } else if viewModel.isMonitoring {
                Circle()
                    .fill(Glass.live)
                    .frame(width: 6, height: 6)
                    .shadow(color: Glass.live.opacity(0.6), radius: 4)
                Text("Live")
                    .fontWeight(.medium)
                    .foregroundStyle(Glass.text.opacity(0.85))
            }
            if viewModel.scanComplete {
                if viewModel.isMonitoring || viewModel.isSyncing { Text("·").opacity(0.5) }
                Text("\(viewModel.totalRecords.formatted()) files indexed")
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
        .font(.system(size: 10.5))
        .foregroundStyle(Glass.secondaryText)
        .padding(.horizontal, 20)
        .frame(height: 34)
        .accessibilityIdentifier("statusBar")
    }
}

/// Keyboard and mouse handling shared by both result lists: Space toggles Quick Look, Esc
/// closes it, ↑ on the first row returns to the search field, and Return or double-click
/// opens the selection (⌘ reveals it in Finder). The keys only fire while the list has
/// focus, so Space still types normally in the search field.
private struct ResultListKeys: ViewModifier {
    private let viewModel = SearchViewModel.shared
    let proxy: ScrollViewProxy
    var isFocused: FocusState<Bool>.Binding
    let onExitTop: () -> Void

    func body(content: Content) -> some View {
        content
            .contextMenu(forSelectionType: String.self) { ids in
                let targets = ids.compactMap { viewModel.resultTarget(for: $0) }
                if !targets.isEmpty {
                    Button("Open") {
                        for target in targets { FileActions.open(target.url, isApp: target.isApp) }
                    }
                    Button("Reveal in Finder") { FileActions.reveal(targets.map(\.url)) }
                    Divider()
                    Button(targets.count > 1 ? "Copy Paths" : "Copy Path") {
                        FileActions.copyPaths(targets.map(\.url))
                    }
                }
            } primaryAction: { ids in
                for target in ids.compactMap({ viewModel.resultTarget(for: $0) }) {
                    FileActions.performPrimaryAction(on: target.url, isApp: target.isApp)
                }
            }
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
