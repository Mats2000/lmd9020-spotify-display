// The window: a sidebar of pages (Display, Lamps, then Venue and TILES), each a calm Sentia page.

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
                case .lamps: LampsPage(show: show)
                case .venue: SoonPage(title: "Venue", text: "Every fixture in the room on one plan, and scenes that move between them.")
                case .tiles: SoonPage(title: "TILES", text: "Pads that call up looks, and TILES' own lights as part of the show.")
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
                let soon = p == .venue || p == .tiles
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
                        Text(show.displayAddress.map { "LMD-9020 at \($0)" } ?? "Looking for the display…")
                            .font(.soraLight(12)).foregroundColor(Sentia.ink2)
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

// MARK: - Lamps

struct LampsPage: View {
    @ObservedObject var show: Show

    private func bind<T>(_ kp: WritableKeyPath<LampLook, T>) -> Binding<T> {
        Binding(get: { show.look[keyPath: kp] }, set: { var l = show.look; l[keyPath: kp] = $0; show.look = l })
    }

    private static let movementNames: [LampLook.Movement: String] = [
        .flow: "Flow", .twinkle: "Twinkle", .flexing: "Flexing", .stacking: "Stacking",
        .strike: "Strike", .gradient: "Gradient", .chase: "Chase", .pulse: "Pulse",
    ]
    private static let sourceNames: [LampLook.Source: String] = [.own: "These colours", .cover: "Cover", .scheme: "Display colours"]
    private static let directionNames: [LampLook.Direction: String] = [
        .up: "Up", .down: "Down", .inward: "Inward", .outward: "Outward", .random: "Random",
    ]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                HStack(alignment: .lastTextBaseline) {
                    PageHeader(title: "Lamps")
                    Spacer()
                    SentiaToggle(title: show.lampsOn ? "Lamps follow the music" : "Lamps off", on: $show.lampsOn)
                }
                HStack(alignment: .top, spacing: 40) {
                    LampDrawing(segments: show.segments)
                    controls.frame(maxWidth: 520)
                    looksColumn.frame(width: 230)
                }
                .padding(.bottom, 40)
            }
            .padding(.horizontal, 40)
        }
    }

    private var controls: some View {
        VStack(alignment: .leading, spacing: 30) {
            VStack(alignment: .leading, spacing: 12) {
                SectionLabel(text: "Movement")
                LazyVGrid(columns: Array(repeating: GridItem(.fixed(118), spacing: 8), count: 4), alignment: .leading, spacing: 8) {
                    ForEach(LampLook.Movement.allCases, id: \.self) { m in
                        Chip(title: Self.movementNames[m]!, chosen: show.look.movement == m, width: 118) { bind(\.movement).wrappedValue = m }
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
                if show.look.source == .own { Swatches(colours: bind(\.colours)) }
                HStack(spacing: 8) {
                    Chip(title: "In order", chosen: !show.look.random) { bind(\.random).wrappedValue = false }
                    Chip(title: "Random", chosen: show.look.random) { bind(\.random).wrappedValue = true }
                }
            }
            VStack(alignment: .leading, spacing: 26) {
                SentiaSlider(label: "Spots", low: bind(\.spotsLow), high: bind(\.spotsHigh), bounds: 1...8, range: true)
                SentiaSlider(label: "Size", low: bind(\.sizeLow), high: bind(\.sizeHigh), bounds: 1...10, range: true)
                SentiaSlider(label: "Speed", low: .constant(1), high: bind(\.speed), bounds: 1...5, range: false)
                SentiaSlider(label: "Rhythm", low: bind(\.rhythmLow), high: bind(\.rhythmHigh), bounds: 0...100, range: true, unit: "%")
            }
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

    private var looksColumn: some View {
        VStack(alignment: .leading, spacing: 4) {
            SectionLabel(text: "Looks").padding(.bottom, 6)
            ForEach(show.looks) { l in
                ListRow(title: l.name, chosen: l.id == show.lookID) { show.lookID = l.id }
            }
            TextField("Name", text: bind(\.name))
                .textFieldStyle(.plain).font(.sora(13))
                .padding(.horizontal, 10).frame(height: 32)
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(Sentia.line, lineWidth: 1))
                .padding(.top, 14)
            HStack(spacing: 8) {
                Chip(title: "Save as new", chosen: false) { show.saveAsNew() }
                Chip(title: "Delete", chosen: false) { show.deleteLook() }
            }.padding(.top, 8)

            SectionLabel(text: "Fixtures").padding(.top, 32).padding(.bottom, 6)
            if show.lampNames.isEmpty && show.matterNames.isEmpty {
                Text("Looking for lamps…").font(.soraLight(12)).foregroundColor(Sentia.ink2)
            }
            ForEach(show.lampNames, id: \.self) { n in Fixture(name: "Govee \(n)", kind: n == "H6076" ? "14 segments" : "segments") }
            ForEach(show.matterNames, id: \.self) { n in Fixture(name: n, kind: "Matter light") }
        }
    }
}

struct Fixture: View {
    let name: String, kind: String
    var body: some View {
        HStack {
            Text(name).font(.sora(12.5))
            Spacer()
            Text(kind).font(.soraLight(11)).foregroundColor(Sentia.ink2)
        }.frame(height: 28)
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

// The look's own colours: click one to change it, right-click to remove, + to add (up to six).
struct Swatches: View {
    @Binding var colours: [RGB]
    var body: some View {
        HStack(spacing: 10) {
            ForEach(colours.indices, id: \.self) { i in
                ZStack {
                    Circle().fill(colours[i].color).frame(width: 26, height: 26)
                        .overlay(Circle().stroke(Sentia.line, lineWidth: 1))
                    ColorPicker("", selection: Binding(get: { colours[i].color }, set: { colours[i] = RGB($0) }), supportsOpacity: false)
                        .labelsHidden().frame(width: 26, height: 26).opacity(0.02)
                }
                .contextMenu { if colours.count > 1 { Button("Remove") { colours.remove(at: i) } } }
            }
            if colours.count < 6 {
                Button { colours.append(colours.last ?? RGB(255, 255, 255)) } label: {
                    Image(systemName: "plus").font(.system(size: 11, weight: .light)).foregroundColor(Sentia.ink2)
                        .frame(width: 26, height: 26).overlay(Circle().stroke(Sentia.line, lineWidth: 1))
                }.buttonStyle(.plain)
            }
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
