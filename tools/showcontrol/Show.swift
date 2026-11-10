// The show: what the window (and the phone page) shows and changes. The display's style and
// colours, the looks, every light's own settings, and scenes.

import AppKit
import ServiceManagement
import SwiftUI

let styles = ["Classic Glow", "Spectrum", "Pulse Halo", "Lava", "Rave", "Fractal"]  // VizStyle order
let schemes = ["Cover Colours", "Sentia", "Ice", "Amber", "Soft Club"]  // ColourScheme order

final class Show: ObservableObject {
    enum Page: String, CaseIterable { case display = "Display", looks = "Looks", venue = "Venue", tiles = "TILES" }

    @Published var page: Page = Page(rawValue: UserDefaults.standard.string(forKey: "page") ?? "") ?? .display {
        didSet { UserDefaults.standard.set(page.rawValue, forKey: "page"); wantFrames() }
    }
    @Published var styleName: String { didSet { UserDefaults.standard.set(styleName, forKey: "styleName"); applyStyle() } }
    @Published var schemeName: String { didSet { UserDefaults.standard.set(schemeName, forKey: "schemeName"); applyStyle() } }
    @Published var lampsOn: Bool { didSet { UserDefaults.standard.set(lampsOn, forKey: "lights"); pushPlan() } }
    @Published var looks: [LampLook] { didSet { LookStore.save(looks); pushPlan() } }
    @Published var lookID: UUID { didSet { UserDefaults.standard.set(lookID.uuidString, forKey: "look"); pushPlan() } }
    @Published var fixtures: [Fixture] = [] { didSet { saveVenue(); pushPlan() } }
    @Published var scenes: [ShowScene] { didSet { saveVenue() } }
    @Published var trimMs: Int { didSet { UserDefaults.standard.set(trimMs, forKey: "trim"); Sync.trim = Double(trimMs) / 1000 } }
    @Published var previewSegments = [RGB](repeating: .black, count: 14)
    @Published var live: [String: [RGB]] = [:]
    @Published var preview: NSImage?
    @Published var title = ""
    @Published var artist = ""
    @Published var status = "Starting…"
    @Published var needsPermission = false
    @Published var displayAddress: String?
    @Published var webAddress: String?
    @Published var webCode = ""
    @Published var speakerMs = 0
    @Published var openAtLogin = SMAppService.mainApp.status == .enabled

    // The look open on the Looks page.
    var look: LampLook {
        get { looks.first { $0.id == lookID } ?? looks[0] }
        set { if let i = looks.firstIndex(where: { $0.id == newValue.id }) { looks[i] = newValue } }
    }

    private let link: Link
    private let analyzer: Analyzer
    private let capture: Capture
    private let watcher = NowPlayingWatcher()
    let rig: Rig
    private var venue = VenueStore.load()
    private var heldIDs: Set<String>
    private var frameBuffer = [UInt8](repeating: 0, count: 256 * 240 * 3)
    private let started = Date()
    var windowVisible = true { didSet { if windowVisible != oldValue { wantFrames() } } }
    var phoneWatching = Date.distantPast  // the phone page asks for live colours too

    private let displayOwn = 0.045

