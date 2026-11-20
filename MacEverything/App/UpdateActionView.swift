import SwiftUI

/// Liquid-glass update control placed in the search panel header to the left of the gear menu.
struct UpdateActionView: View {
    let manager: ReleaseUpdateManager

    var body: some View {
        switch manager.phase {
        case .hidden:
            EmptyView()

        case .available(let release):
            Button {
                manager.startDownload()
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: "arrow.down.circle.fill")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(Glass.accent)
                    Text("下载更新 \(release.displayTag)")
                        .font(.system(size: 11.5, weight: .medium))
                        .foregroundStyle(Glass.text)
                        .lineLimit(1)
                }
            }
            .buttonStyle(GlassPillButtonStyle(tint: Glass.accent))
            .help("下载 MacEverything \(release.displayTag)（\(release.architecture.displayName)）安装包")
            .accessibilityLabel("下载更新 \(release.displayTag)")
            .accessibilityIdentifier("updateDownloadButton")

        case .downloading(let release, let progress, _, _):
            HStack(spacing: 6) {
                if let progress {
                    ProgressView(value: progress, total: 1.0)
                        .progressViewStyle(.circular)
                        .controlSize(.mini)
                        .tint(Glass.accent)
                    Text("下载中 \(Int((progress * 100).rounded()))%")
                        .font(.system(size: 11, weight: .medium).monospacedDigit())
                        .foregroundStyle(Glass.text.opacity(0.9))
                        .lineLimit(1)
                } else {
                    ProgressView()
                        .controlSize(.mini)
                        .tint(Glass.accent)
                    Text("下载中…")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(Glass.text.opacity(0.9))
                        .lineLimit(1)
                }
            }
            .padding(.horizontal, 11)
            .frame(height: 28)
            .background(Capsule().fill(Glass.accent.opacity(0.14)))
            .overlay(Capsule().strokeBorder(Glass.accent.opacity(0.32), lineWidth: 1))
            .help("正在下载 MacEverything \(release.displayTag) 安装包到下载目录")
            .accessibilityElement(children: .combine)
            .accessibilityLabel("正在下载更新 \(release.displayTag)")
            .accessibilityIdentifier("updateDownloadingIndicator")

        case .downloaded(let release, let fileURL):
            HStack(spacing: 6) {
                Button {
                    manager.openDownloadedInstaller()
                } label: {
                    HStack(spacing: 5) {
                        Image(systemName: "checkmark.circle.fill")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(Glass.live)
                        Text("打开 \(release.displayTag)")
                            .font(.system(size: 11.5, weight: .medium))
                            .foregroundStyle(Glass.text)
                            .lineLimit(1)
                    }
                }
                .buttonStyle(GlassPillButtonStyle(tint: Glass.live))
                .help("打开已下载的 \(fileURL.lastPathComponent)")
                .accessibilityLabel("打开安装包 \(release.displayTag)")
                .accessibilityIdentifier("updateOpenInstallerButton")

                Button {
                    manager.revealDownloadedInstallerInFinder()
                } label: {
                    Image(systemName: "folder")
                        .font(.system(size: 11.5, weight: .medium))
                }
                .buttonStyle(GlassIconButtonStyle(size: 28))
                .help("在 Finder 中显示 \(fileURL.lastPathComponent)")
                .accessibilityLabel("在 Finder 中显示安装包")
                .accessibilityIdentifier("updateRevealInFinderButton")
            }

        case .failed(_, let reason):
            Button {
                manager.retry()
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .font(.system(size: 11.5, weight: .semibold))
                        .foregroundStyle(Color.orange)
                    Text(reason.shortTitle)
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(Glass.text)
                        .lineLimit(1)
                }
            }
            .buttonStyle(GlassPillButtonStyle(tint: .orange))
            .help(reason.detailMessage)
            .accessibilityLabel(reason.detailMessage)
            .accessibilityIdentifier("updateRetryButton")
        }
    }
}

private struct GlassPillButtonStyle: ButtonStyle {
    let tint: Color

    func makeBody(configuration: Configuration) -> some View {
        GlassPillButtonBody(tint: tint, isPressed: configuration.isPressed) {
            configuration.label
        }
    }
}

private struct GlassPillButtonBody<Label: View>: View {
    let tint: Color
    let isPressed: Bool
    @ViewBuilder let label: () -> Label
    @State private var isHovered = false

    var body: some View {
        label()
            .padding(.horizontal, 11)
            .frame(height: 28)
            .background(
                Capsule().fill(
                    LinearGradient(
                        colors: [
                            tint.opacity(isPressed ? 0.30 : isHovered ? 0.24 : 0.16),
                            Color.white.opacity(isPressed ? 0.12 : isHovered ? 0.08 : 0.045),
                        ],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    )
                )
            )
            .overlay(
                Capsule().strokeBorder(
                    tint.opacity(isPressed ? 0.55 : isHovered ? 0.45 : 0.30),
                    lineWidth: 1
                )
            )
            .contentShape(Capsule())
            .onHover { isHovered = $0 }
    }
}
