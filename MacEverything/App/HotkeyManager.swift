import Cocoa
import Carbon.HIToolbox

/// A global key combination in Carbon terms (virtual key code + Carbon modifier mask).
struct Hotkey: Equatable {
    var keyCode: UInt32
    var modifiers: UInt32

    static let defaultHotkey = Hotkey(keyCode: UInt32(kVK_Space), modifiers: UInt32(optionKey))

    var displayString: String { describeShortcut(keyCode: keyCode, modifiers: modifiers) }

    init(keyCode: UInt32, modifiers: UInt32) {
        self.keyCode = keyCode
        self.modifiers = modifiers
    }

    init(event: NSEvent) {
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        var mods: UInt32 = 0
        if flags.contains(.command) { mods |= UInt32(cmdKey) }
        if flags.contains(.option) { mods |= UInt32(optionKey) }
        if flags.contains(.control) { mods |= UInt32(controlKey) }
        if flags.contains(.shift) { mods |= UInt32(shiftKey) }
        self.init(keyCode: UInt32(event.keyCode), modifiers: mods)
    }

    var isFunctionKey: Bool { Self.functionKeyCodes.contains(Int(keyCode)) }

    var eventModifierFlags: NSEvent.ModifierFlags {
        var flags: NSEvent.ModifierFlags = []
        if modifiers & UInt32(cmdKey) != 0 { flags.insert(.command) }
        if modifiers & UInt32(optionKey) != 0 { flags.insert(.option) }
        if modifiers & UInt32(controlKey) != 0 { flags.insert(.control) }
        if modifiers & UInt32(shiftKey) != 0 { flags.insert(.shift) }
        return flags
    }

    static let functionKeyCodes: Set<Int> = [
        kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10,
        kVK_F11, kVK_F12, kVK_F13, kVK_F14, kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20,
    ]
}

/// Persisted hotkey settings. Keys are unchanged from the original implementation so
/// existing user settings carry over; a stored key code of -1 means "no hotkey".
enum HotkeySettings {
    private static let keyCodeKey = "hotkeyKeyCode"
    private static let modifiersKey = "hotkeyModifiers"

    static var hotkey: Hotkey? {
        get {
            let defaults = UserDefaults.standard
            guard defaults.object(forKey: keyCodeKey) != nil else { return .defaultHotkey }
            let code = defaults.integer(forKey: keyCodeKey)
            guard code >= 0 else { return nil }
            return Hotkey(keyCode: UInt32(code), modifiers: UInt32(defaults.integer(forKey: modifiersKey)))
        }
        set {
            let defaults = UserDefaults.standard
            defaults.set(newValue.map { Int($0.keyCode) } ?? -1, forKey: keyCodeKey)
            defaults.set(newValue.map { Int($0.modifiers) } ?? 0, forKey: modifiersKey)
        }
    }
}

/// Why a key combination can't, or shouldn't, be used as the global hotkey.
struct HotkeyConflict {
    enum Severity { case blocking, warning }
    let severity: Severity
    let message: String
}

enum HotkeyConflictChecker {
    /// Checks a candidate against rules, enabled system shortcuts, this app's own
    /// menu shortcuts and a list of shortcuts most apps use. Registration failures
    /// are reported separately by `HotkeyManager.apply(_:)`.
    static func check(_ hotkey: Hotkey) -> HotkeyConflict? {
        let mods = hotkey.modifiers
        let cmdOptCtrl = UInt32(cmdKey | optionKey | controlKey)

        if mods == 0 && !hotkey.isFunctionKey {
            return HotkeyConflict(severity: .blocking,
                                  message: "Add at least one modifier (⌘ ⌥ ⌃ ⇧).")
        }
        if mods & cmdOptCtrl == 0 && !hotkey.isFunctionKey {
            return HotkeyConflict(severity: .blocking,
                                  message: "\(hotkey.displayString) would block typing. Add ⌘, ⌥ or ⌃.")
        }
        if let name = systemShortcutName(for: hotkey) {
            return HotkeyConflict(severity: .blocking,
                                  message: "\(hotkey.displayString) is used by macOS (\(name)). Choose another, or turn it off in System Settings › Keyboard › Keyboard Shortcuts.")
        }
        if let title = menuItemTitle(for: hotkey, in: NSApp.mainMenu) {
            return HotkeyConflict(severity: .blocking,
                                  message: "\(hotkey.displayString) is already used by MacEverything's “\(title)” menu item.")
        }
        if let name = commonShortcuts[hotkey] {
            return HotkeyConflict(severity: .warning,
                                  message: "\(hotkey.displayString) is commonly used for “\(name)” in other apps; it will stop working there.")
        }
        if mods & cmdOptCtrl == UInt32(optionKey) && !hotkey.isFunctionKey && hotkey.keyCode != UInt32(kVK_Space) {
            return HotkeyConflict(severity: .warning,
                                  message: "\(hotkey.displayString) is normally used to type special characters.")
        }
        return nil
    }

    // MARK: - System shortcuts

