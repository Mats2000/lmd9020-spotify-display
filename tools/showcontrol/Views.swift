// The window: a sidebar of pages (Display, Lights, Scenes, then TILES), each a calm Sentia page.

import AppKit
import SwiftUI

struct RootView: View {
    @ObservedObject var show: Show
    var body: some View {
        HStack(spacing: 0) {
            Sidebar(show: show).frame(width: 220)
            Rectangle().fill(Sentia.line).frame(width: 1)
            Group {
                switch show.page {
                case .display: DisplayPage(show: show)
                case .lights: LightsPage(show: show)
                case .scenes: ScenesPage(show: show)
                case .tiles: TilesPage(show: show)
                case .autopilot: AutopilotPage(show: show)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .background(Sentia.ink)
        .foregroundColor(Sentia.inkLight)
    }
}

struct Sidebar: View {
    @ObservedObject var show: Show
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Show Control").font(.exo(20))
                Text("by Sentia").font(.soraLight(12)).foregroundColor(Sentia.ink2)
            }
            .padding(.top, 44).padding(.bottom, 32)
            ForEach(Show.Page.allCases, id: \.self) { p in
                let soon = false
                ListRow(title: p.rawValue, chosen: show.page == p, trailing: {
                    if soon { Text("soon").font(.sora(10.5)).foregroundColor(Sentia.ink2.opacity(0.7)) }
                }) { show.page = p }
            }
            Spacer()
            VStack(alignment: .leading, spacing: 6) {
                SectionLabel(text: "Now playing")
                Text(show.title.isEmpty ? "—" : show.title).font(.sora(13)).lineLimit(2)
                Text(show.artist).font(.soraLight(12)).foregroundColor(Sentia.ink2).lineLimit(1)
                HStack(spacing: 8) {
                    Circle().fill(show.status == "Listening to Spotify" ? Sentia.periwinkle : Sentia.line).frame(width: 6, height: 6)
                    Text(show.status).font(.soraLight(11)).foregroundColor(Sentia.ink2)
                }.padding(.top, 6)
            }
            .padding(.bottom, 24)
        }
        .padding(.horizontal, 18)
    }
}

struct PageHeader: View {
    let title: String
    var body: some View {
        Text(title).font(.exo(28)).padding(.top, 40).padding(.bottom, 28)
    }
}

// MARK: - Display

struct DisplayPage: View {
    @ObservedObject var show: Show
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                PageHeader(title: "Display")
                HStack(alignment: .top, spacing: 40) {
                    VStack(alignment: .leading, spacing: 14) {
                        ZStack {
                            RoundedRectangle(cornerRadius: 14).fill(Color.black)
                            if let image = show.preview {
                                Image(nsImage: image).resizable().interpolation(.medium)
                            }
                        }
                        .frame(width: 512, height: 384)
                        .clipShape(RoundedRectangle(cornerRadius: 14))
                        .overlay(RoundedRectangle(cornerRadius: 14).stroke(Sentia.line, lineWidth: 1))
                        Text(displayLine).font(.soraLight(12)).foregroundColor(Sentia.ink2)
                            .frame(width: 512, alignment: .leading).fixedSize(horizontal: false, vertical: true)
                        if show.needsPermission {
                            Text("Allow Show Control under Privacy & Security → Screen & System Audio Recording.")
                                .font(.sora(12)).foregroundColor(Sentia.inkLight)
                        }
                    }
                    VStack(alignment: .leading, spacing: 28) {
                        VStack(alignment: .leading, spacing: 4) {
                            SectionLabel(text: "Visualizer").padding(.bottom, 6)
                            ForEach(["Shuffle"] + styles + ["Off"], id: \.self) { s in
                                ListRow(title: s, chosen: show.styleName == s) { show.styleName = s }
                            }
                        }
                        VStack(alignment: .leading, spacing: 4) {
                            SectionLabel(text: "Colours").padding(.bottom, 6)
                            ForEach(Array(schemes.enumerated()), id: \.offset) { i, s in
                                ListRow(title: s, chosen: show.schemeName == s, trailing: { SchemeDots(index: i) }) { show.schemeName = s }
                            }
                        }
                    }
                    .frame(width: 260)
                }
                Rectangle().fill(Sentia.line).frame(height: 1).padding(.vertical, 32)
                HStack(alignment: .top, spacing: 48) {
                    VStack(alignment: .leading, spacing: 10) {
                        SentiaSlider(label: "In time with the speakers", low: .constant(-100), high: $show.trimMs,
                                     bounds: -100...400, range: false, unit: " ms")
                        Text("The speakers add \(show.speakerMs) ms; everything is held back to match, plus this.")
                            .font(.soraLight(11.5)).foregroundColor(Sentia.ink2)
                    }
                    .frame(width: 420)
                    SentiaToggle(title: "Open at login", on: Binding(get: { show.openAtLogin }, set: { show.setOpenAtLogin($0) }))
                        .padding(.top, 2)
                }
                .padding(.bottom, 40)
            }
            .padding(.horizontal, 40)
        }
    }
}

