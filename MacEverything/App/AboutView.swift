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
                Text("菜单栏搜索支持分页结果、快捷键冲突检查及浮动面板，并提供 Quick Look 和 Finder 定位。\n\n优化大索引内存与查询性能，修复目录重扫及索引写入问题；新增 Apple Silicon 与 Intel 的 PR 构建验证。")
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
