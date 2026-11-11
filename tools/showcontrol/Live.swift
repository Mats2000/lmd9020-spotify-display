// Live: the one page to run a show from, laid out like a lighting desk. The music across the top,
// the desk on the left (master, Blackout, Flash, Freeze), the autopilot in the middle, scenes as
// pads on the right. Settings holds what's set once and left.

import SwiftUI

struct LivePage: View {
    @ObservedObject var show: Show
    @State private var naming = false
    @State private var sceneName = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                HStack(alignment: .lastTextBaseline) {
                    PageHeader(title: "Live")
                    Spacer()
                    outputs
                }
                .padding(.bottom, -18)
                music
                HStack(alignment: .top, spacing: 22) {
                    desk.frame(width: 250)
                    autopilot.frame(maxWidth: .infinity)
                    scenes.frame(width: 250)
                }
            }
            .padding(.horizontal, 40)
            .padding(.bottom, 40)
        }
    }

    // MARK: Outputs: what's connected and following the show

    private var outputs: some View {
        HStack(spacing: 18) {
            status("Display", show.displayHeard)
            ForEach(show.fixtures) { f in status(f.kind == .govee ? "Lamp" : "Bulb", f.settings.on && !f.held && show.lampsOn, note: f.held ? "Alexa" : nil) }
            if show.tilesName != nil { status("TILES", show.tiles.on) }
        }
    }

    private func status(_ name: String, _ on: Bool, note: String? = nil) -> some View {
        HStack(spacing: 6) {
            Circle().fill(on ? Sentia.periwinkle : Sentia.line).frame(width: 7, height: 7)
            Text(note.map { "\(name) · \($0)" } ?? name).font(.sora(12)).foregroundColor(on ? Sentia.inkLight : Sentia.ink2)
        }
    }

    // MARK: The music

    private var music: some View {
        HStack(spacing: 28) {
            VStack(alignment: .leading, spacing: 3) {
                Text(show.title.isEmpty ? "Nothing playing" : show.title).font(.sora(15)).lineLimit(1)
                Text(show.artist.isEmpty ? show.status : show.artist).font(.soraLight(12)).foregroundColor(Sentia.ink2).lineLimit(1)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            readout("BPM", show.timing.period > 0.25 ? "\(Int((60 / show.timing.period).rounded()))" : "–")
            readout("Moment", show.timing.playing ? show.autoTier.rawValue.capitalized : "Paused")
            VStack(alignment: .leading, spacing: 8) {
                SectionLabel(text: "Level")
                GeometryReader { g in
                    ZStack(alignment: .leading) {
                        Capsule().fill(Sentia.line).frame(height: 4)
                        Capsule().fill(Sentia.periwinkle).frame(width: g.size.width * min(1, show.timing.energy * 1.4), height: 4)
                    }
                }
                .frame(width: 120, height: 6)
            }
            KickLamp(count: show.kicks)
        }
        .padding(18)
        .overlay(RoundedRectangle(cornerRadius: 18).stroke(Sentia.line, lineWidth: 1))
    }

    private func readout(_ label: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            SectionLabel(text: label)
            Text(value).font(.exo(20))
        }
        .frame(width: 84, alignment: .leading)
    }

    // MARK: The desk

    private var desk: some View {
        VStack(alignment: .leading, spacing: 16) {
            SectionLabel(text: "Desk")
            SentiaSlider(label: "Master", low: .constant(0), high: $show.master, bounds: 0...100, range: false, unit: "%")
            HStack(spacing: 10) {
                DeskButton(title: "Blackout", lit: show.blackout) { show.blackout.toggle() }
                DeskButton(title: "Flash", lit: show.flash, momentary: { show.flash = $0 })
            }
            DeskButton(title: show.freeze ? "Frozen" : "Freeze", lit: show.freeze) { show.freeze.toggle() }
                .opacity(show.autopilot == nil ? 0.45 : 1)
                .disabled(show.autopilot == nil)
            Text("Flash: hold for every light white. Freeze holds the autopilot's look.")
                .font(.soraLight(11)).foregroundColor(Sentia.ink2)
        }
        .padding(18)
        .overlay(RoundedRectangle(cornerRadius: 18).stroke(Sentia.line, lineWidth: 1))
    }

    // MARK: The autopilot

    private static let styles: [(Autopilot.Style?, String)] = [(nil, "Off"), (.club, "Club"), (.lounge, "Lounge"), (.rave, "Rave")]

    private var autopilot: some View {
        VStack(alignment: .leading, spacing: 16) {
            SectionLabel(text: "Autopilot")
            HStack(spacing: 0) {
                ForEach(Self.styles, id: \.1) { s in
                    Button { show.autopilot = s.0 } label: {
                        Text(s.1).font(.sora(13))
                            .frame(maxWidth: .infinity).frame(height: 36)
                            .background(show.autopilot == s.0 ? Sentia.panel : Color.clear)
                            .foregroundColor(show.autopilot == s.0 ? Sentia.inkLight : Sentia.ink2)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    if s.1 != "Rave" { Rectangle().fill(Sentia.line).frame(width: 1, height: 36) }
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(Sentia.line, lineWidth: 1))

            if let style = show.autopilot {
                HStack(alignment: .firstTextBaseline) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(show.liveRecipe?.name ?? "Waiting for the music").font(.exo(22))
                        if let r = show.liveRecipe {
                            Text("\(r.movement.rawValue.capitalized) · bulb \(r.bulb.rawValue) · \(r.display)\(show.tiles.on ? " · TILES \(r.tiles)" : "")")
                                .font(.soraLight(11.5)).foregroundColor(Sentia.ink2).lineLimit(1)
                        }
                    }
                    Spacer()
                    Chip(title: "Next look", chosen: false) { show.pilot.next() }
                }
                HStack(spacing: 8) {
                    ForEach(Autopilot.Palette.allCases, id: \.self) { p in
                        Chip(title: p.name, chosen: show.palette == p) { show.palette = show.palette == p ? nil : p }
                    }
                    Swatches(colours: Binding(get: { [show.paletteColour] }, set: { show.paletteColour = $0[0] }), fixed: true)
                }
                LazyVGrid(columns: [GridItem(.flexible(), spacing: 22), GridItem(.flexible())], alignment: .leading, spacing: 18) {
                    ForEach(Array((Autopilot.macroNames[style] ?? []).enumerated()), id: \.offset) { i, m in
                        SentiaSlider(label: m.0, low: .constant(0), high: macro(style, i), bounds: 0...100, range: false)
                            .help(m.1)
                    }
                }
            } else {
                Text("Off: every light, the display and TILES as you set them. Pick a style to hand the room over.")
                    .font(.soraLight(12)).foregroundColor(Sentia.ink2)
            }
        }
        .padding(18)
        .overlay(RoundedRectangle(cornerRadius: 18).stroke(Sentia.line, lineWidth: 1))
    }

    private func macro(_ s: Autopilot.Style, _ i: Int) -> Binding<Int> {
        Binding(get: { show.macros[s.rawValue]?[i] ?? 50 },
                set: { v in
                    var four = show.macros[s.rawValue] ?? [50, 50, 50, 50]
                    four[i] = v
                    show.macros[s.rawValue] = four
                })
    }

    // MARK: Scenes as pads

    private var scenes: some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionLabel(text: "Scenes")
            LazyVGrid(columns: [GridItem(.flexible(), spacing: 8), GridItem(.flexible())], spacing: 8) {
                ForEach(show.scenes) { sc in
                    Button { show.recall(sc.id) } label: {
                        Text(sc.name).font(.sora(12.5)).multilineTextAlignment(.center).lineLimit(2)
                            .frame(maxWidth: .infinity).frame(height: 56)
                            .overlay(RoundedRectangle(cornerRadius: 12).stroke(Sentia.line, lineWidth: 1))
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .contextMenu { Button("Delete") { show.deleteScene(sc.id) } }
                }
                Button { naming = true } label: {
                    Image(systemName: "plus").font(.system(size: 13, weight: .light)).foregroundColor(Sentia.ink2)
                        .frame(maxWidth: .infinity).frame(height: 56)
                        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Sentia.line, style: StrokeStyle(lineWidth: 1, dash: [4])))
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
            if naming {
                TextField("Name this room", text: $sceneName, onCommit: save)
                    .textFieldStyle(.plain).font(.sora(13))
                    .padding(.horizontal, 10).frame(height: 32)
                    .overlay(RoundedRectangle(cornerRadius: 10).stroke(Sentia.periwinkle, lineWidth: 1))
            }
            Text(show.scenes.isEmpty ? "+ saves the room as it is now: every light, the display's style." : "Click to recall; right-click to delete.")
                .font(.soraLight(11)).foregroundColor(Sentia.ink2)
        }
        .padding(18)
        .overlay(RoundedRectangle(cornerRadius: 18).stroke(Sentia.line, lineWidth: 1))
    }

    private func save() {
        show.saveScene(sceneName)
        sceneName = ""
        naming = false
    }
}

