// The show: what the window (and the phone page) shows and changes. The display's style and
// colours, every light with its own look and settings, the saved looks (presets), and scenes.

import AppKit
import ServiceManagement
import SwiftUI

let styles = ["Classic Glow", "Spectrum", "Pulse Halo", "Lava", "Rave", "Fractal"]  // VizStyle order
let schemes = ["Cover Colours", "Sentia", "Ice", "Amber", "Soft Club"]  // ColourScheme order

final class Show: ObservableObject {
    enum Page: String, CaseIterable { case display = "Display", lights = "Lights", scenes = "Scenes", tiles = "TILES" }

    @Published var page: Page = Page(rawValue: UserDefaults.standard.string(forKey: "page") ?? "") ?? .display {
        didSet { UserDefaults.standard.set(page.rawValue, forKey: "page"); wantFrames() }
    }
    @Published var styleName: String { didSet { UserDefaults.standard.set(styleName, forKey: "styleName"); applyStyle() } }
    @Published var schemeName: String { didSet { UserDefaults.standard.set(schemeName, forKey: "schemeName"); applyStyle() } }
    @Published var lampsOn: Bool { didSet { UserDefaults.standard.set(lampsOn, forKey: "lights"); pushPlan() } }
    @Published var looks: [LampLook] { didSet { LookStore.save(looks) } }  // the presets
    @Published var selected: String? { didSet { UserDefaults.standard.set(selected, forKey: "light"); pushPlan() } }
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
    @Published var hasPassword = WebAuth.shared.hasPassword
    @Published var phonesSignedIn = WebAuth.shared.keys.count
    let publicAddress = UserDefaults.standard.string(forKey: "publicAddress") ?? "https://showcontrol.matiascevallos.com"
    @Published var speakerMs = 0
    @Published var timing = Rig.Timing()
    @Published var openAtLogin = SMAppService.mainApp.status == .enabled

    // The light open on the Lights page, and its look.
    var light: Fixture? { fixtures.first { $0.id == selected } ?? fixtures.first }
    var look: LampLook {
        get { light?.settings.look ?? LampLook.builtIn[0] }
        set { if let id = light?.id { update(id) { $0.settings.look = newValue } } }
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
        looks = LookStore.load()
        selected = d.string(forKey: "light")
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
        analyzer.onKick = { [weak self] s, played, period in
            self?.analyzer.sendKick()
            self?.rig.kick(s, played: played, period: period)
        }
        analyzer.onAccent = { [weak self] s in self?.rig.accent(s) }
        applyStyle()

        rig.onFrames = { [weak self] frames, preview in
            self?.live = frames
            self?.previewSegments = preview
        }
        rig.onHeld = { [weak self] id in self?.hold(id) }
        rig.onTiming = { [weak self] t in if self?.timing != t { self?.timing = t } }
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

    // MARK: Phone page

    func setPassword(_ p: String) -> Bool {
        let ok = WebAuth.shared.setPassword(p)
        refreshWeb()
        return ok
    }
    func signOutPhones() {
        WebAuth.shared.signOutAll()
        refreshWeb()
    }
    func refreshWeb() {
        hasPassword = WebAuth.shared.hasPassword
        phonesSignedIn = WebAuth.shared.keys.count
    }

    func setOpenAtLogin(_ on: Bool) {
        if on { try? SMAppService.mainApp.register() } else { try? SMAppService.mainApp.unregister() }
        openAtLogin = SMAppService.mainApp.status == .enabled
    }

    // MARK: Looks (presets): copied into a light, saved from one

    func loadPreset(_ preset: UUID, into id: String? = nil) {
        guard let p = looks.first(where: { $0.id == preset }), let id = id ?? light?.id else { return }
        update(id) { $0.settings.look = p }
    }

    func savePreset(from id: String? = nil) {
        guard let f = id.flatMap({ i in fixtures.first { $0.id == i } }) ?? light else { return }
        var copy = f.settings.look
        copy.id = UUID()
        if looks.contains(where: { $0.name == copy.name }) { copy.name = "\(copy.name) \(looks.count + 1)" }
        looks.append(copy)
        update(f.id) { $0.settings.look = copy }
    }

    func deletePreset(_ id: UUID) {
        guard looks.count > 1 else { return }
        looks.removeAll { $0.id == id }
    }

    // This light's look on every light.
    func sameOnAll(from id: String? = nil) {
        guard let f = id.flatMap({ i in fixtures.first { $0.id == i } }) ?? light else { return }
        for k in fixtures.indices { fixtures[k].settings.look = f.settings.look }
    }

    // Which preset a look is, if it's one unchanged.
    func preset(of l: LampLook) -> UUID? { looks.first { $0 == l }?.id }

    // MARK: Venue

    private func wantFrames() {
        rig.wantsFrames = (windowVisible && (page == .lights || page == .scenes)) || Date().timeIntervalSince(phoneWatching) < 5
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
                found[k].settings = venue.fixtures[id] ?? {
                    var s = FixtureSettings()
                    s.look = looks.first ?? LampLook.builtIn[0]
                    return s
                }()
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
        p.preview = look
        p.previewSegments = light.map { $0.kind == .govee ? $0.segments : 1 } ?? 14
        p.lampsOn = lampsOn
        rig.set(p)
    }
}