extension DisplayPage {
    // The display announces itself every 2 s; not hearing it means it's off or off the network, or
    // macOS is keeping the app off the network.
    var displayLine: String {
        guard let ip = show.displayAddress else {
            return "Looking for the display… If it's on and on the Wi-Fi, allow Show Control under System Settings → Privacy & Security → Local Network."
        }
        if show.displayHeard { return "LMD-9020 at \(ip)" }
        return "Not hearing the LMD-9020 (last at \(ip); still sending there). Check it's on, or allow Show Control under System Settings → Privacy & Security → Local Network."
    }
}

struct SchemeDots: View {
    let index: Int
    var body: some View {
        HStack(spacing: 4) {
            if index == 0 {
                Text("from the cover").font(.soraLight(10.5)).foregroundColor(Sentia.ink2)
            } else {
                ForEach(Array(schemeLampColours[index - 1].enumerated()), id: \.offset) { _, c in
                    Circle().fill(c.color).frame(width: 9, height: 9)
                }
            }
        }
    }
}

// MARK: - Lights

// Every light on its own: pick one, then its switch, brightness, kick and its own look. Presets
// are looks saved to reuse: one click copies a preset into the open light.
struct LightsPage: View {
    @ObservedObject var show: Show

    private func bind<T>(_ kp: WritableKeyPath<LampLook, T>) -> Binding<T> {
        Binding(get: { show.look[keyPath: kp] }, set: { var l = show.look; l[keyPath: kp] = $0; show.look = l })
    }
    private func setting(_ kp: WritableKeyPath<FixtureSettings, Int>) -> Binding<Int> {
        Binding(get: { show.light?.settings[keyPath: kp] ?? 0 },
                set: { v in if let id = show.light?.id { show.update(id) { $0.settings[keyPath: kp] = v } } })
    }

