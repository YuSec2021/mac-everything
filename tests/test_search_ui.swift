// Standalone AppKit/SwiftUI regression tests. Run with `make test-search-ui`.
import SwiftUI
import Quartz

private final class PreviewTestDelegate: NSObject, NSApplicationDelegate {
    override func acceptsPreviewPanelControl(_ panel: QLPreviewPanel!) -> Bool { true }
    override func beginPreviewPanelControl(_ panel: QLPreviewPanel!) {
        MainActor.assumeIsolated { QuickLookPreview.shared.begin(panel) }
    }
    override func endPreviewPanelControl(_ panel: QLPreviewPanel!) {
        MainActor.assumeIsolated { QuickLookPreview.shared.end(panel) }
    }
}

// QuickLookPreview's test model avoids starting the real filesystem index.
@MainActor
final class SearchViewModel {
    static let shared = SearchViewModel()
    var selectedResultURL: URL?
    var onSelectionChange: (() -> Void)?
    func moveSelection(by delta: Int) {}
}

@Observable @MainActor
final class FieldState {
    var text = ""
    var suggestion: String?
}

private struct FieldHost: View {
    @Bindable var state: FieldState
    @FocusState private var focused: Bool

    var body: some View {
        HighlightedSearchField(text: $state.text, ghostSuggestion: state.suggestion,
                               isFocused: $focused, onTab: {
            guard let suggestion = state.suggestion else { return false }
            state.text = suggestion
            state.suggestion = nil
            return true
        })
        .frame(width: 500, height: 30)
    }
}

@Observable @MainActor
final class ListState {
    var query = "a"
    var revision: UInt64 = 0
    var items = Array(0..<300)
    var selection: Int?
}

private struct ListHost: View {
    @Bindable var state: ListState
    var body: some View {
        List(selection: $state.selection) {
            ForEach(state.items, id: \.self) { item in
                Text("Result \(item)").frame(height: 40).tag(item)
            }
        }
        .listStyle(.plain)
        .background(ResultListPresentation(query: state.query, revision: state.revision,
                                           hasSelection: state.selection != nil))
        .frame(width: 500, height: 300)
    }
}

@main @MainActor
struct SearchUITests {
    static var failures = 0

    static func check(_ condition: @autoclosure () -> Bool, _ message: String) {
        if condition() { print("PASS: \(message)") }
        else { failures += 1; print("FAIL: \(message)") }
        fflush(stdout)
    }

    static func settle() {
        RunLoop.main.run(until: Date().addingTimeInterval(0.15))
    }

    static func waitUntil(_ condition: () -> Bool) {
        let deadline = Date().addingTimeInterval(3)
        while !condition() && Date() < deadline { settle() }
    }

    static func find<T: NSView>(_ type: T.Type, in view: NSView) -> T? {
        if let match = view as? T { return match }
        return view.subviews.lazy.compactMap { find(type, in: $0) }.first
    }

    static func makeWindow<V: View>(_ view: V) -> NSWindow {
        let window = NSWindow(contentViewController: NSHostingController(rootView: view))
        window.isReleasedWhenClosed = false
        window.orderFront(nil)
        settle()
        return window
    }

    static func testCompletion() {
        let state = FieldState()
        let window = makeWindow(FieldHost(state: state))
        defer { window.close() }
        let field = find(HighlightedNSTextView.self, in: window.contentView!)!
        let coordinator = field.delegate as! HighlightedSearchField.Coordinator

        for (prefix, suggestion) in [("fil", "file:"), ("报告", "报告📁最终版"), ("a", String(repeating: "abc", count: 120))] {
            state.text = prefix
            state.suggestion = suggestion
            settle()
            field.setSelectedRange(NSRange(location: prefix.utf16.count, length: 0))
            check(coordinator.handleTab(), "completion consumes Tab: \(prefix)")
            // Deliberately don't yield: a following key must use the new insertion point.
            check(field.selectedRange() == NSRange(location: suggestion.utf16.count, length: 0), "caret reaches UTF-16 end: \(prefix)")
            field.insertText("X", replacementRange: field.selectedRange())
            check(state.text == suggestion + "X", "immediate typing follows completion: \(prefix)")
        }

        state.suggestion = nil
        settle()
        let original = field.string
        check(!coordinator.handleTab() && field.string == original, "Tab without suggestion leaves text alone")

        state.text = "abcdef"
        settle()
        field.setSelectedRange(NSRange(location: 2, length: 2))
        coordinator.applyHighlighting(field)
        check(field.selectedRange() == NSRange(location: 2, length: 2), "highlighting preserves selection")
        state.text = ""
        settle()
        check(field.selectedRange() == NSRange(location: 0, length: 0), "external clear clamps old selection")

        state.text = "fil"
        state.suggestion = "file:"
        settle()
        field.undoManager?.removeAllActions()
        _ = coordinator.handleTab()
        settle()
        check(field.undoManager?.canUndo == true, "completion participates in native undo")
        field.undoManager?.undo()
        settle()
        check(state.text == "fil", "undo restores the prefix and its binding")

        field.setMarkedText("拼", selectedRange: NSRange(location: 1, length: 0),
                            replacementRange: NSRange(location: 0, length: field.string.utf16.count))
        state.suggestion = "拼音"
        check(field.hasMarkedText() && !coordinator.handleTab(), "IME composition does not accept completion")
        field.unmarkText()
    }

