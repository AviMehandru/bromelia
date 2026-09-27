// Renders the Bromelia app icon (1024x1024 PNG): a stylised bromeliad rising from an optical disc.
// Usage: swift make-icon.swift output.png
import AppKit

let size: CGFloat = 1024
let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon-1024.png"
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size), pixelsHigh: Int(size), bitsPerSample: 8,
                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext

// Background squircle.
let inset: CGFloat = 100
let rect = CGRect(x: inset, y: inset, width: size - 2 * inset, height: size - 2 * inset)
let bg = CGPath(roundedRect: rect, cornerWidth: 185, cornerHeight: 185, transform: nil)
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -12), blur: 28, color: NSColor.black.withAlphaComponent(0.35).cgColor)
ctx.addPath(bg)
ctx.setFillColor(NSColor(red: 0.08, green: 0.20, blue: 0.17, alpha: 1).cgColor)
ctx.fillPath()
ctx.restoreGState()
ctx.saveGState()
ctx.addPath(bg)
ctx.clip()
let bgGrad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                        colors: [NSColor(red: 0.07, green: 0.27, blue: 0.24, alpha: 1).cgColor,
                                 NSColor(red: 0.03, green: 0.11, blue: 0.13, alpha: 1).cgColor] as CFArray,
                        locations: [0, 1])!
ctx.drawLinearGradient(bgGrad, start: CGPoint(x: 0, y: size - inset), end: CGPoint(x: 0, y: inset), options: [])

// Disc.
let center = CGPoint(x: size / 2, y: 380)
let discR: CGFloat = 250
let discRect = CGRect(x: center.x - discR, y: center.y - discR * 0.42, width: discR * 2, height: discR * 0.84)
ctx.saveGState()
ctx.addEllipse(in: discRect)
ctx.clip()
let discGrad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                          colors: [NSColor(red: 0.85, green: 0.90, blue: 0.95, alpha: 1).cgColor,
                                   NSColor(red: 0.62, green: 0.55, blue: 0.85, alpha: 1).cgColor,
                                   NSColor(red: 0.45, green: 0.80, blue: 0.85, alpha: 1).cgColor,
                                   NSColor(red: 0.90, green: 0.85, blue: 0.70, alpha: 1).cgColor] as CFArray,
                          locations: [0, 0.35, 0.7, 1])!
ctx.drawLinearGradient(discGrad, start: CGPoint(x: discRect.minX, y: discRect.maxY), end: CGPoint(x: discRect.maxX, y: discRect.minY), options: [])
ctx.restoreGState()
ctx.setFillColor(NSColor(red: 0.05, green: 0.16, blue: 0.15, alpha: 1).cgColor)
ctx.fillEllipse(in: CGRect(x: center.x - 42, y: center.y - 18, width: 84, height: 36))

// Bromeliad leaves: a rosette of pointed leaves rising from the disc hub.
func leaf(angle: CGFloat, length: CGFloat, width: CGFloat, color: NSColor) {
    ctx.saveGState()
    ctx.translateBy(x: center.x, y: center.y + 10)
    ctx.rotate(by: angle)
    let p = CGMutablePath()
    p.move(to: .zero)
    p.addQuadCurve(to: CGPoint(x: 0, y: length), control: CGPoint(x: width, y: length * 0.45))
    p.addQuadCurve(to: .zero, control: CGPoint(x: -width, y: length * 0.45))
    ctx.addPath(p)
    ctx.setFillColor(color.cgColor)
    ctx.fillPath()
    ctx.restoreGState()
}
let greens = [NSColor(red: 0.18, green: 0.62, blue: 0.40, alpha: 1), NSColor(red: 0.30, green: 0.75, blue: 0.45, alpha: 1)]
let outer: [CGFloat] = [-1.05, 1.05, -0.75, 0.75]
for (i, a) in outer.enumerated() { leaf(angle: a, length: 360, width: 70, color: greens[i % 2]) }
let inner: [CGFloat] = [-0.42, 0.42, -0.18, 0.18]
for (i, a) in inner.enumerated() { leaf(angle: a, length: 430, width: 62, color: greens[(i + 1) % 2]) }
// Flower bract.
leaf(angle: 0, length: 470, width: 58, color: NSColor(red: 0.95, green: 0.25, blue: 0.40, alpha: 1))
leaf(angle: 0, length: 400, width: 30, color: NSColor(red: 1.0, green: 0.55, blue: 0.30, alpha: 1))
ctx.restoreGState()

NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
print("wrote \(out)")
