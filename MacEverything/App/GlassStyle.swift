import SwiftUI

/// Colors and surfaces for the liquid glass search UI, taken from the design prototype in
/// docs/prototypes/liquid-glass. The search view always renders dark, so these are fixed
/// values rather than adaptive system colors.
enum Glass {
    static let text = Color(red: 0.95, green: 0.96, blue: 1.0)
    static let secondaryText = Color(red: 0.66, green: 0.69, blue: 0.77)
    static let tertiaryText = Color(red: 0.57, green: 0.60, blue: 0.69)
    static let accent = Color(red: 0.68, green: 0.74, blue: 1.0)
    static let live = Color(red: 0.44, green: 0.91, blue: 0.68)
    static let hairline = Color.white.opacity(0.08)
    static let selection = Color(red: 0.55, green: 0.63, blue: 0.93)
}

/// The window backdrop: a translucent navy tint over the system material, lit by two soft
/// highlights in opposite corners.
struct GlassBackdrop: View {
    var body: some View {
        ZStack {
            Rectangle().fill(.ultraThinMaterial)
            LinearGradient(
                colors: [
                    Color(red: 0.19, green: 0.22, blue: 0.31).opacity(0.82),
                    Color(red: 0.10, green: 0.12, blue: 0.19).opacity(0.90),
                    Color(red: 0.16, green: 0.18, blue: 0.27).opacity(0.84),
                ],
                startPoint: .topLeading, endPoint: .bottomTrailing)
            RadialGradient(
                colors: [Color(red: 0.44, green: 0.49, blue: 0.70).opacity(0.32), .clear],
                center: UnitPoint(x: 0.12, y: 0), startRadius: 0, endRadius: 340)
            RadialGradient(
                colors: [Color(red: 0.32, green: 0.36, blue: 0.59).opacity(0.30), .clear],
                center: UnitPoint(x: 0.95, y: 1), startRadius: 0, endRadius: 320)
        }
        .ignoresSafeArea()
    }
}

/// A raised pane of glass: a faint white wash, a hairline rim and a brighter top edge.
private struct GlassSurface<S: InsettableShape>: ViewModifier {
    let shape: S
    var isFocused = false

    func body(content: Content) -> some View {
        content
            .background(
                LinearGradient(colors: [.white.opacity(0.11), .white.opacity(0.045)],
                               startPoint: .topLeading, endPoint: .bottomTrailing),
                in: shape)
            .overlay {
                shape.strokeBorder(
                    isFocused ? Glass.accent.opacity(0.55) : Color.white.opacity(0.2), lineWidth: 1)
            }
            .overlay {
                // Specular highlight along the top edge.
                shape.inset(by: 0.5).stroke(
                    LinearGradient(colors: [.white.opacity(0.22), .clear],
                                   startPoint: .top, endPoint: .center),
                    lineWidth: 1)
            }
            .shadow(color: isFocused ? Glass.accent.opacity(0.18) : .black.opacity(0.18),
                    radius: isFocused ? 6 : 10, y: isFocused ? 0 : 5)
    }
}

extension View {
    func glassSurface<S: InsettableShape>(_ shape: S, isFocused: Bool = false) -> some View {
        modifier(GlassSurface(shape: shape, isFocused: isFocused))
    }
}

/// Round glass button for the header and search bar controls.
struct GlassIconButtonStyle: ButtonStyle {
    var size: CGFloat = 28

    func makeBody(configuration: Configuration) -> some View {
        GlassIconLabel(size: size, isPressed: configuration.isPressed) { configuration.label }
    }
}

/// The circle behind a glass icon button, with its own hover state.
private struct GlassIconLabel<Label: View>: View {
    var size: CGFloat = 28
    var isPressed = false
    @ViewBuilder let label: () -> Label
    @State private var isHovered = false

    var body: some View {
        label()
            .font(.system(size: 13, weight: .medium))
            .foregroundStyle(Glass.text.opacity(0.85))
            .frame(width: size, height: size)
            .background(Circle().fill(.white.opacity(isPressed ? 0.16 : isHovered ? 0.11 : 0.045)))
            .overlay(Circle().strokeBorder(.white.opacity(0.09), lineWidth: 1))
            .contentShape(Circle())
            .onHover { isHovered = $0 }
    }
}

/// A keyboard key cap for hints, e.g. `Space`.
struct GlassKeyCap: View {
    let key: String

    var body: some View {
        Text(key)
            .font(.system(size: 10, weight: .medium))
            .foregroundStyle(Glass.text.opacity(0.8))
            .padding(.horizontal, 6)
            .padding(.vertical, 3)
            .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 5))
            .overlay(RoundedRectangle(cornerRadius: 5).strokeBorder(.white.opacity(0.15), lineWidth: 1))
    }
}

/// Background for a result row: a soft wash on hover, a tinted gradient pill when selected.
struct GlassRowBackground: View {
    let isSelected: Bool
    let isHovered: Bool

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: 10, style: .continuous)
        if isSelected {
            shape
                .fill(LinearGradient(colors: [Glass.selection.opacity(0.32), Glass.selection.opacity(0.12)],
                                     startPoint: .leading, endPoint: .trailing))
                .overlay(shape.strokeBorder(Glass.accent.opacity(0.22), lineWidth: 1))
        } else {
            shape.fill(.white.opacity(isHovered ? 0.055 : 0))
        }
    }
}

/// Square glass tile that holds a file icon.
struct GlassIconTile: View {
    let image: NSImage

    var body: some View {
        Image(nsImage: image)
            .resizable()
            .aspectRatio(contentMode: .fit)
            .frame(width: 22, height: 22)
            .frame(width: 32, height: 32)
            .background(.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 9, style: .continuous))
    }
}