    private static func systemShortcutName(for hotkey: Hotkey) -> String? {
        var unmanaged: Unmanaged<CFArray>?
        guard CopySymbolicHotKeys(&unmanaged) == noErr,
              let entries = unmanaged?.takeRetainedValue() as? [[String: Any]] else { return nil }
        for entry in entries {
            guard (entry[kHISymbolicHotKeyEnabled as String] as? Bool) == true,
                  let code = (entry[kHISymbolicHotKeyCode as String] as? NSNumber)?.uint32Value,
                  let mods = (entry[kHISymbolicHotKeyModifiers as String] as? NSNumber)?.uint32Value else { continue }
            // Symbolic hotkey modifiers can carry extra bits (e.g. function key flag).
            let relevant = UInt32(cmdKey | optionKey | controlKey | shiftKey)
            if code == hotkey.keyCode && mods & relevant == hotkey.modifiers {
                return knownSystemShortcuts[hotkey] ?? "a system shortcut"
            }
        }
        return nil
    }

    /// Friendly names for the default system shortcuts most likely to collide.
    private static let knownSystemShortcuts: [Hotkey: String] = [
        Hotkey(keyCode: UInt32(kVK_Space), modifiers: UInt32(cmdKey)): "Spotlight",
        Hotkey(keyCode: UInt32(kVK_Space), modifiers: UInt32(cmdKey | optionKey)): "Finder search window",
        Hotkey(keyCode: UInt32(kVK_Space), modifiers: UInt32(controlKey)): "Select previous input source",
        Hotkey(keyCode: UInt32(kVK_Space), modifiers: UInt32(controlKey | optionKey)): "Select next input source",
        Hotkey(keyCode: UInt32(kVK_Space), modifiers: UInt32(controlKey | cmdKey)): "Emoji & Symbols",
        Hotkey(keyCode: UInt32(kVK_ANSI_3), modifiers: UInt32(cmdKey | shiftKey)): "Screenshot",
        Hotkey(keyCode: UInt32(kVK_ANSI_4), modifiers: UInt32(cmdKey | shiftKey)): "Screenshot selection",
        Hotkey(keyCode: UInt32(kVK_ANSI_5), modifiers: UInt32(cmdKey | shiftKey)): "Screenshot toolbar",
        Hotkey(keyCode: UInt32(kVK_UpArrow), modifiers: UInt32(controlKey)): "Mission Control",
        Hotkey(keyCode: UInt32(kVK_DownArrow), modifiers: UInt32(controlKey)): "Application windows",
    ]

    // MARK: - App menu shortcuts

    private static func menuItemTitle(for hotkey: Hotkey, in menu: NSMenu?) -> String? {
        let key = hotkey.keyCode == UInt32(kVK_Space) ? " " : printableKeyName(hotkey.keyCode)?.lowercased()
        guard let key else { return nil }
        return menuItemTitle(matching: key, modifiers: hotkey.eventModifierFlags, in: menu)
    }

    private static func menuItemTitle(matching key: String, modifiers: NSEvent.ModifierFlags, in menu: NSMenu?) -> String? {
        guard let menu else { return nil }
        for item in menu.items {
            if let title = menuItemTitle(matching: key, modifiers: modifiers, in: item.submenu) { return title }
            var itemModifiers = item.keyEquivalentModifierMask.intersection(.deviceIndependentFlagsMask)
            // An uppercase key equivalent ("A") implies Shift.
            if item.keyEquivalent != item.keyEquivalent.lowercased() { itemModifiers.insert(.shift) }
            if item.keyEquivalent.lowercased() == key, itemModifiers == modifiers {
                return item.title
            }
        }
        return nil
    }

    // MARK: - Common app shortcuts

    private static let commonShortcuts: [Hotkey: String] = {
        let cmd = UInt32(cmdKey)
        let pairs: [(Int, UInt32, String)] = [
            (kVK_ANSI_Q, cmd, "Quit"), (kVK_ANSI_W, cmd, "Close"), (kVK_ANSI_H, cmd, "Hide"),
            (kVK_ANSI_M, cmd, "Minimize"), (kVK_ANSI_N, cmd, "New"), (kVK_ANSI_O, cmd, "Open"),
            (kVK_ANSI_S, cmd, "Save"), (kVK_ANSI_P, cmd, "Print"), (kVK_ANSI_T, cmd, "New Tab"),
            (kVK_ANSI_F, cmd, "Find"), (kVK_ANSI_A, cmd, "Select All"), (kVK_ANSI_C, cmd, "Copy"),
            (kVK_ANSI_V, cmd, "Paste"), (kVK_ANSI_X, cmd, "Cut"), (kVK_ANSI_Z, cmd, "Undo"),
            (kVK_ANSI_Z, cmd | UInt32(shiftKey), "Redo"), (kVK_ANSI_Comma, cmd, "Settings"),
            (kVK_Tab, cmd, "App Switcher"), (kVK_ANSI_Grave, cmd, "Cycle Windows"),
            (kVK_ANSI_Q, cmd | UInt32(controlKey), "Lock Screen"),
            (kVK_ANSI_F, cmd | UInt32(controlKey), "Full Screen"),
            (kVK_Escape, cmd | UInt32(optionKey), "Force Quit"),
        ]
        return Dictionary(uniqueKeysWithValues: pairs.map { (Hotkey(keyCode: UInt32($0.0), modifiers: $0.1), $0.2) })
    }()
}