    init() {
        let d = UserDefaults.standard
        styleName = d.string(forKey: "styleName") ?? styles[0]
        schemeName = d.string(forKey: "schemeName") ?? schemes[0]
        lampsOn = d.object(forKey: "lights") as? Bool ?? true
        let saved = LookStore.load()
        looks = saved
        lookID = UUID(uuidString: d.string(forKey: "look") ?? "").flatMap { id in saved.first { $0.id == id }?.id } ?? saved[0].id
        trimMs = d.integer(forKey: "trim")
        Sync.trim = Double(d.integer(forKey: "trim")) / 1000
        scenes = venue.scenes
        heldIDs = Set(venue.held)
        let link = Link(host: d.string(forKey: "host"))
        self.link = link
        let displayOwn = self.displayOwn
        analyzer = Analyzer { packet in Sync.later(own: displayOwn) { link.send(packet) } }
        capture = Capture(analyzer: analyzer)
        rig = Rig(analyzer: analyzer)
        analyzer.onKick = { [weak self] s in
            self?.analyzer.sendKick()
            self?.rig.kick(s)
        }
        analyzer.onAccent = { [weak self] s in self?.rig.accent(s) }
        applyStyle()

        rig.onFrames = { [weak self] frames, preview in
            self?.live = frames
            self?.previewSegments = preview
        }
        rig.onHeld = { [weak self] id in self?.hold(id) }
        rig.govee.onFound = { [weak self] in self?.refreshFixtures() }
        watcher.onNewTrack = { [weak self] track, image in
            guard let self else { return }
            self.title = track.title
            self.artist = track.artist
            if self.shuffling { self.shuffle() }
            if let image { self.rig.setCover(coverColours(image)) }
            if let image {
                let cover = rgb(image, size: 320), thumb = rgb(image, size: 64)
                cover.withUnsafeBufferPointer { c in
                    thumb.withUnsafeBufferPointer { t in lmd_demo_set_track(c.baseAddress, t.baseAddress, track.title, track.artist) }
                }
            } else {
                lmd_demo_set_track(nil, nil, track.title, track.artist)
            }
        }
        rig.govee.discover()
        if !CGPreflightScreenCaptureAccess() { CGRequestScreenCaptureAccess() }
        pushPlan()

        update()
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.update() }
        Timer.scheduledTimer(withTimeInterval: 1.0 / 30, repeats: true) { [weak self] _ in self?.drawPreview() }
        var scans = 0
        Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in  // until a lamp answers, then each minute
            guard let self else { return }
            scans += 1
            if self.rig.govee.lamps.isEmpty || scans % 6 == 0 { self.rig.govee.discover() }
        }
    }

    func shutDown() { rig.shutDown() }

    // MARK: Display

    private var shuffling = false
    private var shuffledAt = Date.distantPast

    private func shuffle() {
        var next = Int.random(in: 0..<styles.count)
        if styles.count > 1 && next == Int(analyzer.style) { next = (next + 1 + Int.random(in: 0..<styles.count - 1)) % styles.count }
        analyzer.style = UInt8(next)
        shuffledAt = Date()
    }

    private func applyStyle() {
        analyzer.enabled = styleName != "Off"
        shuffling = styleName == "Shuffle"
        if shuffling { shuffle() } else { analyzer.style = UInt8(styles.firstIndex(of: styleName) ?? 0) }
        analyzer.scheme = UInt8(schemes.firstIndex(of: schemeName) ?? 0)
    }

    // Capture only while Spotify plays and something wants the music, so macOS's recording
    // indicator goes away with it; a short grace keeps it through track changes and quick pauses.
    private var quietSince: Date?
    private var starting = false

    private func update() {
        watcher.poll()
        if shuffling && Date().timeIntervalSince(shuffledAt) > 60 { shuffle() }
        let wanted = (analyzer.enabled || lampsOn) && (watcher.playing || !watcher.reachable)
        if wanted {
            quietSince = nil
            startCapture()
        } else if capture.running {
            let since = quietSince ?? Date()
            quietSince = since
            if Date().timeIntervalSince(since) > 10 {
                quietSince = nil
                Task {
                    await capture.stop()
                    analyzer.quiet()
                }
            }
        }
        displayAddress = link.address
        refreshFixtures()
        wantFrames()
        speakerMs = Int(Sync.speakerDelay * 1000)
        openAtLogin = SMAppService.mainApp.status == .enabled
        let r = analyzer.reading
        if needsPermission { status = "Needs permission to record system audio" }
        else if r.playing { status = "Listening to Spotify" }
        else { status = watcher.paused ? "Spotify is paused" : (watcher.playing ? "Starting…" : "Spotify is quiet") }
    }

    private func startCapture() {
        guard !capture.running, !starting else { return }
        starting = true
        Task {
            defer { Task { @MainActor in self.starting = false } }
            do {
                try await capture.start()
                await MainActor.run { self.needsPermission = false }
            } catch CaptureError.noSpotify, CaptureError.noDisplay {
            } catch {
                await MainActor.run { self.needsPermission = true }
            }
        }
    }

    // The window's preview of the display, drawn by the firmware's own scene code.
    private func drawPreview() {
        guard windowVisible, page == .display else { return }
        let r = analyzer.reading
        lmd_demo_set_playing(watcher.playing ? 1 : 0, watcher.paused ? 1 : 0)
        let now = Date(), c = Calendar.current.dateComponents([.hour, .minute, .weekday, .month, .day], from: now)
        let ms = UInt32(truncatingIfNeeded: Int(now.timeIntervalSince(started) * 1000))
        r.bands.withUnsafeBufferPointer { b in
            r.wave.withUnsafeBufferPointer { w in
                frameBuffer.withUnsafeMutableBufferPointer { out in
                    lmd_demo_render(b.baseAddress, UnsafeRawPointer(w.baseAddress!).assumingMemoryBound(to: Int8.self), r.beat,
                                    r.playing && analyzer.enabled ? 1 : 0, Int32(analyzer.style), Int32(analyzer.scheme), ms,
                                    Int32(c.hour ?? 0), Int32(c.minute ?? 0), Int32((c.weekday ?? 1) - 1), Int32((c.month ?? 1) - 1),
                                    Int32(c.day ?? 1), out.baseAddress)
                }
            }
        }
        let provider = CGDataProvider(data: Data(frameBuffer) as CFData)!
        if let image = CGImage(width: 256, height: 240, bitsPerComponent: 8, bitsPerPixel: 24, bytesPerRow: 256 * 3,
                               space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: 0), provider: provider,
                               decode: nil, shouldInterpolate: true, intent: .defaultIntent) {
            preview = NSImage(cgImage: image, size: NSSize(width: 512, height: 384))
        }
    }

    func setOpenAtLogin(_ on: Bool) {
        if on { try? SMAppService.mainApp.register() } else { try? SMAppService.mainApp.unregister() }
        openAtLogin = SMAppService.mainApp.status == .enabled
    }

    // MARK: Looks

    func saveAsNew() {
        var copy = look
        copy.id = UUID()
        copy.name = "My look \(looks.count + 1)"
        looks.append(copy)
        lookID = copy.id
    }

    func deleteLook(_ id: UUID? = nil) {
        let gone = id ?? lookID
        guard looks.count > 1, let i = looks.firstIndex(where: { $0.id == gone }) else { return }
        looks.remove(at: i)
        if lookID == gone { lookID = looks[max(0, i - 1)].id }
        for k in fixtures.indices where fixtures[k].settings.lookID == gone { fixtures[k].settings.lookID = nil }
    }

    // The open look on every light.
    func useOnAll() {
        for k in fixtures.indices { fixtures[k].settings.lookID = lookID }
    }

    // MARK: Venue

    private func wantFrames() {
        rig.wantsFrames = (windowVisible && (page == .looks || page == .venue)) || Date().timeIntervalSince(phoneWatching) < 5
    }

    // The lights found so far, each with its saved settings.
    private func refreshFixtures() {
        var found: [Fixture] = rig.govee.lamps.map { ip, lamp in
            Fixture(id: "govee:\(ip)", name: "Govee \(lamp.model)", kind: .govee, segments: lamp.segments)
        }
        found += rig.matter.names.map { Fixture(id: "matter:\($0)", name: $0, kind: .matter, segments: 1) }
        found.sort { ($0.kind.rawValue, $0.name) < ($1.kind.rawValue, $1.name) }
        for k in found.indices {
            let id = found[k].id
            if let old = fixtures.first(where: { $0.id == id }) {
                found[k] = old
            } else {
                found[k].settings = venue.fixtures[id] ?? FixtureSettings()
                found[k].name = venue.names[id] ?? found[k].name
                found[k].held = heldIDs.contains(id)
            }
        }
        if found != fixtures { fixtures = found }
    }

    func update(_ id: String, _ change: (inout Fixture) -> Void) {
        guard let k = fixtures.firstIndex(where: { $0.id == id }) else { return }
        change(&fixtures[k])
    }

    // Someone else (Alexa) has this light: the show lets go and stays off it until it's taken back.
    func hold(_ id: String) {
        guard let k = fixtures.firstIndex(where: { $0.id == id }), !fixtures[k].held else { return }
        fixtures[k].held = true
        heldIDs.insert(id)
        rig.hold(fixtures[k])
        saveVenue()
    }

    func takeBack(_ id: String) {
        guard let k = fixtures.firstIndex(where: { $0.id == id }), fixtures[k].held else { return }
        rig.takeBack(fixtures[k])
        heldIDs.remove(id)
        fixtures[k].held = false
    }

    func saveScene(_ name: String) {
        let settings = Dictionary(uniqueKeysWithValues: fixtures.map { ($0.id, $0.settings) })
        scenes.append(ShowScene(name: name.isEmpty ? "Scene \(scenes.count + 1)" : name, fixtures: settings, style: styleName, scheme: schemeName))
    }

    func recall(_ id: UUID) {
        guard let s = scenes.first(where: { $0.id == id }) else { return }
        for k in fixtures.indices { if let f = s.fixtures[fixtures[k].id] { fixtures[k].settings = f } }
        styleName = s.style
        schemeName = s.scheme
    }

    func deleteScene(_ id: UUID) { scenes.removeAll { $0.id == id } }

    private func saveVenue() {
        for f in fixtures {
            venue.fixtures[f.id] = f.settings
            venue.names[f.id] = f.name
        }
        venue.scenes = scenes
        venue.held = Array(heldIDs)
        VenueStore.save(venue)
    }

    private func pushPlan() {
        var p = Rig.Plan()
        p.fixtures = fixtures
        p.looks = Dictionary(uniqueKeysWithValues: looks.map { ($0.id, $0) })
        p.editing = look
        p.lampsOn = lampsOn
        rig.set(p)
    }
}
