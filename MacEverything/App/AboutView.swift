import SwiftUI

struct AboutView: View {
    private var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "—"
    }

    private var build: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "—"
    }

    var body: some View {
        VStack(spacing: 20) {
            VStack(spacing: 8) {
                Image("BrandIcon")
                    .resizable()
                    .interpolation(.high)
                    .frame(width: 112, height: 112)
                    .accessibilityHidden(true)
                Text("MacEverything")
                    .font(.system(size: 28, weight: .semibold, design: .rounded))
                Text("Everything on your Mac. One search away.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                Text("Version \(version) · Build \(build)")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(.quaternary, in: Capsule())
            }

            Divider()

            HStack {
                Text("Created by")
                    .foregroundStyle(.secondary)
                Spacer()
                Text("YuSec")
                    .fontWeight(.medium)
            }

            VStack(alignment: .leading, spacing: 8) {
                Text("版本说明 / Version notes")
                    .font(.headline)
                Text("全新蓝绿渐变图标与品牌标志；新增 About 窗口，集中展示作者、版本和项目地址。\n\n支持文件名与内容搜索、高级查询语法、全局快捷键及 MCP 集成。")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(16)
            .background(.quaternary, in: RoundedRectangle(cornerRadius: 12))

            VStack(spacing: 12) {
                Link(destination: URL(string: "https://github.com/YuSec2021/mac-everything")!) {
                    Label("YuSec2021/mac-everything", systemImage: "arrow.up.right.square")
                }
                .accessibilityIdentifier("aboutGitHubLink")
                Link("查看完整更新记录 / Changelog", destination: URL(string:
                    "https://github.com/YuSec2021/mac-everything/blob/main/docs/changelog/README.md")!)
                    .font(.caption)
            }

            Text("Made for macOS · Open source under the MIT License")
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
        .padding(32)
        .frame(width: 448)
    }
}

final class AboutWindowController {
    static let shared = AboutWindowController()
    private var window: NSWindow?

    func showWindow() {
        NSApp.activate(ignoringOtherApps: true)
        if let window {
            window.deminiaturize(nil)
            window.makeKeyAndOrderFront(nil)
            return
        }

        let window = NSWindow(contentViewController: NSHostingController(rootView: AboutView()))
        window.title = "About MacEverything"
        window.styleMask = [.titled, .closable]
        window.isReleasedWhenClosed = false
        window.center()
        window.makeKeyAndOrderFront(nil)
        self.window = window
    }
}
