import SwiftUI

/// Refresh the List's AppKit viewport after result changes and popover reuse. This
/// doesn't reload its data source or recreate the List, so selection and paging survive.
struct ResultListPresentation: NSViewRepresentable {
    let query: String
    let revision: UInt64
    var hasSelection = false

    func makeNSView(context: Context) -> Probe { Probe() }

    func updateNSView(_ view: Probe, context: Context) {
        view.update(query: query, revision: revision, hasSelection: hasSelection)
    }

    final class Probe: NSView {
        private var query: String?
        private var revision: UInt64?
        private var resetScroll = false
        private var refreshScheduled = false
        private var windowObserver: NSObjectProtocol?

        deinit {
            if let windowObserver { NotificationCenter.default.removeObserver(windowObserver) }
        }

        func update(query: String, revision: UInt64, hasSelection: Bool) {
            guard self.query != query || self.revision != revision else { return }
            // A retained selection is positioned by ScrollViewReader after the row update.
            resetScroll = resetScroll || (self.query != query && !hasSelection)
            self.query = query
            self.revision = revision
            scheduleRefresh()
        }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let windowObserver { NotificationCenter.default.removeObserver(windowObserver) }
            windowObserver = nil
            guard let window else { return }
            windowObserver = NotificationCenter.default.addObserver(
                forName: NSWindow.didBecomeKeyNotification, object: window, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.scheduleRefresh() }
            }
            scheduleRefresh()
        }

        private func scheduleRefresh() {
            guard !refreshScheduled else { return }
            refreshScheduled = true
            // SwiftUI must commit the new rows before AppKit lays out the viewport.
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.refreshScheduled = false
                self.refresh()
            }
        }

        private func refresh() {
            guard let window else { return }
            window.contentView?.layoutSubtreeIfNeeded()
            guard let table = enclosingTable(), let scroll = table.enclosingScrollView else { return }
            table.selectionHighlightStyle = .none
            scroll.layoutSubtreeIfNeeded()
            table.layoutSubtreeIfNeeded()

            let clip = scroll.contentView
            var origin = clip.bounds.origin
            // An unselected new query starts at the first row; paging retains its offset.
            origin.y = resetScroll ? 0 : min(max(0, origin.y), max(0, table.bounds.height - clip.bounds.height))
            if origin != clip.bounds.origin {
                clip.scroll(to: origin)
                scroll.reflectScrolledClipView(clip)
            }
            resetScroll = false

            // A recycled hosting row can have correct data/selection but a stale layer.
            // Invalidate visible row contents as well as the table, without stealing focus.
            invalidateDisplay(clip)
        }

        private func invalidateDisplay(_ view: NSView) {
            view.needsDisplay = true
            for subview in view.subviews { invalidateDisplay(subview) }
        }

        private func enclosingTable() -> NSTableView? {
            var ancestor = superview
            while let view = ancestor {
                if let table = firstTable(in: view) { return table }
                ancestor = view.superview
            }
            return nil
        }

        private func firstTable(in view: NSView) -> NSTableView? {
            if let table = view as? NSTableView { return table }
            for child in view.subviews {
                if let table = firstTable(in: child) { return table }
            }
            return nil
        }
    }
}