extension Hotkey: Hashable {}

final class HotkeyManager {
    static let shared = HotkeyManager()

    enum RegistrationError: Error {
        case failed(OSStatus)

        var message: String {
            switch self {
            case .failed(let status) where status == OSStatus(eventHotKeyExistsErr):
                return "This shortcut is already taken by another app."
            case .failed(let status):
                return "macOS refused to register this shortcut (error \(status))."
            }
        }
    }

    /// Called on the main thread when the registered hotkey is pressed.
    var onPressed: (@MainActor () -> Void)?
    private(set) var registeredHotkey: Hotkey?
    /// Set when the saved hotkey couldn't be registered at launch.
    private(set) var launchRegistrationError: RegistrationError?
    /// True when another app had already claimed the hotkey exclusively, so ours was
    /// registered as shared and macOS may deliver the press to that app instead.
    private(set) var isSharedWithAnotherApp = false

    private var hotKeyRef: EventHotKeyRef?
    private var eventHandlerRef: EventHandlerRef?
    private static let hotKeyID = EventHotKeyID(signature: OSType(0x4D455648), id: 1) // 'MEVH'

    private init() {}

    func register() {
        installEventHandler()
        guard let hotkey = HotkeySettings.hotkey else { return }
        if case .failure(let error) = registerSystemHotkey(hotkey) {
            launchRegistrationError = error
            AppLogger.error("Hotkey", "Failed to register \(hotkey.displayString): \(error.message)")
        } else if isSharedWithAnotherApp {
            AppLogger.warn("Hotkey", "\(hotkey.displayString) is also registered by another app")
        }
    }

    /// Registers `hotkey` (nil clears it) and persists it on success. On failure the
    /// previous hotkey stays registered and saved.
    @discardableResult
    func apply(_ hotkey: Hotkey?) -> Result<Void, RegistrationError> {
        let previous = registeredHotkey
        unregisterSystemHotkey()
        if let hotkey, case .failure(let error) = registerSystemHotkey(hotkey) {
            if let previous { _ = registerSystemHotkey(previous) }
            return .failure(error)
        }
        launchRegistrationError = nil
        HotkeySettings.hotkey = hotkey
        return .success(())
    }

    /// Temporarily releases the hotkey so it can be pressed while recording a new one.
    func suspend() {
        let hotkey = registeredHotkey
        unregisterSystemHotkey()
        registeredHotkey = hotkey
    }

    func resume() {
        guard hotKeyRef == nil, let hotkey = registeredHotkey else { return }
        _ = registerSystemHotkey(hotkey)
    }

    /// Tries an exclusive registration first, which is how macOS reports that another app
    /// already holds the combination; in that case falls back to a shared registration
    /// (as before this check existed) and flags it so settings can warn about it.
    private func registerSystemHotkey(_ hotkey: Hotkey) -> Result<Void, RegistrationError> {
        var ref: EventHotKeyRef?
        var status = RegisterEventHotKey(hotkey.keyCode, hotkey.modifiers, Self.hotKeyID,
                                         GetApplicationEventTarget(), OptionBits(kEventHotKeyExclusive), &ref)
        isSharedWithAnotherApp = status == OSStatus(eventHotKeyExistsErr)
        if isSharedWithAnotherApp {
            status = RegisterEventHotKey(hotkey.keyCode, hotkey.modifiers, Self.hotKeyID,
                                         GetApplicationEventTarget(), OptionBits(kEventHotKeyNoOptions), &ref)
        }
        guard status == noErr, let ref else {
            isSharedWithAnotherApp = false
            return .failure(.failed(status))
        }
        hotKeyRef = ref
        registeredHotkey = hotkey
        return .success(())
    }

    private func unregisterSystemHotkey() {
        if let ref = hotKeyRef {
            UnregisterEventHotKey(ref)
            hotKeyRef = nil
        }
        registeredHotkey = nil
        isSharedWithAnotherApp = false
    }

    private func installEventHandler() {
        guard eventHandlerRef == nil else { return }
        var eventSpec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard),
                                      eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, event, _ -> OSStatus in
            var pressedID = EventHotKeyID()
            GetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID),
                              nil, MemoryLayout<EventHotKeyID>.size, nil, &pressedID)
            guard pressedID.signature == HotkeyManager.hotKeyID.signature,
                  pressedID.id == HotkeyManager.hotKeyID.id else { return OSStatus(eventNotHandledErr) }
            DispatchQueue.main.async { MainActor.assumeIsolated { HotkeyManager.shared.onPressed?() } }
            return noErr
        }, 1, &eventSpec, nil, &eventHandlerRef)
    }

    deinit {
        unregisterSystemHotkey()
        if let handler = eventHandlerRef {
            RemoveEventHandler(handler)
        }
    }
}