    static let movementNames: [LampLook.Movement: String] = [
        .flow: "Flow", .twinkle: "Twinkle", .flexing: "Flexing", .stacking: "Stacking",
        .strike: "Strike", .gradient: "Gradient", .chase: "Chase", .pulse: "Pulse",
    ]
    static let sourceNames: [LampLook.Source: String] = [.own: "These colours", .cover: "Cover", .scheme: "Display colours"]
    static let directionNames: [LampLook.Direction: String] = [
        .up: "Up", .down: "Down", .inward: "Inward", .outward: "Outward", .random: "Random",
    ]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                HStack(alignment: .lastTextBaseline) {
                    PageHeader(title: "Lights")
                    Spacer()
                    SentiaToggle(title: show.lampsOn ? "Lights follow the music" : "Lights off", on: $show.lampsOn)
                }
                if show.fixtures.isEmpty {
                    Text("Looking for lights…").font(.soraLight(12)).foregroundColor(Sentia.ink2)
                } else {
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: 12) {
                            ForEach(show.fixtures) { f in LightTab(show: show, fixture: f, chosen: f.id == show.light?.id) }
                        }
                    }
                    .padding(.bottom, 32)
                    if let f = show.light { panel(f) }
                }
                Text(timingText).font(.soraLight(11.5)).foregroundColor(Sentia.ink2).padding(.top, 36).padding(.bottom, 40)
            }
            .padding(.horizontal, 40)
        }
    }

    private var timingText: String {
        let t = show.timing
        var text = "The music reaches the app \(t.captureMs) ms after Spotify plays it, in pieces of \(t.bufferMs) ms; the speakers add \(show.speakerMs) ms."
        text += " Kicks reach the lamp \(t.lampMs) ms after you hear them."
        guard show.fixtures.contains(where: { $0.kind == .matter }) else { return text }
        if t.ahead { return text + " The kicks repeat (\(t.hitPercent)% guessed right), so the bulb gets them ahead, to land as you hear them." }
        return text + " The bulb \(t.bulbMs) ms; it gets them ahead once 70% of the pattern is guessed right (now \(t.hitPercent)%)."
    }

    @ViewBuilder private func panel(_ f: Fixture) -> some View {
        HStack(alignment: .top, spacing: 40) {
            if f.kind == .govee { LampDrawing(segments: show.previewSegments) } else { BulbDrawing(colour: show.previewSegments.first ?? .black) }
            VStack(alignment: .leading, spacing: 30) {
                HStack(spacing: 16) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(f.name).font(.sora(16))
                        Text(f.kind == .govee ? "Govee · \(f.segments) segments · \(f.address)" : "Matter light").font(.soraLight(11)).foregroundColor(Sentia.ink2)
                    }
                    Spacer()
                    if f.held {
                        Chip(title: "Take back", chosen: false) { show.takeBack(f.id) }
                    } else {
                        Chip(title: "Hand to Alexa", chosen: false) { show.hold(f.id) }
                        SentiaToggle(title: "", on: Binding(get: { f.settings.on }, set: { v in show.update(f.id) { $0.settings.on = v } }))
                    }
                }
                if f.held {
                    Text("Alexa has it. The show leaves it alone until you take it back.").font(.soraLight(12.5)).foregroundColor(Sentia.ink2)
                } else {
                    if f.kind == .matter { bulbStyle(f) }
                    VStack(alignment: .leading, spacing: 26) {
                        SentiaSlider(label: "Brightness", low: .constant(0), high: setting(\.brightness), bounds: 0...100, range: false, unit: "%")
                        if f.kind != .matter || f.settings.bulbStyle != .wash {
                            SentiaSlider(label: "Kick", low: .constant(0), high: setting(\.kick), bounds: 0...150, range: false, unit: "%")
                        }
                    }
                    lookControls(f)
                }
            }
            .frame(maxWidth: 520)
            presets(f).frame(width: 230)
        }
    }

    static let bulbStyles: [(BulbStyle, String, String)] = [
        (.strobe, "Strobe", "A hard flash on each kick, straight off between them. Best on sparse beats."),
        (.pulse, "Pulse", "Up on each kick, fading back down through the beat: the bulb's slow fade as the shape."),
        (.wash, "Wash", "No kicks: brightness with the music's energy, colour along the look's colours with how bright or bassy it sounds, in long fades."),
    ]

    private func bulbStyle(_ f: Fixture) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionLabel(text: "Style")
            HStack(spacing: 8) {
                ForEach(Self.bulbStyles, id: \.0) { s in
                    Chip(title: s.1, chosen: f.settings.bulbStyle == s.0) { show.update(f.id) { $0.settings.bulb = s.0 } }
                }
            }
            Text(Self.bulbStyles.first { $0.0 == f.settings.bulbStyle }?.2 ?? "").font(.soraLight(11.5)).foregroundColor(Sentia.ink2)
        }
    }

    @ViewBuilder private func lookControls(_ f: Fixture) -> some View {
        let strip = f.kind == .govee
        if strip {
            VStack(alignment: .leading, spacing: 12) {
                SectionLabel(text: "Movement")
                LazyVGrid(columns: Array(repeating: GridItem(.fixed(118), spacing: 8), count: 4), alignment: .leading, spacing: 8) {
                    ForEach(LampLook.Movement.allCases, id: \.self) { m in
                        Chip(title: Self.movementNames[m]!, chosen: show.look.movement == m, width: 118) { bind(\.movement).wrappedValue = m }
                    }
                }
            }
        }
        VStack(alignment: .leading, spacing: 12) {
            SectionLabel(text: "Colours")
            HStack(spacing: 8) {
                ForEach(LampLook.Source.allCases, id: \.self) { s in
                    Chip(title: Self.sourceNames[s]!, chosen: show.look.source == s) { bind(\.source).wrappedValue = s }
                }
            }
            Swatches(colours: bind(\.colours)) { if show.look.source != .own { bind(\.source).wrappedValue = .own } }
                .opacity(show.look.source == .own ? 1 : 0.55)
            HStack(spacing: 8) {
                Chip(title: "In order", chosen: !show.look.random) { bind(\.random).wrappedValue = false }
                Chip(title: "Random", chosen: show.look.random) { bind(\.random).wrappedValue = true }
            }
            HStack(spacing: 8) {
                Chip(title: "With the beat", chosen: !show.look.bySong) { bind(\.followsSong).wrappedValue = false }
                Chip(title: "With the song", chosen: show.look.bySong) { bind(\.followsSong).wrappedValue = true }
            }
            if show.look.bySong {
                Text("One colour at a time; the next when the song changes character (a drop, a breakdown, a new part). Changes so far: \(show.timing.songSteps).")
                    .font(.soraLight(11.5)).foregroundColor(Sentia.ink2)
            }
        }
        VStack(alignment: .leading, spacing: 26) {
            if strip {
                SentiaSlider(label: "Spots", low: bind(\.spotsLow), high: bind(\.spotsHigh), bounds: 1...8, range: true)
                SentiaSlider(label: "Size", low: bind(\.sizeLow), high: bind(\.sizeHigh), bounds: 1...10, range: true)
            }
            SentiaSlider(label: strip ? "Speed" : "Colour changes", low: .constant(1), high: bind(\.speed), bounds: 1...5, range: false)
            SentiaSlider(label: "Rhythm", low: bind(\.rhythmLow), high: bind(\.rhythmHigh), bounds: 0...100, range: true, unit: "%")
        }
        if strip {
            VStack(alignment: .leading, spacing: 12) {
                SectionLabel(text: "Direction")
                HStack(spacing: 8) {
                    ForEach(LampLook.Direction.allCases, id: \.self) { d in
                        Chip(title: Self.directionNames[d]!, chosen: show.look.direction == d) { bind(\.direction).wrappedValue = d }
                    }
                }
            }
        }
    }

    private func presets(_ f: Fixture) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            SectionLabel(text: "Presets").padding(.bottom, 6)
            let current = show.preset(of: f.settings.look)
            ForEach(show.looks) { l in
                ListRow(title: l.name, chosen: l.id == current, trailing: {
                    if show.looks.count > 1 {
                        Button { show.deletePreset(l.id) } label: {
                            Image(systemName: "xmark").font(.system(size: 10, weight: .light)).foregroundColor(Sentia.ink2)
                        }.buttonStyle(.plain)
                    }
                }) { show.loadPreset(l.id) }
            }
            TextField("Look name", text: bind(\.name))
                .textFieldStyle(.plain).font(.sora(13))
                .padding(.horizontal, 10).frame(height: 32)
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(Sentia.line, lineWidth: 1))
                .padding(.top, 14)
            Chip(title: "Save as a preset", chosen: false) { show.savePreset() }.padding(.top, 8)
            Chip(title: "Same on every light", chosen: false) { show.sameOnAll() }.padding(.top, 8)
        }
    }
}

