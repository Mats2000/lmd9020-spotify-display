// The window: a sidebar of pages (Display, Looks, Venue, then TILES), each a calm Sentia page.

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
                case .looks: LooksPage(show: show)
                case .venue: VenuePage(show: show)
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
                let soon = p == .tiles
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

// MARK: - Looks

struct LooksPage: View {
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
                PageHeader(title: "Looks")
                HStack(alignment: .top, spacing: 40) {
                    LampDrawing(segments: show.previewSegments)
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
            Chip(title: "Use on all lights", chosen: false) { show.useOnAll() }.padding(.top, 8)

        }
    }
}

// MARK: - Venue

// Every light with its own switch, look and brightness; scenes; and the phone page.
struct VenuePage: View {
    @ObservedObject var show: Show
    @State private var sceneName = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                HStack(alignment: .lastTextBaseline) {
                    PageHeader(title: "Venue")
                    Spacer()
                    SentiaToggle(title: show.lampsOn ? "Lamps follow the music" : "Lamps off", on: $show.lampsOn)
                }
                HStack(alignment: .top, spacing: 40) {
                    VStack(alignment: .leading, spacing: 12) {
                        SectionLabel(text: "Lights").padding(.bottom, 4)
                        if show.fixtures.isEmpty {
                            Text("Looking for lights…").font(.soraLight(12)).foregroundColor(Sentia.ink2)
                        }
                        ForEach(show.fixtures) { f in FixtureCard(show: show, fixture: f) }
                    }
                    .frame(maxWidth: 560)
                    VStack(alignment: .leading, spacing: 4) {
                        SectionLabel(text: "Scenes").padding(.bottom, 6)
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

                        SectionLabel(text: "On your phone").padding(.top, 36).padding(.bottom, 8)
                        Text(show.webAddress ?? "Not on a network").font(.sora(13)).textSelection(.enabled)
                        Text("Code \(show.webCode)").font(.sora(13)).foregroundColor(Sentia.ink2).padding(.top, 2)
                        Text("Open it in Safari on the home Wi-Fi, then Share → Add to Home Screen.")
                            .font(.soraLight(11.5)).foregroundColor(Sentia.ink2).padding(.top, 6)
                    }
                    .frame(width: 260)
                }
                .padding(.bottom, 40)
            }
            .padding(.horizontal, 40)
        }
    }
}

struct FixtureCard: View {
    @ObservedObject var show: Show
    let fixture: Fixture

    private var brightness: Binding<Int> {
        Binding(get: { fixture.settings.brightness }, set: { v in show.update(fixture.id) { $0.settings.brightness = v } })
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(fixture.name).font(.sora(14))
                    Text(fixture.kind == .govee ? "Govee · \(fixture.segments) segments · \(fixture.address)" : "Matter light")
                        .font(.soraLight(11)).foregroundColor(Sentia.ink2)
                }
                Spacer()
                if !fixture.held {
                    SentiaToggle(title: "", on: Binding(get: { fixture.settings.on }, set: { v in show.update(fixture.id) { $0.settings.on = v } }))
                }
            }
            if fixture.held {
                HStack {
                    Text("Alexa has it. The show leaves it alone.").font(.soraLight(12)).foregroundColor(Sentia.ink2)
                    Spacer()
                    Chip(title: "Take back", chosen: false) { show.takeBack(fixture.id) }
                }
            } else {
                HStack(spacing: 3) {
                    ForEach(Array((show.live[fixture.id] ?? [.black]).enumerated()), id: \.offset) { _, c in
                        RoundedRectangle(cornerRadius: 3).fill(c.color).frame(height: 14)
                    }
                }
                HStack(spacing: 16) {
                    Menu {
                        Button("The open look (\(show.look.name))") { show.update(fixture.id) { $0.settings.lookID = nil } }
                        Divider()
                        ForEach(show.looks) { l in Button(l.name) { show.update(fixture.id) { $0.settings.lookID = l.id } } }
                    } label: {
                        Text(show.looks.first { $0.id == fixture.settings.lookID }?.name ?? "The open look")
                            .font(.sora(12.5)).foregroundColor(Sentia.inkLight)
                    }
                    .menuStyle(.borderlessButton).fixedSize()
                    .padding(.horizontal, 14).frame(height: 32)
                    .overlay(Capsule().stroke(Sentia.line, lineWidth: 1.2))
                    Spacer()
                    Chip(title: "Hand to Alexa", chosen: false) { show.hold(fixture.id) }
                }
                SentiaSlider(label: "Brightness", low: .constant(0), high: brightness, bounds: 0...100, range: false, unit: "%")
            }
        }
        .padding(16)
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(Sentia.line, lineWidth: 1))
        .opacity(fixture.held ? 0.75 : 1)
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
struct Swatches: View {
    @Binding var colours: [RGB]
    @State private var hovered: Int?
    var body: some View {
        HStack(spacing: 10) {
            ForEach(colours.indices, id: \.self) { i in
                ZStack {
                    Circle().fill(colours[i].color).frame(width: 26, height: 26)
                        .overlay(Circle().stroke(Sentia.line, lineWidth: 1))
                    ColorPicker("", selection: Binding(get: { colours[i].color }, set: { colours[i] = RGB($0) }), supportsOpacity: false)
                        .labelsHidden().frame(width: 26, height: 26).opacity(0.02)
                }
                .overlay(alignment: .topTrailing) {
                    if colours.count > 1 && hovered == i {
                        Button { colours.remove(at: i) } label: {
                            Image(systemName: "xmark").font(.system(size: 7, weight: .semibold)).foregroundColor(Sentia.ink2)
                                .frame(width: 14, height: 14).background(Circle().fill(Sentia.ink)).overlay(Circle().stroke(Sentia.line, lineWidth: 1))
                        }.buttonStyle(.plain).offset(x: 5, y: -5)
                    }
                }
                .onHover { hovered = $0 ? i : (hovered == i ? nil : hovered) }
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