    static func testResultViewport() {
        let state = ListState()
        let window = makeWindow(ListHost(state: state))
        defer { window.close() }
        let table = find(NSTableView.self, in: window.contentView!)!
        let clip = table.enclosingScrollView!.contentView
        check(table.numberOfRows == 300 && table.rows(in: table.visibleRect).length > 0,
              "results have visible rows without keyboard focus")

        table.scrollRowToVisible(150)
        settle()
        let pagedOffset = clip.bounds.origin.y
        check(pagedOffset > 0, "fixture scrolls beyond first page")
        state.selection = 150
        state.items += Array(300..<400)
        state.revision += 1
        settle()
        check(abs(clip.bounds.origin.y - pagedOffset) < 1, "pagination preserves scroll position")
        check(state.selection == 150 && table.selectedRow >= 0, "pagination preserves selection")

        state.query = "retained selection"
        state.revision += 1
        settle()
        check(abs(clip.bounds.origin.y - pagedOffset) < 1 && state.selection == 150,
              "new query with retained selection does not reset its viewport")

        state.query = "b"
        state.items = Array(1000..<1400) // same row count; count alone can't detect this update.
        state.selection = nil
        state.revision += 1
        settle()
        check(clip.bounds.origin.y == 0, "same-sized new query returns viewport to first row")
        check(table.rows(in: table.visibleRect).location == 0, "new query's first rows are visible")

        state.items = [2000, 2001]
        state.query = "small"
        state.revision += 1
        settle()
        window.orderOut(nil)
        window.makeKeyAndOrderFront(nil)
        settle()
        check(table.numberOfRows == 2 && table.rows(in: table.visibleRect).length == 2,
              "reopened search shows short results without scrolling")
    }

    static func testPreviewLevel() {
        let source = makeWindow(Text("Search surface"))
        defer { source.close() }
        source.level = .popUpMenu
        let controller = QuickLookPreview.shared
        controller.searchWindow = { source }
        SearchViewModel.shared.selectedResultURL = URL(fileURLWithPath: #filePath)
        let panel = QLPreviewPanel.shared()!
        let originalLevel = panel.level
        var visible = false
        controller.onVisibilityChange = { visible = $0 }
        controller.toggle()
        settle()
        check(panel.level.rawValue > source.level.rawValue, "preview appears above popover-level window")
        controller.close()
        check(panel.level == originalLevel && !visible, "closing preview restores level and transient behavior")
        settle()

        source.level = .floating
        source.makeKeyAndOrderFront(nil)
        controller.toggle()
        settle()
        check(panel.level.rawValue > source.level.rawValue, "preview appears above fallback floating panel")
        panel.close()
        waitUntil { !panel.isVisible }
        check(!panel.isVisible && panel.level == originalLevel && !visible,
              "native close restores presentation too (visible=\(panel.isVisible), callback=\(visible), level=\(panel.level.rawValue), original=\(originalLevel.rawValue))")
        controller.close()
    }

    static func main() {
        _ = NSApplication.shared
        let delegate = PreviewTestDelegate()
        NSApp.delegate = delegate
        NSApp.setActivationPolicy(.accessory)
        testCompletion()
        testResultViewport()
        testPreviewLevel()
        withExtendedLifetime(delegate) {}
        print("\(failures) failure(s)")
        exit(failures == 0 ? 0 : 1)
    }
}
