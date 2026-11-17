import Cocoa
import Quartz

/// Drives the shared Quick Look panel for the selected search result.
///
/// The panel looks for its controller along the key window's responder chain; that chain
/// ends at the app delegate, which forwards to `begin(_:)` / `end(_:)`. This lets the main
/// window and the menu bar popover share one controller without subclassing their windows.
@MainActor
final class QuickLookPreview: NSObject, QLPreviewPanelDataSource, QLPreviewPanelDelegate {
    static let shared = QuickLookPreview()

    private let viewModel = SearchViewModel.shared
    private var previewURL: URL?
    private var originalLevel: NSWindow.Level?
    /// Resolve the actual search surface rather than a key window that may be Quick Look.
    var searchWindow: (() -> NSWindow?)?
    /// Told when the preview opens (true) and closes (false), so the transient menu bar
    /// popover can stay open while the panel is in front of it.
    var onVisibilityChange: ((Bool) -> Void)?

    var isVisible: Bool {
        QLPreviewPanel.sharedPreviewPanelExists() && QLPreviewPanel.shared().isVisible
    }

    func toggle() {
        if isVisible {
            close()
        } else if viewModel.selectedResultURL != nil {
            // orderFront rather than makeKey: the result list keeps keyboard focus, so ↑/↓
            // keep moving the selection.
            onVisibilityChange?(true)
            let panel = QLPreviewPanel.shared()!
            let previousLevel = panel.level
            panel.orderFront(nil)
            // Quick Look configures its own level while ordering in. Apply ours after it.
            originalLevel = previousLevel
            if let window = searchWindow?() {
                panel.level = NSWindow.Level(rawValue: max(panel.level.rawValue, window.level.rawValue + 1))
            }
        } else {
            NSSound.beep()
        }
    }

    func close() {
        guard isVisible else { return }
        let panel = QLPreviewPanel.shared()!
        panel.orderOut(nil)
        restorePresentation(panel)
    }

    // MARK: - Panel control (forwarded from AppDelegate)

    func begin(_ panel: QLPreviewPanel) {
        previewURL = viewModel.selectedResultURL
        panel.dataSource = self
        panel.delegate = self
        viewModel.onSelectionChange = { [weak self] in self?.selectionChanged() }
    }

    func end(_ panel: QLPreviewPanel) {
        viewModel.onSelectionChange = nil
        previewURL = nil
        panel.dataSource = nil
        panel.delegate = nil
        restorePresentation(panel)
    }

    private func restorePresentation(_ panel: QLPreviewPanel) {
        if let originalLevel {
            panel.level = originalLevel
            self.originalLevel = nil
        }
        onVisibilityChange?(false)
    }

    nonisolated func windowWillClose(_ notification: Notification) {
        MainActor.assumeIsolated {
            guard let panel = notification.object as? QLPreviewPanel else { return }
            restorePresentation(panel)
        }
    }

    private func selectionChanged() {
        guard isVisible else { return }
        guard let url = viewModel.selectedResultURL else {
            close()
            return
        }
        guard url != previewURL else { return }
        previewURL = url
        QLPreviewPanel.shared().reloadData()
    }

    // MARK: - QLPreviewPanelDataSource

    nonisolated func numberOfPreviewItems(in panel: QLPreviewPanel!) -> Int {
        MainActor.assumeIsolated { previewURL == nil ? 0 : 1 }
    }

    nonisolated func previewPanel(_ panel: QLPreviewPanel!, previewItemAt index: Int) -> QLPreviewItem! {
        MainActor.assumeIsolated { previewURL as NSURL? }
    }

    // MARK: - QLPreviewPanelDelegate

    /// Keys typed while the panel itself is key (e.g. after clicking it).
    nonisolated func previewPanel(_ panel: QLPreviewPanel!, handle event: NSEvent!) -> Bool {
        guard event.type == .keyDown,
              event.modifierFlags.intersection([.command, .option, .control]).isEmpty else { return false }
        let keyCode = event.keyCode
        return MainActor.assumeIsolated {
            switch keyCode {
            case 49, 53: // Space, Esc
                close()
            case 125: // ↓
                viewModel.moveSelection(by: 1)
            case 126: // ↑
                viewModel.moveSelection(by: -1)
            default:
                return false
            }
            return true
        }
    }
}
