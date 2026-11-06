import SwiftUI
import Observation

@MainActor
@Observable
final class SearchOptions {
    static let shared = SearchOptions()

    var isRegex: Bool = false {
        didSet {
            if isRegex {
                isWholeWord = false
                isMatchFilename = false
            }
        }
    }
    var isCaseSensitive: Bool = false
    var isWholeWord: Bool = false {
        didSet {
            if isWholeWord {
                isRegex = false
                isMatchFilename = false
            }
        }
    }
    var isMatchFilename: Bool = false {
        didSet {
            if isMatchFilename {
                isRegex = false
                isWholeWord = false
            }
        }
    }

    private init() {}

    var hasActiveOptions: Bool {
        isRegex || isCaseSensitive || isWholeWord || isMatchFilename
    }

    /// Build query string with prefixes for the C++ engine
    func buildQuery(_ keyword: String) -> String {
        guard !keyword.isEmpty else { return keyword }
        var prefix = ""
        if isRegex { prefix += "regex:" }
        if isCaseSensitive { prefix += "case:" }
        if isWholeWord { prefix += "ww:" }
        if isMatchFilename { prefix += "wfn:" }
        return prefix + keyword
    }
}

struct SearchOptionBadges: View {
    let options: SearchOptions

    var body: some View {
        if options.hasActiveOptions {
            HStack(spacing: 4) {
                if options.isRegex {
                    badge("Regex", help: "Disable regular expression matching") { options.isRegex.toggle() }
                }
                if options.isCaseSensitive {
                    badge("Case sensitive", help: "Disable case sensitive matching") { options.isCaseSensitive.toggle() }
                }
                if options.isWholeWord {
                    badge("Whole word", help: "Disable whole word matching") { options.isWholeWord.toggle() }
                }
                if options.isMatchFilename {
                    badge("Filename", help: "Disable whole filename matching") { options.isMatchFilename.toggle() }
                }
            }
        }
    }

    private func badge(_ text: String, help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 4) {
                Text(text)
                Image(systemName: "xmark").font(.system(size: 8, weight: .semibold))
            }
            .font(.system(size: 10, weight: .medium))
            .foregroundStyle(Glass.accent)
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .background(Glass.accent.opacity(0.14), in: Capsule())
            .overlay(Capsule().strokeBorder(Glass.accent.opacity(0.25), lineWidth: 1))
        }
        .buttonStyle(.plain)
        .help(help)
        .accessibilityLabel(help)
    }
}
