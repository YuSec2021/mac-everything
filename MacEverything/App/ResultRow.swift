import SwiftUI
import AppKit

/// Shared icon cache to avoid repeated NSWorkspace.shared.icon(forFile:) calls.
/// - App bundles (type=5): cached by full path (each app has a unique icon)
/// - Regular files (type=1): cached by file extension (same ext → same icon)
/// - Directories/symlinks/other: cached by type (one icon per type)
final class FileIconCache {
    static let shared = FileIconCache()
    private let cache = NSCache<NSString, NSImage>()

    private init() {
        cache.countLimit = 500
    }

    func icon(for item: FileItem) -> NSImage {
        // Detect app bundles by name suffix (handles both type=5 and legacy type=2)
        let isApp = item.name.hasSuffix(".app")

        let key: String
        if isApp {
            // App bundles — unique icon per app
            key = "app:" + item.path + "/" + item.name
        } else if item.type == 1 {
            // Regular file — same icon per extension
            let ext = (item.name as NSString).pathExtension.lowercased()
            key = "ext:" + (ext.isEmpty ? "__no_ext__" : ext)
        } else {
            // Dir, symlink, other — one icon per type
            key = "type:\(item.type)"
        }

        let nsKey = key as NSString
        if let cached = cache.object(forKey: nsKey) {
            return cached
        }

        let fullPath = item.path + "/" + item.name
        let nsImage = NSWorkspace.shared.icon(forFile: fullPath)
        nsImage.size = NSSize(width: 24, height: 24)
        cache.setObject(nsImage, forKey: nsKey)
        return nsImage
    }

    func icon(forPath path: String) -> NSImage {
        let ext = (path as NSString).pathExtension.lowercased()
        let key = "ext:" + (ext.isEmpty ? "__no_ext__" : ext) as NSString
        if let cached = cache.object(forKey: key) {
            return cached
        }
        let nsImage = NSWorkspace.shared.icon(forFile: path)
        nsImage.size = NSSize(width: 24, height: 24)
        cache.setObject(nsImage, forKey: key)
        return nsImage
    }
}

/// Open / reveal / copy for result rows; the result lists' context menu and primary
/// action (double-click, Return) call these with the rows' file URLs.
enum FileActions {
    /// ⌘ turns the primary action into Reveal in Finder.
    static func performPrimaryAction(on url: URL, isApp: Bool) {
        // The triggering click/key event carries the flags it was sent with; the global
        // state can lag behind it.
        let flags = NSApp.currentEvent?.modifierFlags ?? NSEvent.modifierFlags
        if flags.contains(.command) {
            reveal([url])
        } else {
            open(url, isApp: isApp)
        }
    }

    static func open(_ url: URL, isApp: Bool) {
        if isApp {
            NSWorkspace.shared.openApplication(at: url, configuration: NSWorkspace.OpenConfiguration())
        } else if !NSWorkspace.shared.open(url) {
            NSSound.beep()
        }
    }

    static func reveal(_ urls: [URL]) {
        NSWorkspace.shared.activateFileViewerSelecting(urls)
    }

    static func copyPaths(_ urls: [URL]) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(urls.map(\.path).joined(separator: "\n"), forType: .string)
    }
}

struct ResultRow: View {
    let item: FileItem
    let hints: [HighlightHint]
    @State private var isHovered = false

    var body: some View {
        HStack(spacing: 12) {
            fileIcon(for: item)
                .resizable()
                .aspectRatio(contentMode: .fit)
                .frame(width: 24, height: 24)

            VStack(alignment: .leading, spacing: 4) {
                let highlighted = highlightCrossMatches(
                    path: item.path, name: item.name, hints: hints,
                    nameFont: .system(size: 13, weight: .medium), nameColor: .primary,
                    pathFont: .system(size: 11), pathColor: .secondary)
                highlighted.nameText.lineLimit(1).truncationMode(.middle)
                highlighted.pathText.lineLimit(1).truncationMode(.middle)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .layoutPriority(1)

            if item.type == 1 && item.size > 0 {
                Text(formatSize(item.size))
                    .font(.system(size: 11).monospacedDigit())
                    .frame(width: 72, alignment: .trailing)
                    .foregroundColor(.secondary)
            }
        }
        .frame(minHeight: 36)
        .padding(.vertical, 3)
        .padding(.horizontal, 14)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(isHovered ? Color.primary.opacity(0.045) : Color.clear)
        )
        .help(item.path + "/" + item.name)
        .contentShape(Rectangle())
        .onHover { hovering in
            isHovered = hovering
        }
        .onDrag {
            let fullPath = item.path + "/" + item.name
            return NSItemProvider(object: NSURL(fileURLWithPath: fullPath))
        }
        // Simultaneous leaves the click to the List too, so it still selects the row.
        .simultaneousGesture(TapGesture().modifiers(.command).onEnded {
            FileActions.reveal([URL(fileURLWithPath: item.path + "/" + item.name)])
        })
        .accessibilityIdentifier("resultRow")
    }

    private func fileIcon(for item: FileItem) -> Image {
        Image(nsImage: FileIconCache.shared.icon(for: item))
    }

    private func formatSize(_ bytes: UInt64) -> String {
        let units = ["B", "KB", "MB", "GB", "TB"]
        var size = Double(bytes)
        var unitIndex = 0
        while size >= 1024 && unitIndex < units.count - 1 {
            size /= 1024
            unitIndex += 1
        }
        if unitIndex == 0 { return "\(bytes) B" }
        return String(format: "%.1f %@", size, units[unitIndex])
    }
}
