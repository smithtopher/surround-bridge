// Draws the Surround Bridge app icon (original artwork): a speaker sending sound arcs to both
// sides, white on a deep blue-to-violet rounded square, on macOS's 1024 icon grid (824 pt body).
//   swiftc scripts/make_icon.swift -o /tmp/make_icon && /tmp/make_icon out.png
import AppKit

let size = 1024.0
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size), pixelsHigh: Int(size), bitsPerSample: 8,
                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                           bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext

// body: the standard macOS icon shape (824 x 824, corner ~185), with a soft shadow
let body = NSRect(x: 100, y: 100, width: 824, height: 824)
let shape = NSBezierPath(roundedRect: body, xRadius: 185, yRadius: 185)
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -12), blur: 28, color: NSColor.black.withAlphaComponent(0.35).cgColor)
NSColor(calibratedRed: 0.10, green: 0.12, blue: 0.32, alpha: 1).setFill()
shape.fill()
ctx.restoreGState()
NSGradient(colors: [NSColor(calibratedRed: 0.09, green: 0.20, blue: 0.55, alpha: 1),
                    NSColor(calibratedRed: 0.36, green: 0.16, blue: 0.62, alpha: 1)])!.draw(in: shape, angle: -60)

// speaker: box + cone, centred
let cx = 512.0, cy = 512.0
NSColor.white.setFill()
NSBezierPath(roundedRect: NSRect(x: cx - 120, y: cy - 62, width: 86, height: 124), xRadius: 14, yRadius: 14).fill()
let cone = NSBezierPath()
cone.move(to: NSPoint(x: cx - 46, y: cy - 62))
cone.line(to: NSPoint(x: cx + 40, y: cy - 150))
cone.line(to: NSPoint(x: cx + 40, y: cy + 150))
cone.line(to: NSPoint(x: cx - 46, y: cy + 62))
cone.close()
cone.fill()

// sound arcs: three to the right (front), three fainter to the left (the surrounds)
func arc(_ x: Double, _ r: Double, _ from: Double, _ to: Double, _ alpha: Double, _ width: Double) {
    let p = NSBezierPath()
    p.appendArc(withCenter: NSPoint(x: x, y: cy), radius: r, startAngle: from, endAngle: to)
    p.lineWidth = width
    p.lineCapStyle = .round
    NSColor.white.withAlphaComponent(alpha).setStroke()
    p.stroke()
}
for (i, r) in [110.0, 185.0, 260.0].enumerated() {
    arc(cx + 40, r, -42, 42, 1.0 - Double(i) * 0.18, 34)
    arc(cx - 120, r - 10, 150, 210, 0.42 - Double(i) * 0.1, 26)
}
NSGraphicsContext.restoreGraphicsState()

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon-1024.png"
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
print("wrote \(out)")