// A light in the row at the top: its name, its colours now, and whether it's in the show.
struct LightTab: View {
    @ObservedObject var show: Show
    let fixture: Fixture
    let chosen: Bool
    var body: some View {
        Button { show.selected = fixture.id } label: {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text(fixture.name).font(.sora(13)).lineLimit(1)
                    Spacer()
                    Circle().fill(fixture.held ? Sentia.line : (fixture.settings.on ? Sentia.periwinkle : Sentia.line)).frame(width: 6, height: 6)
                }
                HStack(spacing: 2) {
                    ForEach(Array((show.live[fixture.id] ?? [.black]).enumerated()), id: \.offset) { _, c in
                        RoundedRectangle(cornerRadius: 2).fill(c.color).frame(height: 10)
                    }
                }
                Text(fixture.held ? "Alexa has it" : (fixture.settings.on ? fixture.settings.look.name : "Off"))
                    .font(.soraLight(11)).foregroundColor(Sentia.ink2).lineLimit(1)
            }
            .padding(14)
            .frame(width: 200)
            .background(RoundedRectangle(cornerRadius: 16).fill(chosen ? Sentia.panel : Color.clear))
            .overlay(RoundedRectangle(cornerRadius: 16).stroke(chosen ? Sentia.periwinkle : Sentia.line, lineWidth: chosen ? 1.2 : 1))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

// A single bulb, as it looks now.
struct BulbDrawing: View {
    let colour: RGB
    var body: some View {
        Circle().fill(colour.color)
            .shadow(color: colour.color.opacity(Double(colour.level) / 255 * 0.8), radius: 18)
            .frame(width: 72, height: 72)
            .padding(18)
            .background(RoundedRectangle(cornerRadius: 22).fill(Color.black.opacity(0.35)))
            .overlay(RoundedRectangle(cornerRadius: 22).stroke(Sentia.line, lineWidth: 1))
    }
}

// MARK: - Scenes

// The whole room saved under a name (each light's look and settings, the display's style), and
// the phone page.
struct ScenesPage: View {
    @ObservedObject var show: Show
    @State private var sceneName = ""
    @State private var password = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                PageHeader(title: "Scenes")
                HStack(alignment: .top, spacing: 56) {
                    VStack(alignment: .leading, spacing: 4) {
                        SectionLabel(text: "Scenes").padding(.bottom, 6)
                        if show.scenes.isEmpty {
                            Text("None yet. Set the lights up, then save the room here.").font(.soraLight(12)).foregroundColor(Sentia.ink2)
                        }
                        ForEach(show.scenes) { sc in
                            ListRow(title: sc.name, chosen: false, trailing: {
                                Button { show.deleteScene(sc.id) } label: {
                                    Image(systemName: "xmark").font(.system(size: 10, weight: .light)).foregroundColor(Sentia.ink2)
                                }.buttonStyle(.plain)
                            }) { show.recall(sc.id) }
                        }
                        TextField("Scene name", text: $sceneName)
                            .textFieldStyle(.plain).font(.sora(13))
                            .padding(.horizontal, 10).frame(height: 32)
                            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Sentia.line, lineWidth: 1))
                            .padding(.top, 14)
                        Chip(title: "Save this as a scene", chosen: false) {
                            show.saveScene(sceneName)
                            sceneName = ""
                        }.padding(.top, 8)
                    }
                    .frame(width: 300)
                    VStack(alignment: .leading, spacing: 4) {
                        SectionLabel(text: "On your phone").padding(.bottom, 8)
                        Text(show.publicAddress ?? "Not on the web yet").font(.sora(13)).textSelection(.enabled)
                        Text("At home also \(show.webAddress ?? "—")").font(.soraLight(12)).foregroundColor(Sentia.ink2).textSelection(.enabled).padding(.top, 2)
                        Text(show.hasPassword ? "Phones sign in with the password set here." : "Set a password before a phone can sign in.")
                            .font(.soraLight(11.5)).foregroundColor(Sentia.ink2).padding(.top, 10)
                        SecureField(show.hasPassword ? "New password" : "Password", text: $password)
                            .textFieldStyle(.plain).font(.sora(13))
                            .padding(.horizontal, 10).frame(height: 32)
                            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Sentia.line, lineWidth: 1))
                            .padding(.top, 10)
                        Text("At least 10 characters.").font(.soraLight(11)).foregroundColor(Sentia.ink2).padding(.top, 4)
                        HStack(spacing: 8) {
                            Chip(title: "Set password", chosen: false) {
                                if show.setPassword(password) { password = "" }
                            }
                            Chip(title: "Sign out every phone", chosen: false) { show.signOutPhones() }
                        }.padding(.top, 8)
                    }
                    .frame(width: 320)
                }
                .padding(.bottom, 40)
            }
            .padding(.horizontal, 40)
        }
    }
}

