import SwiftUI

@main
struct MacEverythingApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @ObservedObject private var searchOptions = SearchOptions.shared

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

class ContentSettingsWindowController {
    static let shared = ContentSettingsWindowController()
    private var window: NSWindow?

    func showWindow() {
        if let existing = window, existing.isVisible {
            existing.makeKeyAndOrderFront(nil)
            return
        }

        let settingsView = ContentSettingsView()
        let hostingController = NSHostingController(rootView: settingsView)
        let win = NSWindow(contentViewController: hostingController)
        win.title = "Content Settings"
        win.styleMask = [.titled, .closable]
        win.center()
        win.makeKeyAndOrderFront(nil)
        window = win
    }
}

class ShortcutSettingsWindowController {
    static let shared = ShortcutSettingsWindowController()
    private var window: NSWindow?

    func showWindow() {
        if let existing = window, existing.isVisible {
            existing.makeKeyAndOrderFront(nil)
            return
        }

        let settingsView = ShortcutSettingsView()
        let hostingController = NSHostingController(rootView: settingsView)
        let win = NSWindow(contentViewController: hostingController)
        win.title = "Shortcut Settings"
        win.styleMask = [.titled, .closable]
        win.center()
        win.makeKeyAndOrderFront(nil)
        window = win
    }
}

class SearchSyntaxHelpWindowController {
    static let shared = SearchSyntaxHelpWindowController()
    private var window: NSWindow?

    func showWindow() {
        if let existing = window, existing.isVisible {
            existing.makeKeyAndOrderFront(nil)
            return
        }

        let helpView = SearchSyntaxHelpView()
        let hostingController = NSHostingController(rootView: helpView)
        let win = NSWindow(contentViewController: hostingController)
        win.title = "Search Syntax Help"
        win.styleMask = [.titled, .closable, .resizable]
        win.setContentSize(NSSize(width: 580, height: 720))
        win.minSize = NSSize(width: 450, height: 400)
        win.center()
        win.makeKeyAndOrderFront(nil)
        window = win
    }
}
