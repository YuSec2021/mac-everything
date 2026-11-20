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
                Text("1.5.0：启动后异步检查 GitHub 正式 Release 并缓存结果；有新版本时在搜索面板右上角显示液态玻璃更新按钮，自动匹配 Apple Silicon（arm64）或 Intel（x86_64）DMG 安装包并直接下载到下载目录，支持进度显示、失败重试，以及下载完成后直接打开安装包或在 Finder 中显示。\n\n1.4.1：修复重叠索引压缩误删存活记录的问题；Rebuild Index 改为丢弃全部缓存（含 index.v6 与内容索引）并完整重扫，扫描完成后立即落盘新的 index.v6；扫描与监听跳过 /System/Volumes/Data 避免 APFS 重复索引，旧缓存自动后台重扫去重，状态栏计数文案改为 items indexed。\n\n内容搜索的扩展名和文件大小上限设置重启后保留；MCP 集成修复 Claude Code 配置写错文件（需重新启用一次）、新增 Codex、配置文件解析失败时不再覆盖并正确解码查询与报告 HTTP 错误。\n\n1.4.0：应用常驻菜单栏，搜索面板改为液态玻璃风格，支持分页结果、快捷键冲突检查、Quick Look 和 Finder 定位。")
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
