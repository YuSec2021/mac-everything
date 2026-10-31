import Cocoa
import SwiftUI
import Quartz
import ServiceManagement

/// MacEverything has no main window: it runs as a menu bar app (LSUIElement), and every
/// search happens in the panel that drops down from the status item.
class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate, NSPopoverDelegate, NSWindowDelegate {
    private var hotkeyManager: HotkeyManager?
    private var statusItem: NSStatusItem?
    private var statusMenu: NSMenu?
    private var searchPopover: NSPopover?
    /// Stands in for the popover when the status item has no place in the menu bar.
    private var fallbackPanel: NSPanel?
    private var appActiveBeforeHotkey: NSRunningApplication?
    private var launchAtLoginItem: NSMenuItem?
    private var mcpMenuItems: [MCPClient: NSMenuItem] = [:]

    func applicationDidFinishLaunching(_ notification: Notification) {
        MacSearchBridge.initializeLogger()

        MainActor.assumeIsolated {
            // The view model starts indexing and file monitoring; the main window used to
            // create it at launch, so create it here before any search UI is shown.
            _ = SearchViewModel.shared
            QuickLookPreview.shared.onVisibilityChange = { [weak self] visible in
                self?.searchPopover?.behavior = visible ? .applicationDefined : .transient
            }
        }
        hotkeyManager = HotkeyManager.shared
        hotkeyManager?.onPressed = { [weak self] in self?.handleHotkey() }
        hotkeyManager?.register()
        setupStatusBar()
    }