// The lamp as it looks now: its segments stacked, the first at the bottom.
struct LampDrawing: View {
    let segments: [RGB]
    var body: some View {
        VStack(spacing: 3) {
            ForEach(Array(segments.enumerated().reversed()), id: \.offset) { _, c in
                RoundedRectangle(cornerRadius: 3).fill(c.color)
                    .shadow(color: c.color.opacity(Double(c.level) / 255 * 0.8), radius: 8)
                    .frame(width: 30, height: 30)
            }
        }
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 22).fill(Color.black.opacity(0.35)))
        .overlay(RoundedRectangle(cornerRadius: 22).stroke(Sentia.line, lineWidth: 1))
    }
}

// The look's own colours: click one to change it, its x (on hover) to remove it, + to add (up to six).
// The look's own colours: click one to change it in the colour panel (it keeps changing as you
// drag), its × (on hover) to remove it, + to add one (up to six) and change it straight away.
struct Swatches: View {
    @Binding var colours: [RGB]
    var fixed = false  // a set number of colours: no adding or removing
    var edited: () -> Void = {}  // a colour was chosen: the look uses its own colours from now on
    @State private var hovered: Int?
    @State private var editing: Int?

    private func open(_ i: Int) {
        editing = i
        ColourPanel.shared.open(colours[i]) { c in
            guard i < colours.count else { return }
            colours[i] = c
            edited()
        }
    }

