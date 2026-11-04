import SwiftUI

@main
struct MacEverythingApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @Bindable private var searchOptions = SearchOptions.shared

    /// MacEverything lives in the menu bar (LSUIElement), so it has no window scene. The
    /// Settings scene is only a host for the menu commands below: their key equivalents
    /// still fire while the search panel or a settings window is key, even though the
    /// menu bar itself isn't shown. `.appSettings` is replaced, so it never opens.
    var body: some Scene {
        Settings { EmptyView() }
        .commands {
            CommandGroup(replacing: .appInfo) {
                Button("About MacEverything") {
                    AboutWindowController.shared.showWindow()
                }
            }

            CommandMenu("Search") {
                Toggle("Regex", isOn: $searchOptions.isRegex)
                    .keyboardShortcut("r", modifiers: [.command])
                Toggle("Case Sensitive", isOn: $searchOptions.isCaseSensitive)
                    .keyboardShortcut("c", modifiers: [.command, .shift])
                Toggle("Whole Word", isOn: $searchOptions.isWholeWord)
                    .keyboardShortcut("w", modifiers: [.command, .shift])
                Toggle("Match Filename", isOn: $searchOptions.isMatchFilename)
                    .keyboardShortcut("f", modifiers: [.command, .shift])
            }

            CommandGroup(replacing: .help) {
                Button("Search Syntax Help") {
                    SearchSyntaxHelpWindowController.shared.showWindow()
                }
                .keyboardShortcut("/", modifiers: [.command, .shift])
            }

            CommandGroup(replacing: .appSettings) {
                Button("Rebuild Index") { appDelegate.rebuildIndex() }
                .keyboardShortcut("r", modifiers: [.command, .shift])

                Button("Shortcut Settings...") {
                    ShortcutSettingsWindowController.shared.showWindow()
                }
                .keyboardShortcut(",", modifiers: [.command])

                Button("Content Settings...") {
                    ContentSettingsWindowController.shared.showWindow()
                }

                Divider()

                Menu("MCP Integration") {
                    ForEach(MCPClient.allCases, id: \.self) { client in
                        Toggle(client.displayName, isOn: Binding(
                            get: { MCPConfigManager.isEnabled(for: client) },
                            set: { MCPConfigManager.setEnabled($0, for: client) }
                        ))
                    }
                }
            }
        }
    }
}

/// Shows a SwiftUI view in a standalone window, bringing it forward if it is already open.
/// A closed window is replaced on the next show, so the view starts from fresh state.
final class HostingWindowController<Content: View> {
    private let title: String
    private let styleMask: NSWindow.StyleMask
    private let contentSize: NSSize?
    private let minSize: NSSize?
    private let makeContent: () -> Content
    private var window: NSWindow?

    init(title: String, styleMask: NSWindow.StyleMask = [.titled, .closable],
         contentSize: NSSize? = nil, minSize: NSSize? = nil,
         content: @escaping () -> Content) {
        self.title = title
        self.styleMask = styleMask
        self.contentSize = contentSize
        self.minSize = minSize
        self.makeContent = content
    }

    func showWindow() {
        if let existing = window, existing.isVisible {
            existing.makeKeyAndOrderFront(nil)
            return
        }

        let win = NSWindow(contentViewController: NSHostingController(rootView: makeContent()))
        win.title = title
        win.styleMask = styleMask
        // `window` holds the only reference; AppKit must not also release it on close.
        win.isReleasedWhenClosed = false
        if let contentSize { win.setContentSize(contentSize) }
        if let minSize { win.minSize = minSize }
        win.center()
        win.makeKeyAndOrderFront(nil)
        window = win
    }
}

enum ContentSettingsWindowController {
    static let shared = HostingWindowController(title: "Content Settings") { ContentSettingsView() }
}

enum ShortcutSettingsWindowController {
    static let shared = HostingWindowController(title: "Shortcut Settings") { ShortcutSettingsView() }
}

enum SearchSyntaxHelpWindowController {
    static let shared = HostingWindowController(
        title: "Search Syntax Help",
        styleMask: [.titled, .closable, .resizable],
        contentSize: NSSize(width: 580, height: 720),
        minSize: NSSize(width: 450, height: 400)
    ) { SearchSyntaxHelpView() }
}
