import SwiftUI
import Carbon.HIToolbox

struct ShortcutSettingsView: View {
    private enum Message {
        case info(String)
        case warning(String)
        case error(String)
    }

    @State private var isRecording = false
    @State private var hotkey: Hotkey? = HotkeySettings.hotkey
    @State private var message: Message?

    var body: some View {
        VStack(spacing: 18) {
            Text("Global Hotkey Settings")
                .font(.title2)
                .fontWeight(.semibold)

            Text("Record a key combination to open or close the menu bar search panel from any app.")
                .font(.body)
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: 360)

            HStack(spacing: 8) {
                Button(action: toggleRecording) {
                    Text(recorderTitle)
                        .font(.title3)
                        .fontWeight(.medium)
                        .foregroundColor(isRecording ? .accentColor : (hotkey == nil ? .secondary : .primary))
                        .frame(minWidth: 220, minHeight: 40)
                        .background(
                            RoundedRectangle(cornerRadius: 8)
                                .stroke(isRecording ? Color.accentColor : Color.gray.opacity(0.4), lineWidth: isRecording ? 2 : 1)
                                .background(
                                    RoundedRectangle(cornerRadius: 8)
                                        .fill(Color(nsColor: .controlBackgroundColor))
                                )
                        )
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("hotkeyRecorder")

                Button {
                    finishRecording()
                    apply(nil)
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 16))
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
                .disabled(hotkey == nil && !isRecording)
                .help("Clear shortcut")
                .accessibilityLabel("Clear shortcut")
            }

            if isRecording {
                ShortcutRecorderView(
                    onKeyCombo: { recorded in
                        finishRecording()
                        record(recorded)
                    },
                    onClear: {
                        finishRecording()
                        apply(nil)
                    },
                    onCancel: finishRecording
                )
                .frame(width: 0, height: 0)
            }

            messageView
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: 380, minHeight: 34, alignment: .top)

            HStack(spacing: 16) {
                Button("Reset to Default") {
                    finishRecording()
                    record(.defaultHotkey)
                }

                Button("Close") {
                    NSApp.keyWindow?.close()
                }
                .keyboardShortcut(.cancelAction)
            }
        }
        .padding(30)
        .frame(width: 460)
        .onAppear {
            hotkey = HotkeySettings.hotkey
            if let error = HotkeyManager.shared.launchRegistrationError {
                message = .error("\(hotkey?.displayString ?? "The shortcut") isn't active: \(error.message) Record a different one.")
            } else if HotkeyManager.shared.isSharedWithAnotherApp, let hotkey {
                message = .warning(sharedWarning(hotkey))
            }
        }
        .onDisappear(perform: finishRecording)
    }

    private var recorderTitle: String {
        if isRecording { return "Type shortcut…" }
        return hotkey?.displayString ?? "Click to record"
    }

    @ViewBuilder
    private var messageView: some View {
        switch message {
        case .info(let text):
            Label(text, systemImage: "checkmark.circle").foregroundStyle(.secondary)
        case .warning(let text):
            Label(text, systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
        case .error(let text):
            Label(text, systemImage: "xmark.octagon").foregroundStyle(.red)
        case nil:
            Text(isRecording ? "Press Esc to cancel, Delete to clear." : " ")
                .foregroundStyle(.secondary)
        }
    }

    private func toggleRecording() {
        if isRecording {
            finishRecording()
        } else {
            message = nil
            HotkeyManager.shared.suspend()
            isRecording = true
        }
    }

    private func finishRecording() {
        guard isRecording else { return }
        isRecording = false
        HotkeyManager.shared.resume()
    }

    private func record(_ candidate: Hotkey) {
        let conflict = HotkeyConflictChecker.check(candidate)
        if let conflict, conflict.severity == .blocking {
            message = .error(conflict.message)
            return
        }
        guard apply(candidate) else { return }
        if HotkeyManager.shared.isSharedWithAnotherApp {
            message = .warning(sharedWarning(candidate))
        } else if let conflict {
            message = .warning(conflict.message)
        }
    }

    private func sharedWarning(_ hotkey: Hotkey) -> String {
        "Another running app has also claimed \(hotkey.displayString), so pressing it may open that app instead. Choose a different shortcut if MacEverything doesn't appear."
    }

    @discardableResult
    private func apply(_ candidate: Hotkey?) -> Bool {
        switch HotkeyManager.shared.apply(candidate) {
        case .success:
            hotkey = candidate
            message = .info(candidate.map { "\($0.displayString) is now active." } ?? "Global hotkey turned off.")
            return true
        case .failure(let error):
            message = .error("Couldn't use \(candidate?.displayString ?? "this shortcut"): \(error.message)")
            return false
        }
    }
}

