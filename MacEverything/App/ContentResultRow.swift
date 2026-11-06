import SwiftUI
import AppKit

struct ContentResultRow: View {
    let item: ContentFileItem
    let keyword: String
    @State private var isHovered = false

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            GlassIconTile(image: FileIconCache.shared.icon(forPath: item.filePath))

            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(item.fileName)
                        .font(.system(size: 13))
                        .fontWeight(.semibold)
                        .foregroundColor(Glass.text)
                        .lineLimit(1)

                    Text(directoryPath(item.filePath))
                        .font(.system(size: 11))
                        .foregroundColor(Glass.secondaryText)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }

                highlightMatches(in: item.snippet, keyword: keyword, font: .caption, color: Glass.secondaryText,
                                 highlightColor: Glass.accent)
                    .lineLimit(2)
            }

            Spacer()
        }
        .frame(minHeight: 48)
        .padding(.vertical, 8)
        .padding(.horizontal, 12)
        .background(GlassRowBackground(isSelected: SearchViewModel.shared.selectedResultID == item.id,
                                       isHovered: isHovered))
        .help(item.filePath)
        .contentShape(Rectangle())
        .onHover { hovering in
            isHovered = hovering
        }
        .onDrag {
            return NSItemProvider(object: NSURL(fileURLWithPath: item.filePath))
        }
        // Simultaneous leaves the click to the List too, so it still selects the row.
        .simultaneousGesture(TapGesture().modifiers(.command).onEnded {
            FileActions.reveal([URL(fileURLWithPath: item.filePath)])
        })
        .accessibilityIdentifier("contentResultRow")
    }

    private func directoryPath(_ fullPath: String) -> String {
        if let range = fullPath.range(of: "/", options: .backwards) {
            return String(fullPath[fullPath.startIndex..<range.lowerBound])
        }
        return fullPath
    }
}
