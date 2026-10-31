// Run from the repository root: swift assets/branding/generate.swift
import AppKit

let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let catalog = root.appendingPathComponent("MacEverything/Assets.xcassets")
let brand = root.appendingPathComponent("assets/branding")

func mark(_ context: CGContext) {
    context.setLineCap(.round)
    context.setLineJoin(.round)
    context.setLineWidth(58)
    context.strokeEllipse(in: CGRect(x: 246, y: 228, width: 462, height: 462))
    context.move(to: CGPoint(x: 645, y: 627))
    context.addLine(to: CGPoint(x: 790, y: 772))
    context.strokePath()
    context.setLineWidth(45)
    for (y, end) in [(365.0, 563.0), (459.0, 517.0), (553.0, 563.0)] {
        context.move(to: CGPoint(x: 389, y: y))
        context.addLine(to: CGPoint(x: end, y: y))
        context.strokePath()
    }
}

func icon(_ context: CGContext) {
    context.saveGState()
    let tile = CGPath(roundedRect: CGRect(x: 64, y: 64, width: 896, height: 896),
                      cornerWidth: 204, cornerHeight: 204, transform: nil)
    context.setShadow(offset: CGSize(width: 0, height: 12), blur: 24,
                      color: NSColor.black.withAlphaComponent(0.18).cgColor)
    context.setFillColor(NSColor(calibratedRed: 0.03, green: 0.34, blue: 0.67, alpha: 1).cgColor)
    context.addPath(tile)
    context.fillPath()
    context.setShadow(offset: .zero, blur: 0)
    context.addPath(tile)
    context.clip()
    let gradient = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: [
        NSColor(calibratedRed: 0.13, green: 0.83, blue: 0.77, alpha: 1).cgColor,
        NSColor(calibratedRed: 0.04, green: 0.51, blue: 0.75, alpha: 1).cgColor,
        NSColor(calibratedRed: 0.10, green: 0.27, blue: 0.63, alpha: 1).cgColor
    ] as CFArray, locations: [0, 0.52, 1])!
    context.drawLinearGradient(gradient, start: CGPoint(x: 130, y: 60),
                               end: CGPoint(x: 860, y: 1000), options: [])
    context.restoreGState()
    context.setStrokeColor(NSColor.white.cgColor)
    context.setShadow(offset: CGSize(width: 0, height: 5), blur: 10,
                      color: NSColor(calibratedWhite: 0, alpha: 0.12).cgColor)
    mark(context)
    context.setShadow(offset: .zero, blur: 0)
}

func png(width: Int, height: Int, draw: (CGContext) -> Void, to url: URL) throws {
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
    let context = NSGraphicsContext.current!.cgContext
    context.translateBy(x: 0, y: CGFloat(height))
    context.scaleBy(x: 1, y: -1)
    draw(context)
    NSGraphicsContext.restoreGraphicsState()
    try bitmap.representation(using: .png, properties: [:])!.write(to: url)
}

for size in [16, 32, 64, 128, 256, 512, 1024] {
    try png(width: size, height: size, draw: { context in
        context.scaleBy(x: CGFloat(size) / 1024, y: CGFloat(size) / 1024)
        icon(context)
    }, to: catalog.appendingPathComponent("AppIcon.appiconset/icon_\(size).png"))
}

let logoSet = catalog.appendingPathComponent("BrandIcon.imageset")
let menuSet = catalog.appendingPathComponent("MenuBarIcon.imageset")
try FileManager.default.createDirectory(at: logoSet, withIntermediateDirectories: true)
try FileManager.default.createDirectory(at: menuSet, withIntermediateDirectories: true)
try Data(contentsOf: catalog.appendingPathComponent("AppIcon.appiconset/icon_512.png"))
    .write(to: logoSet.appendingPathComponent("brand-icon.png"))
try """
{"images":[{"filename":"brand-icon.png","idiom":"universal"}],"info":{"author":"xcode","version":1}}
""".write(to: logoSet.appendingPathComponent("Contents.json"), atomically: true, encoding: .utf8)
try png(width: 48, height: 48, draw: { context in
    context.scaleBy(x: 48 / 660, y: 48 / 660)
    context.translateBy(x: -190, y: -180)
    context.setStrokeColor(NSColor.black.cgColor)
    mark(context)
}, to: menuSet.appendingPathComponent("menu-bar.png"))
try """
{"images":[{"filename":"menu-bar.png","idiom":"universal"}],"info":{"author":"xcode","version":1},"properties":{"template-rendering-intent":"template"}}
""".write(to: menuSet.appendingPathComponent("Contents.json"), atomically: true, encoding: .utf8)

// The wordmark uses system typography and a transparent background.
for dark in [false, true] {
try png(width: 1120, height: 300, draw: { context in
    context.saveGState()
    context.scaleBy(x: 300 / 1024, y: 300 / 1024)
    icon(context)
    context.restoreGState()
    context.translateBy(x: 320, y: 206)
    context.scaleBy(x: 1, y: -1)
    let text = "MacEverything" as NSString
    text.draw(at: .zero, withAttributes: [.font: NSFont.systemFont(ofSize: 108, weight: .semibold),
        .foregroundColor: dark ? NSColor.white : NSColor(calibratedRed: 0.08, green: 0.16, blue: 0.23, alpha: 1)])
}, to: brand.appendingPathComponent(dark ? "logo-dark.png" : "logo.png"))
}
print("Generated all seven icon sizes, BrandIcon, MenuBarIcon, and logo.png")

let symbol = """
<circle cx="477" cy="459" r="231"/><path d="M645 627L790 772"/><path d="M389 365H563M389 459H517M389 553H563" stroke-width="45"/>
"""
let vector = """
<defs><linearGradient id="tile" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#21d4c4"/><stop offset=".52" stop-color="#0a82bf"/><stop offset="1" stop-color="#1a45a1"/></linearGradient></defs>
<rect x="64" y="64" width="896" height="896" rx="204" fill="url(#tile)"/>
<g fill="none" stroke="white" stroke-width="58" stroke-linecap="round" stroke-linejoin="round">\(symbol)</g>
"""
try """
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" role="img" aria-label="MacEverything icon">\(vector)</svg>
""".write(to: brand.appendingPathComponent("icon.svg"), atomically: true, encoding: .utf8)
try """
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1120 300" role="img" aria-label="MacEverything">
<style>text{fill:#14293b}@media(prefers-color-scheme:dark){text{fill:white}}</style>
<g transform="scale(0.29296875)">\(vector)</g>
<text x="320" y="198" font-family="-apple-system,BlinkMacSystemFont,Helvetica,Arial,sans-serif" font-size="108" font-weight="600" letter-spacing="-4">MacEverything</text>
</svg>
""".write(to: brand.appendingPathComponent("logo.svg"), atomically: true, encoding: .utf8)