func describeShortcut(keyCode: UInt32, modifiers: UInt32) -> String {
    var parts: [String] = []

    if modifiers & UInt32(controlKey) != 0 { parts.append("⌃") }
    if modifiers & UInt32(optionKey) != 0 { parts.append("⌥") }
    if modifiers & UInt32(shiftKey) != 0 { parts.append("⇧") }
    if modifiers & UInt32(cmdKey) != 0 { parts.append("⌘") }

    parts.append(specialKeyNames[Int(keyCode)] ?? printableKeyName(keyCode) ?? "Key(\(keyCode))")
    return parts.joined(separator: "")
}

private let specialKeyNames: [Int: String] = [
    kVK_Space: "Space", kVK_Return: "Return", kVK_ANSI_KeypadEnter: "Enter", kVK_Tab: "Tab",
    kVK_Escape: "Esc", kVK_Delete: "Delete", kVK_ForwardDelete: "Fwd Delete",
    kVK_UpArrow: "↑", kVK_DownArrow: "↓", kVK_LeftArrow: "←", kVK_RightArrow: "→",
    kVK_Home: "Home", kVK_End: "End", kVK_PageUp: "Page Up", kVK_PageDown: "Page Down",
    kVK_F1: "F1", kVK_F2: "F2", kVK_F3: "F3", kVK_F4: "F4", kVK_F5: "F5",
    kVK_F6: "F6", kVK_F7: "F7", kVK_F8: "F8", kVK_F9: "F9", kVK_F10: "F10",
    kVK_F11: "F11", kVK_F12: "F12", kVK_F13: "F13", kVK_F14: "F14", kVK_F15: "F15",
    kVK_F16: "F16", kVK_F17: "F17", kVK_F18: "F18", kVK_F19: "F19", kVK_F20: "F20",
]

/// The character a key produces on the current keyboard layout (e.g. "A", "/", "Ö"),
/// so shortcuts display the way they are printed on the user's keyboard.
func printableKeyName(_ keyCode: UInt32) -> String? {
    guard let source = TISCopyCurrentKeyboardLayoutInputSource()?.takeRetainedValue(),
          let layoutPtr = TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) else { return nil }
    let layoutData = Unmanaged<CFData>.fromOpaque(layoutPtr).takeUnretainedValue() as Data
    var deadKeyState: UInt32 = 0
    var chars = [UniChar](repeating: 0, count: 4)
    var length = 0
    let status = layoutData.withUnsafeBytes { raw -> OSStatus in
        guard let layout = raw.bindMemory(to: UCKeyboardLayout.self).baseAddress else { return OSStatus(paramErr) }
        return UCKeyTranslate(layout, UInt16(keyCode), UInt16(kUCKeyActionDisplay), 0, UInt32(LMGetKbdType()),
                              OptionBits(kUCKeyTranslateNoDeadKeysBit), &deadKeyState, chars.count, &length, &chars)
    }
    guard status == noErr, length > 0 else { return nil }
    let name = String(utf16CodeUnits: chars, count: length).trimmingCharacters(in: .whitespacesAndNewlines.union(.controlCharacters))
    return name.isEmpty ? nil : name.uppercased()
}

// NSView-based key recorder to capture raw key events
struct ShortcutRecorderView: NSViewRepresentable {
    var onKeyCombo: (Hotkey) -> Void
    var onClear: () -> Void
    var onCancel: () -> Void

    func makeNSView(context: Context) -> ShortcutRecorderNSView {
        let view = ShortcutRecorderNSView()
        view.onKeyCombo = onKeyCombo
        view.onClear = onClear
        view.onCancel = onCancel
        DispatchQueue.main.async {
            view.window?.makeFirstResponder(view)
        }
        return view
    }

    func updateNSView(_ nsView: ShortcutRecorderNSView, context: Context) {
        nsView.onKeyCombo = onKeyCombo
        nsView.onClear = onClear
        nsView.onCancel = onCancel
    }
}

class ShortcutRecorderNSView: NSView {
    var onKeyCombo: ((Hotkey) -> Void)?
    var onClear: (() -> Void)?
    var onCancel: (() -> Void)?

    override var acceptsFirstResponder: Bool { true }

    // ⌘-combinations arrive here before the menu; capture them so recording ⌘Q doesn't quit.
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        guard window?.firstResponder === self, event.type == .keyDown else { return false }
        keyDown(with: event)
        return true
    }

    override func keyDown(with event: NSEvent) {
        let hotkey = Hotkey(event: event)
        if hotkey.modifiers == 0 {
            switch Int(event.keyCode) {
            case kVK_Escape: onCancel?(); return
            case kVK_Delete, kVK_ForwardDelete: onClear?(); return
            default: break
            }
        }
        // Validation (modifier rules, conflicts) is reported by the settings view.
        onKeyCombo?(hotkey)
    }

    override func flagsChanged(with event: NSEvent) {
        // Ignore pure modifier key presses
    }

    override func resignFirstResponder() -> Bool {
        DispatchQueue.main.async { [weak self] in self?.onCancel?() }
        return true
    }
}