    /// Opening the app again from Finder, Spotlight or Launchpad shows the search panel.
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        showSearch()
        return false
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return false
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        DispatchQueue.global(qos: .userInitiated).async {
            MacSearchBridge.shared().prepareForTermination()
            DispatchQueue.main.async {
                NSApp.reply(toApplicationShouldTerminate: true)
            }
        }
        return .terminateLater
    }

    func applicationWillTerminate(_ notification: Notification) {
        // Safety net: shutdown is idempotent (compare_exchange_strong guard)
        MacSearchBridge.shared().prepareForTermination()
    }

    // MARK: - Status Bar

    private func setupStatusBar() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = statusItem?.button {
            button.image = NSImage(named: "MenuBarIcon")
            button.image?.size = NSSize(width: 18, height: 18)
            button.image?.isTemplate = true
            button.setAccessibilityLabel("Search with MacEverything")
            button.toolTip = "MacEverything — click to search, right-click for options"
            button.target = self
            button.action = #selector(statusItemClicked(_:))
            button.sendAction(on: [.leftMouseUp, .rightMouseUp])
        }

        let menu = NSMenu()
        menu.delegate = self
        menu.addItem(NSMenuItem(title: "Search…", action: #selector(showSearch), keyEquivalent: ""))
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Rebuild Index", action: #selector(rebuildIndex), keyEquivalent: ""))
        menu.addItem(NSMenuItem(title: "Shortcut Settings...", action: #selector(openShortcutSettings), keyEquivalent: ""))
        menu.addItem(NSMenuItem(title: "Content Settings...", action: #selector(openContentSettings), keyEquivalent: ""))
        menu.addItem(NSMenuItem(title: "Search Syntax Help...", action: #selector(openSearchSyntaxHelp), keyEquivalent: ""))

        let mcpSubmenu = NSMenu(title: "MCP Integration")
        for client in MCPClient.allCases {
            let item = NSMenuItem(title: client.displayName, action: #selector(toggleMCPClient(_:)), keyEquivalent: "")
            item.representedObject = client
            mcpMenuItems[client] = item
            mcpSubmenu.addItem(item)
        }
        let mcpItem = NSMenuItem(title: "MCP Integration", action: nil, keyEquivalent: "")
        mcpItem.submenu = mcpSubmenu
        menu.addItem(mcpItem)

        menu.addItem(.separator())
        let loginItem = NSMenuItem(title: "Launch at Login", action: #selector(toggleLaunchAtLogin), keyEquivalent: "")
        launchAtLoginItem = loginItem
        menu.addItem(loginItem)
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "About MacEverything", action: #selector(openAbout), keyEquivalent: ""))
        menu.addItem(NSMenuItem(title: "Quit MacEverything", action: #selector(quitApp), keyEquivalent: "q"))
        statusMenu = menu
    }

    // MARK: - Search Panel

    /// The window of whichever search surface is on screen, if any.
    private var visibleSearchWindow: NSWindow? {
        if searchPopover?.isShown == true { return searchPopover?.contentViewController?.view.window }
        if let panel = fallbackPanel, panel.isVisible { return panel }
        return nil
    }

    /// Shows the search panel, or brings it to the front if it is already open.
    @objc func showSearch() {
        if let window = visibleSearchWindow {
            NSApp.activate(ignoringOtherApps: true)
            window.makeKey()
            focusSearchField(in: window)
            return
        }
        // A status item hidden by the notch or a menu bar manager has nothing to anchor to.
        if let button = statusItem?.button, statusItem?.isVisible == true, button.window?.screen != nil {
            showSearchPopover(relativeTo: button)
            focusSearchField(in: searchPopover?.contentViewController?.view.window)
        } else {
            showFallbackPanel()
        }
    }

    private func closeSearch() {
        MainActor.assumeIsolated { QuickLookPreview.shared.close() }
        searchPopover?.performClose(nil)
        fallbackPanel?.close()
    }

    // MARK: - Global Hotkey

    /// Like Spotlight, the hotkey opens the search panel over whatever app is in front,
    /// and a second press closes it and hands focus back to that app.
    @MainActor
    private func handleHotkey() {
        if let window = visibleSearchWindow {
            if NSApp.isActive {
                closeSearch()
                if let previous = appActiveBeforeHotkey, !previous.isTerminated {
                    NSApp.yieldActivation(to: previous)
                    previous.activate()
                }
                appActiveBeforeHotkey = nil
            } else {
                NSApp.activate(ignoringOtherApps: true)
                window.makeKey()
                focusSearchField(in: window)
            }
            return
        }
        if let front = NSWorkspace.shared.frontmostApplication, front != NSRunningApplication.current {
            appActiveBeforeHotkey = front
        } else {
            appActiveBeforeHotkey = nil
        }
        showSearch()
    }

    /// Puts the cursor in the search field with the previous query selected, so typing replaces it.
    private func focusSearchField(in window: NSWindow?) {
        DispatchQueue.main.async {
            guard let window, let field = window.contentView?.firstDescendant(of: HighlightedNSTextView.self) else { return }
            window.makeFirstResponder(field)
            field.selectAll(nil)
        }
    }

    @objc private func statusItemClicked(_ sender: NSStatusBarButton) {
        if NSApp.currentEvent?.type == .rightMouseUp {
            closeSearch()
            statusMenu?.popUp(positioning: nil, at: NSPoint(x: 0, y: sender.bounds.minY - 4), in: sender)
            return
        }
        if searchPopover?.isShown == true {
            searchPopover?.performClose(nil)
            return
        }
        fallbackPanel?.close()
        showSearchPopover(relativeTo: sender)
    }

    private func makeSearchView() -> NSHostingController<MenuBarSearchView> {
        NSHostingController(rootView: MenuBarSearchView(actions: self, close: { [weak self] in self?.closeSearch() }))
    }

    private func showSearchPopover(relativeTo sender: NSStatusBarButton) {
        if searchPopover == nil {
            let popover = NSPopover()
            popover.behavior = .transient
            popover.animates = false
            popover.delegate = self
            popover.contentSize = NSSize(width: 640, height: 500)
            popover.contentViewController = makeSearchView()
            searchPopover = popover
        }
        NSApp.activate(ignoringOtherApps: true)
        searchPopover?.show(relativeTo: sender.bounds, of: sender, preferredEdge: .minY)
        searchPopover?.contentViewController?.view.window?.makeKey()
        Task { @MainActor in SearchViewModel.shared.onWindowFocusChanged(true) }
    }

    /// The same search view in a floating panel at the top of the screen, for when the
    /// status item is hidden and the popover has nowhere to drop down from.
    private func showFallbackPanel() {
        if fallbackPanel == nil {
            let panel = NSPanel(contentViewController: makeSearchView())
            panel.styleMask = [.titled, .closable, .fullSizeContentView]
            panel.titleVisibility = .hidden
            panel.titlebarAppearsTransparent = true
            panel.level = .floating
            panel.hidesOnDeactivate = true
            panel.isReleasedWhenClosed = false
            panel.delegate = self
            fallbackPanel = panel
        }
        guard let panel = fallbackPanel else { return }
        if let screen = NSScreen.main {
            let frame = screen.visibleFrame
            panel.setFrameTopLeftPoint(NSPoint(x: frame.midX - panel.frame.width / 2, y: frame.maxY - 80))
        }
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
        focusSearchField(in: panel)
        Task { @MainActor in SearchViewModel.shared.onWindowFocusChanged(true) }
    }

    func popoverWillClose(_ notification: Notification) {
        MainActor.assumeIsolated { QuickLookPreview.shared.close() }
    }

    func popoverDidClose(_ notification: Notification) {
        Task { @MainActor in SearchViewModel.shared.onWindowFocusChanged(false) }
    }

    func windowWillClose(_ notification: Notification) {
        guard (notification.object as? NSWindow) === fallbackPanel else { return }
        MainActor.assumeIsolated {
            QuickLookPreview.shared.close()
            SearchViewModel.shared.onWindowFocusChanged(false)
        }
    }

    // MARK: - Quick Look

    // The preview panel searches the key window's responder chain for a controller; the
    // app delegate is the last stop, so every search surface reaches it.
    override func acceptsPreviewPanelControl(_ panel: QLPreviewPanel!) -> Bool { true }

    override func beginPreviewPanelControl(_ panel: QLPreviewPanel!) {
        MainActor.assumeIsolated { QuickLookPreview.shared.begin(panel) }
    }

    override func endPreviewPanelControl(_ panel: QLPreviewPanel!) {
        MainActor.assumeIsolated { QuickLookPreview.shared.end(panel) }
    }

    // MARK: - NSMenuDelegate

    func menuWillOpen(_ menu: NSMenu) {
        launchAtLoginItem?.state = isLaunchAtLoginEnabled ? .on : .off
        for (client, item) in mcpMenuItems {
            item.state = MCPConfigManager.isEnabled(for: client) ? .on : .off
        }
    }

    // MARK: - Menu Actions

    // Shared by the status item's menu, the search panel's gear menu and the app's key
    // equivalents.

    /// Opens the search panel too, so the scan progress is visible.
    @objc func rebuildIndex() {
        showSearch()
        MainActor.assumeIsolated { SearchViewModel.shared.rebuildIndex() }
    }

    @objc func openShortcutSettings() {
        closeSearch()
        NSApp.activate(ignoringOtherApps: true)
        ShortcutSettingsWindowController.shared.showWindow()
    }

    @objc func openContentSettings() {
        closeSearch()
        NSApp.activate(ignoringOtherApps: true)
        ContentSettingsWindowController.shared.showWindow()
    }

    @objc func openAbout() {
        closeSearch()
        AboutWindowController.shared.showWindow()
    }

    @objc func openSearchSyntaxHelp() {
        NSApp.activate(ignoringOtherApps: true)
        SearchSyntaxHelpWindowController.shared.showWindow()
    }

    @objc private func toggleMCPClient(_ sender: NSMenuItem) {
        guard let client = sender.representedObject as? MCPClient else { return }
        let currentlyEnabled = MCPConfigManager.isEnabled(for: client)
        MCPConfigManager.setEnabled(!currentlyEnabled, for: client)
        sender.state = !currentlyEnabled ? .on : .off
    }

    var isLaunchAtLoginEnabled: Bool { SMAppService.mainApp.status == .enabled }

    @objc func toggleLaunchAtLogin() {
        do {
            if SMAppService.mainApp.status == .enabled {
                try SMAppService.mainApp.unregister()
            } else {
                try SMAppService.mainApp.register()
            }
        } catch {
            AppLogger.error("App", "Failed to toggle launch at login: \(error)")
        }
    }

    @objc func quitApp() {
        NSApp.terminate(nil)
    }
}

private extension NSView {
    func firstDescendant<T: NSView>(of type: T.Type) -> T? {
        for subview in subviews {
            if let match = subview as? T ?? subview.firstDescendant(of: type) { return match }
        }
        return nil
    }
}

private struct MenuBarSearchView: View {
    let actions: AppDelegate
    let close: () -> Void
    @State private var launchAtLogin = false

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("MacEverything").font(.system(size: 12, weight: .semibold))
                Spacer()
                Menu {
                    Button("Rebuild Index") { actions.rebuildIndex() }
                    Divider()
                    Button("Shortcut Settings…") { actions.openShortcutSettings() }
                    Button("Content Settings…") { actions.openContentSettings() }
                    Menu("MCP Integration") {
                        ForEach(MCPClient.allCases, id: \.self) { client in
                            Toggle(client.displayName, isOn: Binding(
                                get: { MCPConfigManager.isEnabled(for: client) },
                                set: { MCPConfigManager.setEnabled($0, for: client) }
                            ))
                        }
                    }
                    Toggle("Launch at Login", isOn: Binding(
                        get: { launchAtLogin },
                        set: { _ in
                            actions.toggleLaunchAtLogin()
                            launchAtLogin = actions.isLaunchAtLoginEnabled
                        }
                    ))
                    Divider()
                    Button("About MacEverything") { actions.openAbout() }
                    Button("Quit MacEverything") { actions.quitApp() }
                } label: {
                    Image(systemName: "gearshape")
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
                .help("Settings and more")
                .accessibilityLabel("Settings and more")
                .accessibilityIdentifier("panelSettingsMenu")
                Button(action: close) { Image(systemName: "xmark") }
                    .help("Close search")
                    .accessibilityLabel("Close search")
            }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
            .padding(.horizontal, 18)
            .padding(.top, 14)
            ContentView()
        }
        .frame(width: 640, height: 500)
        .background(Color(nsColor: .windowBackgroundColor))
        .onExitCommand(perform: close)
        .onAppear { launchAtLogin = actions.isLaunchAtLoginEnabled }
    }
}