    var body: some View {
        HStack(spacing: 10) {
            ForEach(colours.indices, id: \.self) { i in
                Button { open(i) } label: {
                    Circle().fill(colours[i].color).frame(width: 26, height: 26)
                        .overlay(Circle().stroke(editing == i ? Sentia.periwinkle : Sentia.line, lineWidth: editing == i ? 2 : 1))
                        .padding(2)
                }
                .buttonStyle(.plain)
                .overlay(alignment: .topTrailing) {
                    if !fixed && colours.count > 1 && hovered == i {
                        Button {
                            if editing == i { ColourPanel.shared.close(); editing = nil }
                            colours.remove(at: i)
                            edited()
                        } label: {
                            Image(systemName: "xmark").font(.system(size: 7, weight: .semibold)).foregroundColor(Sentia.ink2)
                                .frame(width: 14, height: 14).background(Circle().fill(Sentia.ink)).overlay(Circle().stroke(Sentia.line, lineWidth: 1))
                        }.buttonStyle(.plain).offset(x: 4, y: -4)
                    }
                }
                .onHover { hovered = $0 ? i : (hovered == i ? nil : hovered) }
                .contextMenu { if !fixed && colours.count > 1 { Button("Remove") { colours.remove(at: i); edited() } } }
            }
            if !fixed && colours.count < 6 {
                Button {
                    colours.append((colours.last ?? RGB(255, 0, 255)).turned(by: 60))
                    edited()
                    open(colours.count - 1)
                } label: {
                    Image(systemName: "plus").font(.system(size: 11, weight: .light)).foregroundColor(Sentia.ink2)
                        .frame(width: 26, height: 26).overlay(Circle().stroke(Sentia.line, lineWidth: 1)).padding(2)
                }.buttonStyle(.plain)
            }
        }
        .onDisappear { ColourPanel.shared.close() }
    }
}

// The system colour panel, aimed at one colour at a time.
final class ColourPanel: NSObject {
    static let shared = ColourPanel()
    private var change: ((RGB) -> Void)?

    func open(_ c: RGB, _ change: @escaping (RGB) -> Void) {
        let p = NSColorPanel.shared
        self.change = nil  // setting the panel's colour below mustn't count as a choice
        p.showsAlpha = false
        p.isContinuous = true
        p.color = NSColor(srgbRed: CGFloat(c.r) / 255, green: CGFloat(c.g) / 255, blue: CGFloat(c.b) / 255, alpha: 1)
        p.setTarget(self)
        p.setAction(#selector(changed(_:)))
        self.change = change
        p.orderFront(nil)
    }

    func close() {
        change = nil
        NSColorPanel.shared.orderOut(nil)
    }

    @objc private func changed(_ p: NSColorPanel) {
        let n = p.color.usingColorSpace(.sRGB) ?? .white
        change?(RGB(Int(n.redComponent * 255), Int(n.greenComponent * 255), Int(n.blueComponent * 255)))
    }
}

extension RGB {
    // The same colour with its hue turned, for a new swatch that isn't a copy of the last.
    func turned(by degrees: Double) -> RGB {
        let r = Double(self.r) / 255, g = Double(self.g) / 255, b = Double(self.b) / 255
        let mx = max(r, g, b), mn = min(r, g, b), c = mx - mn
        var h = 0.0
        if c > 0 { h = mx == r ? ((g - b) / c).truncatingRemainder(dividingBy: 6) : mx == g ? (b - r) / c + 2 : (r - g) / c + 4 }
        h = (h * 60 + degrees + 720).truncatingRemainder(dividingBy: 360)
        return RGB(hue: h, saturation: mx > 0 && c > 0 ? c / mx : 1, value: max(mx, 0.6))
    }
}

// MARK: - Autopilot

// A show director: picks the whole rig's look from the music as it goes (Autopilot.swift).
struct AutopilotPage: View {
    @ObservedObject var show: Show

