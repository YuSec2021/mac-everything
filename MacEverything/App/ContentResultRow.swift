import SwiftUI
import AppKit

struct ContentResultRow: View {
    let item: ContentFileItem
    let keyword: String
    @State private var isHovered = false

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            fileIcon(for: item.filePath)
                .resizable()
                .aspectRatio(contentMode: .fit)
                .frame(width: 24, height: 24)

            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(item.fileName)
                        .font(.system(size: 13))
                        .fontWeight(.medium)
                        .foregroundColor(.primary)
                        .lineLimit(1)

                    Text(directoryPath(item.filePath))
                        .font(.system(size: 11))
                        .foregroundColor(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }

                highlightMatches(in: item.snippet, keyword: keyword, font: .caption, color: .secondary)
                    .lineLimit(2)
            }

            Spacer()
        }
        .frame(minHeight: 48)
        .padding(.vertical, 8)
        .padding(.horizontal, 14)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(isHovered ? Color.primary.opacity(0.045) : Color.clear)
        )
        .help(item.filePath)
        .contentShape(Rectangle())
        .onHover { hovering in
            isHovered = hovering
        }
        .contextMenu {
            Button("Open") { openFile() }
            Button("Reveal in Finder") { revealInFinder() }
            Divider()
            Button("Copy Path") { copyPath() }
        }
        .onDrag {
            return NSItemProvider(object: NSURL(fileURLWithPath: item.filePath))
        }
        // Simultaneous gestures leave the click to the List too, so it can select the row.
        .simultaneousGesture(TapGesture(count: 2).onEnded {
            if NSEvent.modifierFlags.contains(.command) {
                revealInFinder()
            } else {
                openFile()
            }
        })
        .simultaneousGesture(TapGesture(count: 1).onEnded {
            if NSEvent.modifierFlags.contains(.command) {
                revealInFinder()
            }
        })
        .accessibilityIdentifier("contentResultRow")
    }

    private func directoryPath(_ fullPath: String) -> String {
        if let range = fullPath.range(of: "/", options: .backwards) {
            return String(fullPath[fullPath.startIndex..<range.lowerBound])
        }
        return fullPath
    }

    private func fileIcon(for path: String) -> Image {
        Image(nsImage: FileIconCache.shared.icon(forPath: path))
    }

    private func openFile() {
        if !NSWorkspace.shared.open(URL(fileURLWithPath: item.filePath)) {
            NSSound.beep()
        }
    }

    private func revealInFinder() {
        NSWorkspace.shared.selectFile(item.filePath, inFileViewerRootedAtPath: "")
    }

    private func copyPath() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(item.filePath, forType: .string)
    }
}