// A desk key: magenta while lit (on, or held), as on the hardware.
struct DeskButton: View {
    let title: String
    let lit: Bool
    var action: (() -> Void)? = nil
    var momentary: ((Bool) -> Void)? = nil
    @State private var held = false

    var body: some View {
        let on = lit || held
        Text(title).font(.sora(13.5))
            .frame(maxWidth: .infinity).frame(height: 48)
            .foregroundColor(on ? Sentia.ink : Sentia.inkLight)
            .background(RoundedRectangle(cornerRadius: 12).fill(on ? Sentia.magenta : Color.clear))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(on ? Sentia.magenta : Sentia.line, lineWidth: 1.2))
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    guard !held else { return }
                    held = true
                    momentary?(true)
                }
                .onEnded { _ in
                    held = false
                    if let m = momentary { m(false) } else { action?() }
                })
    }
}

// Lights up on each kick and falls away, so the show's beat can be seen at a glance.
struct KickLamp: View {
    let count: Int
    @State private var lit = 0.0

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionLabel(text: "Kick")
            Circle().fill(Sentia.magenta.opacity(0.15 + 0.85 * lit)).frame(width: 18, height: 18)
                .overlay(Circle().stroke(Sentia.line, lineWidth: 1))
        }
        .onChange(of: count) { _ in
            lit = 1
            withAnimation(.easeOut(duration: 0.25)) { lit = 0 }
        }
    }
}