    static let styles: [(Autopilot.Style?, String, String)] = [
        (nil, "Off", "Every light, the display and TILES as you set them."),
        (.club, "Club", "Follows the song's arc: washes and flows in the breakdowns, grooves in the body, strobes and strikes at the peak, a hard switch on a drop. A new look about every 32 bars or at a new section."),
        (.lounge, "Lounge", "Slow and evolving: gradients, breathing and washes, the calm visualizers, changing gently every minute or two. Never strobes."),
        (.rave, "Rave", "Maximal: strikes, chases and sparkles, the bulb strobing, the colour schemes turning over, a new look every 8 bars."),
    ]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                PageHeader(title: "Autopilot")
                HStack(alignment: .top, spacing: 56) {
                    VStack(alignment: .leading, spacing: 10) {
                        ForEach(Self.styles, id: \.1) { s in
                            Button { show.autopilot = s.0 } label: {
                                VStack(alignment: .leading, spacing: 6) {
                                    Text(s.1).font(.sora(15))
                                    Text(s.2).font(.soraLight(12)).foregroundColor(Sentia.ink2).fixedSize(horizontal: false, vertical: true)
                                }
                                .padding(16)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .background(RoundedRectangle(cornerRadius: 16).fill(show.autopilot == s.0 ? Sentia.panel : Color.clear))
                                .overlay(RoundedRectangle(cornerRadius: 16).stroke(show.autopilot == s.0 ? Sentia.periwinkle : Sentia.line, lineWidth: show.autopilot == s.0 ? 1.2 : 1))
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .frame(width: 420)
                    VStack(alignment: .leading, spacing: 14) {
                        SectionLabel(text: "Now")
                        if show.autopilot == nil {
                            Text("Off.").font(.soraLight(13)).foregroundColor(Sentia.ink2)
                        } else if let r = show.autoRecipe {
                            Text(r.name).font(.exo(22))
                            Text("The music: \(show.autoTier.rawValue)\(show.timing.playing ? "" : " (paused)")").font(.sora(13))
                            meter
                            VStack(alignment: .leading, spacing: 4) {
                                line("Lamps", "\(r.movement.rawValue.capitalized), speed \(r.speed), \(r.source == .cover ? "the cover's colours" : r.source == .scheme ? "the display's colours" : "own colours")")
                                line("Bulb", r.bulb.rawValue.capitalized)
                                line("Display", "\(r.display), \(r.scheme)")
                                line("TILES", show.tiles.on ? "\(r.tiles)".capitalized : "not taken over (the TILES page)")
                            }
                            Text("Drops caught: \(show.pilot.drops)").font(.soraLight(11.5)).foregroundColor(Sentia.ink2)
                        } else {
                            Text("Waiting for the music.").font(.soraLight(13)).foregroundColor(Sentia.ink2)
                        }
                        Text("It lies over your settings and saves nothing: off, everything is as you set it.")
                            .font(.soraLight(11.5)).foregroundColor(Sentia.ink2).padding(.top, 18)
                    }
                    .frame(width: 340, alignment: .leading)
                }
                .padding(.bottom, 40)
            }
            .padding(.horizontal, 40)
        }
    }

    private var meter: some View {
        GeometryReader { g in
            ZStack(alignment: .leading) {
                Capsule().fill(Sentia.line).frame(height: 3)
                Capsule().fill(Sentia.periwinkle).frame(width: g.size.width * min(1, show.timing.energy * 1.4), height: 3)
            }
        }
        .frame(height: 6)
    }

    private func line(_ label: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text(label).font(.sora(12)).foregroundColor(Sentia.ink2).frame(width: 64, alignment: .leading)
            Text(value).font(.sora(12.5))
        }
    }
}

// MARK: - TILES

// TILES' LEDs as part of the show: while on, its underglow strobes on the kicks and the pads play
// an animation from the music (TilesLink, and the firmware's show link).
struct TilesPage: View {
    @ObservedObject var show: Show

    static let animations: [(TilesLink.Animation, String, String)] = [
        (.meter, "Meter", "Each column a level bar from its part of the sound, bass on the left, like the TILES screensaver."),
        (.spectrum, "Spectrum", "The meter's bars coloured across the grid, main colour to second, peaks in the accent."),
        (.pulse, "Pulse", "The whole grid flashes the main colour on each kick, over a dim bed of the second."),
        (.ripple, "Ripple", "A ring spreads from the centre on each kick."),
        (.rain, "Rain", "Drops fall from the top row: kicks in the main colour, snares and hats in the accent."),
        (.chase, "Chase", "A lit column steps across the grid on each kick, its trail fading."),
        (.sparkle, "Sparkle", "Pads spark in the accent colour on snares and hats; kicks wash the grid in the main colour."),
    ]
    static let glows: [(TilesLink.Glow, String)] = [(.white, "White"), (.main, "Main"), (.second, "Second"), (.off, "Off")]

    private func set(_ change: (inout TilesLink.Mode) -> Void) {
        var t = show.tiles
        change(&t)
        show.tiles = t
    }
    private func int(_ kp: WritableKeyPath<TilesLink.Mode, Int>) -> Binding<Int> {
        Binding(get: { show.tiles[keyPath: kp] }, set: { v in set { $0[keyPath: kp] = v } })
    }
    private var colours: Binding<[RGB]> {
        Binding(get: { [show.tiles.main, show.tiles.second, show.tiles.accent] },
                set: { c in set { t in
                    if c.count > 0 { t.main = c[0] }
                    if c.count > 1 { t.second = c[1] }
                    if c.count > 2 { t.accent = c[2] }
                } })
    }

