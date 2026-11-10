// Sentia's look for Show Control (the manual style guide's colours and type): ink page, light ink
// text, periwinkle for what's chosen, magenta only for what lights. Exo 2 for titles, Sora for
// the rest; no other typeface.

import CoreText
import SwiftUI

enum Sentia {
    static let ink = Color(red: 0x17 / 255, green: 0x1d / 255, blue: 0x3a / 255)
    static let panel = Color(red: 0x1c / 255, green: 0x23 / 255, blue: 0x45 / 255)
    static let line = Color(red: 46 / 255, green: 53 / 255, blue: 86 / 255)
    static let inkLight = Color(red: 0xe6 / 255, green: 0xe9 / 255, blue: 0xf6 / 255)
    static let ink2 = Color(red: 0x9c / 255, green: 0xa3 / 255, blue: 0xc6 / 255)
    static let periwinkle = Color(red: 0x94 / 255, green: 0xab / 255, blue: 0xf9 / 255)
    static let magenta = Color(red: 1, green: 0, blue: 1)

    // The fonts travel inside the app (Resources/Fonts).
    static func registerFonts() {
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("Fonts"),
              let files = try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil) else { return }
        for f in files { CTFontManagerRegisterFontsForURL(f as CFURL, .process, nil) }
    }
}

extension Font {
    static func exo(_ size: CGFloat) -> Font { .custom("Exo2.0-SemiBold", size: size) }
    static func sora(_ size: CGFloat) -> Font { .custom("Sora-Regular", size: size) }
    static func soraLight(_ size: CGFloat) -> Font { .custom("Sora-Light", size: size) }
}

extension RGB {
    var color: Color { Color(red: Double(r) / 255, green: Double(g) / 255, blue: Double(b) / 255) }
    init(_ c: Color) {
        let n = NSColor(c).usingColorSpace(.sRGB) ?? .white
        self.init(Int(n.redComponent * 255), Int(n.greenComponent * 255), Int(n.blueComponent * 255))
    }
}

// Small uppercase tracked label over a group of controls.
struct SectionLabel: View {
    let text: String
    var body: some View {
        Text(text.uppercased()).font(.sora(10.5)).tracking(1.6).foregroundColor(Sentia.ink2)
    }
}

// A choice among a few: an outlined pill, filled and periwinkle-edged when chosen.
struct Chip: View {
    let title: String
    let chosen: Bool
    var width: CGFloat? = nil
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            Text(title).font(.sora(12.5)).foregroundColor(chosen ? Sentia.inkLight : Sentia.ink2)
                .padding(.horizontal, 16).frame(width: width, height: 32)
                .background(Capsule().fill(chosen ? Sentia.panel : Color.clear))
                .overlay(Capsule().stroke(chosen ? Sentia.periwinkle : Sentia.line, lineWidth: 1.2))
                .contentShape(Capsule())
        }.buttonStyle(.plain)
    }
}

// A row in a list (presets, styles): plain text, a soft panel behind the chosen one.
struct ListRow<Trailing: View>: View {
    let title: String
    let chosen: Bool
    @ViewBuilder var trailing: () -> Trailing
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack {
                Text(title).font(.sora(13)).foregroundColor(chosen ? Sentia.inkLight : Sentia.ink2)
                Spacer()
                trailing()
            }
            .padding(.horizontal, 10).frame(height: 34)
            .background(RoundedRectangle(cornerRadius: 10).fill(chosen ? Sentia.panel : Color.clear))
            .contentShape(Rectangle())
        }.buttonStyle(.plain)
    }
}

extension ListRow where Trailing == EmptyView {
    init(title: String, chosen: Bool, action: @escaping () -> Void) {
        self.init(title: title, chosen: chosen, trailing: { EmptyView() }, action: action)
    }
}

// A hairline track with one or two round thumbs, in whole steps.
struct SentiaSlider: View {
    let label: String
    @Binding var low: Int
    @Binding var high: Int
    let bounds: ClosedRange<Int>
    let range: Bool  // two thumbs (low, high) or one (high)
    var unit = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                SectionLabel(text: label)
                Spacer()
                Text(range ? "\(low) – \(high)\(unit)" : "\(high)\(unit)").font(.sora(13)).foregroundColor(Sentia.inkLight)
            }
            GeometryReader { geo in
                let w = geo.size.width
                let span = CGFloat(bounds.upperBound - bounds.lowerBound)
                let x = { (v: Int) in CGFloat(v - bounds.lowerBound) / span * w }
                let value = { (px: CGFloat) in bounds.lowerBound + Int((max(0, min(w, px)) / w * span).rounded()) }
                ZStack(alignment: .leading) {
                    Rectangle().fill(Sentia.line).frame(height: 2)
                    Rectangle().fill(Sentia.periwinkle).frame(width: x(high) - (range ? x(low) : 0), height: 2)
                        .offset(x: range ? x(low) : 0)
                    if range { thumb.offset(x: x(low) - 7) }
                    thumb.offset(x: x(high) - 7)
                }
                .frame(height: 16)
                .contentShape(Rectangle())
                .gesture(DragGesture(minimumDistance: 0).onChanged { g in
                    let v = value(g.location.x)
                    if range && abs(g.location.x - x(low)) < abs(g.location.x - x(high)) { low = min(v, high) }
                    else { high = max(v, range ? low : bounds.lowerBound) }
                })
            }.frame(height: 16)
        }
    }

    private var thumb: some View {
        Circle().fill(Sentia.ink).frame(width: 14, height: 14).overlay(Circle().stroke(Sentia.periwinkle, lineWidth: 1.6))
    }
}

// A switch in Sentia colours.
struct SentiaToggle: View {
    let title: String
    @Binding var on: Bool
    var body: some View {
        Button { on.toggle() } label: {
            HStack(spacing: 10) {
                ZStack(alignment: on ? .trailing : .leading) {
                    Capsule().fill(on ? Sentia.panel : Color.clear).overlay(Capsule().stroke(on ? Sentia.periwinkle : Sentia.line, lineWidth: 1.2))
                        .frame(width: 34, height: 20)
                    Circle().fill(on ? Sentia.periwinkle : Sentia.ink2).frame(width: 12, height: 12).padding(.horizontal, 4)
                }
                Text(title).font(.sora(12.5)).foregroundColor(on ? Sentia.inkLight : Sentia.ink2)
            }.contentShape(Rectangle())
        }.buttonStyle(.plain)
    }
}