// MARK: - Settings

// What's set once and left: the phone remote and its password, timing, start at login.
struct SettingsPage: View {
    @ObservedObject var show: Show
    @State private var password = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                PageHeader(title: "Settings")
                HStack(alignment: .top, spacing: 56) {
                    VStack(alignment: .leading, spacing: 6) {
                        SectionLabel(text: "Phone remote").padding(.bottom, 6)
                        Text(show.publicAddress ?? "Not on the web yet").font(.sora(13)).textSelection(.enabled)
                        Text("At home also \(show.webAddress ?? "—")").font(.soraLight(12)).foregroundColor(Sentia.ink2).textSelection(.enabled)
                        Text(show.hasPassword ? "Phones sign in with the password set here." : "Set a password before a phone can sign in.")
                            .font(.soraLight(11.5)).foregroundColor(Sentia.ink2).padding(.top, 10)
                        SecureField(show.hasPassword ? "New password" : "Password (10 characters or more)", text: $password)
                            .textFieldStyle(.plain).font(.sora(13))
                            .padding(.horizontal, 10).frame(height: 32)
                            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Sentia.line, lineWidth: 1))
                            .padding(.top, 8)
                        HStack(spacing: 8) {
                            Chip(title: "Set password", chosen: false) { if show.setPassword(password) { password = "" } }
                            Chip(title: "Sign out every phone", chosen: false) { show.signOutPhones() }
                        }
                        .padding(.top, 8)
                    }
                    .frame(width: 340, alignment: .leading)
                    VStack(alignment: .leading, spacing: 22) {
                        VStack(alignment: .leading, spacing: 10) {
                            SectionLabel(text: "Timing")
                            SentiaSlider(label: "In time with the speakers", low: .constant(-100), high: $show.trimMs,
                                         bounds: -100...400, range: false, unit: " ms")
                            Text(timingText).font(.soraLight(11.5)).foregroundColor(Sentia.ink2).fixedSize(horizontal: false, vertical: true)
                        }
                        SentiaToggle(title: "Open at login", on: Binding(get: { show.openAtLogin }, set: { show.setOpenAtLogin($0) }))
                    }
                    .frame(width: 420, alignment: .leading)
                }
                .padding(.bottom, 40)
            }
            .padding(.horizontal, 40)
        }
    }

    private var timingText: String {
        let t = show.timing
        var text = "The speakers add \(show.speakerMs) ms; everything is held back to match, plus this. The music reaches the app \(t.captureMs) ms after Spotify plays it; kicks reach the lamp \(t.lampMs) ms after you hear them"
        if show.fixtures.contains(where: { $0.kind == .matter }) { text += ", the bulb \(t.bulbMs) ms" }
        text += "."
        if t.ahead { text += " The kick pattern repeats (\(t.hitPercent)% guessed right): kicks go out ahead." }
        return text
    }
}