    var body: some View {
        let t = show.tiles, a = t.animation
        let meterLike = a == .meter || a == .spectrum, moving = a == .ripple || a == .rain || a == .chase
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                HStack(alignment: .lastTextBaseline) {
                    PageHeader(title: "TILES")
                    Spacer()
                    SentiaToggle(title: t.on ? "TILES' lights are the show's" : "Take over TILES' lights",
                                 on: Binding(get: { show.tiles.on }, set: { v in set { $0.on = v } }))
                }
                Text(show.tilesName.map { "\($0) connected." } ?? "No TILES connected: plug one in by USB.")
                    .font(.soraLight(12)).foregroundColor(Sentia.ink2).padding(.bottom, 28)
                HStack(alignment: .top, spacing: 48) {
                    VStack(alignment: .leading, spacing: 30) {
                        VStack(alignment: .leading, spacing: 12) {
                            SectionLabel(text: "Pads")
                            LazyVGrid(columns: Array(repeating: GridItem(.fixed(112), spacing: 8), count: 4), alignment: .leading, spacing: 8) {
                                ForEach(Self.animations, id: \.0) { an in
                                    Chip(title: an.1, chosen: a == an.0, width: 112) { set { $0.animation = an.0 } }
                                }
                            }
                            Text(Self.animations.first { $0.0 == a }?.2 ?? "").font(.soraLight(11.5)).foregroundColor(Sentia.ink2)
                        }
                        VStack(alignment: .leading, spacing: 26) {
                            SentiaSlider(label: "Brightness", low: .constant(0), high: int(\.brightness), bounds: 0...100, range: false, unit: "%")
                            SentiaSlider(label: "Tail", low: .constant(0), high: int(\.tail), bounds: 0...100, range: false)
                            SentiaSlider(label: "Background", low: .constant(0), high: int(\.bed), bounds: 0...100, range: false)
                            if meterLike {
                                SentiaSlider(label: "Sensitivity", low: .constant(0), high: int(\.sensitivity), bounds: 0...100, range: false)
                            }
                            if moving {
                                SentiaSlider(label: "Speed", low: .constant(0), high: int(\.speed), bounds: 0...100, range: false)
                            }
                        }
                        if a == .meter {
                            HStack(spacing: 8) {
                                Chip(title: "White, yellow, red", chosen: !t.meterColours) { set { $0.meterColours = false } }
                                Chip(title: "My colours", chosen: t.meterColours) { set { $0.meterColours = true } }
                            }
                        }
                    }
                    .frame(maxWidth: 500, alignment: .leading)
                    VStack(alignment: .leading, spacing: 30) {
                        VStack(alignment: .leading, spacing: 12) {
                            SectionLabel(text: "Colours")
                            Swatches(colours: colours, fixed: true)
                            Text("Main, second, accent.").font(.soraLight(11.5)).foregroundColor(Sentia.ink2)
                        }
                        VStack(alignment: .leading, spacing: 12) {
                            SectionLabel(text: "Underglow")
                            HStack(spacing: 8) {
                                ForEach(Self.glows, id: \.0) { g in
                                    Chip(title: g.1, chosen: t.glow == g.0) { set { $0.glow = g.0 } }
                                }
                            }
                            if t.glow != .off {
                                SentiaSlider(label: "Strobe length", low: .constant(0), high: int(\.strobe), bounds: 0...100, range: false)
                            }
                            Text(t.glow == .white ? "A white strobe on each kick, falling toward the main colour." : t.glow == .off ? "Dark." : "A strobe in that colour on each kick.")
                                .font(.soraLight(11.5)).foregroundColor(Sentia.ink2)
                        }
                    }
                    .frame(width: 360, alignment: .leading)
                }
                Text("Only while the music plays. TILES goes back to its own lights two seconds after the show stops; its pads still play notes underneath.")
                    .font(.soraLight(11.5)).foregroundColor(Sentia.ink2).frame(maxWidth: 520, alignment: .leading).padding(.top, 36).padding(.bottom, 40)
            }
            .padding(.horizontal, 40)
        }
    }
}

struct SoonPage: View {
    let title: String, text: String
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            PageHeader(title: title)
            Text(text).font(.soraLight(14)).foregroundColor(Sentia.ink2).frame(maxWidth: 420, alignment: .leading)
        }
        .padding(.horizontal, 40)
    }
}
