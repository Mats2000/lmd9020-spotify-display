// Show Control by Sentia: the home's lighting desk and show control. It listens to Spotify on this
// Mac and drives everything visual from it: the LMD-9020 display (its visualizer), Govee lamps
// segment by segment, Matter lights. Kicks go out the moment they're heard; every output is held
// back just enough to land with the sound from the speakers. Build with build.sh.
//
// macOS asks once to allow screen and system audio recording (that's how one app's sound is
// captured; nothing of the screen is used), access to the local network, and control of Spotify.

import AppKit
import ServiceManagement
import SwiftUI

let styles = ["Classic Glow", "Spectrum", "Pulse Halo", "Lava", "Rave", "Fractal"]  // VizStyle order
let schemes = ["Cover Colours", "Sentia", "Ice", "Amber", "Soft Club"]  // ColourScheme order

final class Show: ObservableObject {
    enum Page: String, CaseIterable { case display = "Display", lamps = "Lamps", venue = "Venue", tiles = "TILES" }

    // What the window shows and edits.
    @Published var page: Page = .display
    @Published var styleName: String { didSet { UserDefaults.standard.set(styleName, forKey: "styleName"); applyStyle() } }
    @Published var schemeName: String { didSet { UserDefaults.standard.set(schemeName, forKey: "schemeName"); applyStyle() } }
    @Published var lampsOn: Bool { didSet { UserDefaults.standard.set(lampsOn, forKey: "lights"); if !lampsOn { quietLamps() } } }
    @Published var looks: [LampLook]
    @Published var lookID: UUID { didSet { UserDefaults.standard.set(lookID.uuidString, forKey: "look"); pushLook() } }
    @Published var trimMs: Int { didSet { UserDefaults.standard.set(trimMs, forKey: "trim"); Sync.trim = Double(trimMs) / 1000 } }
    @Published var segments = [RGB](repeating: .black, count: 14)
    @Published var preview: NSImage?
    @Published var title = ""
    @Published var artist = ""
    @Published var status = "Starting…"
    @Published var needsPermission = false
    @Published var displayAddress: String?
    @Published var lampNames: [String] = []
    @Published var matterNames: [String] = []
    @Published var speakerMs = 0
    @Published var openAtLogin = SMAppService.mainApp.status == .enabled

    var look: LampLook {
        get { looks.first { $0.id == lookID } ?? looks[0] }
        set {
            guard let i = looks.firstIndex(where: { $0.id == newValue.id }) else { return }
            looks[i] = newValue
            LookStore.save(looks)
            pushLook()
        }
    }

    private let link: Link
    private let analyzer: Analyzer
    private let capture: Capture
    private let watcher = NowPlayingWatcher()
    private let govee = GoveeLamps()
    private let matter = MatterLights()
    private let engine: LampEngine
    private let lampQueue = DispatchQueue(label: "lamps", qos: .userInteractive)
    private var lampTimer: DispatchSourceTimer?
    private var coverPalette: [RGB] = [RGB(255, 236, 210)]
    private var frameBuffer = [UInt8](repeating: 0, count: 256 * 240 * 3)
    private let started = Date()
    var windowVisible = true

    // Each output's own delay, held back less by it (see Sync).
    private let displayOwn = 0.045, lampOwn = 0.03, matterOwn = 0.15

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
        engine = LampEngine(count: 14, look: saved[0])
        let link = Link(host: d.string(forKey: "host"))
        self.link = link
        let displayOwn = self.displayOwn
        analyzer = Analyzer { packet in Sync.later(own: displayOwn) { link.send(packet) } }
        capture = Capture(analyzer: analyzer)
        analyzer.onKick = { [weak self] strength in self?.kick(strength) }
        pushLook()
        applyStyle()

        watcher.onNewTrack = { [weak self] track, image in
            guard let self else { return }
            self.title = track.title
            self.artist = track.artist
            if self.shuffling { self.shuffle() }
            if let image { self.coverPalette = coverColours(image) }
            if let image {
                let cover = rgb(image, size: 320), thumb = rgb(image, size: 64)
                cover.withUnsafeBufferPointer { c in
                    thumb.withUnsafeBufferPointer { t in lmd_demo_set_track(c.baseAddress, t.baseAddress, track.title, track.artist) }
                }
            } else {
                lmd_demo_set_track(nil, nil, track.title, track.artist)
            }
        }
        govee.onFound = { [weak self] in self?.lampNames = self?.govee.lamps.values.map(\.model).sorted() ?? [] }
        govee.discover()
        matter.start()
        if !CGPreflightScreenCaptureAccess() { CGRequestScreenCaptureAccess() }

        update()
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.update() }
        Timer.scheduledTimer(withTimeInterval: 1.0 / 30, repeats: true) { [weak self] _ in self?.drawPreview() }
        var scans = 0
        Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in  // until a lamp answers, then each minute
            guard let self else { return }
            scans += 1
            if self.govee.lamps.isEmpty || scans % 6 == 0 { self.govee.discover() }
        }
        startLamps()
    }

    func shutDown() {
        govee.stop()
        matter.stop()  // the helper puts the Matter lights back as it exits
    }

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
        matterNames = matter.names
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

    // MARK: Lamps

    func saveAsNew() {
        var copy = look
        copy.id = UUID()
        copy.name = "My look \(looks.count + 1)"
        looks.append(copy)
        LookStore.save(looks)
        lookID = copy.id
    }

    func deleteLook() {
        guard looks.count > 1, let i = looks.firstIndex(where: { $0.id == lookID }) else { return }
        looks.remove(at: i)
        LookStore.save(looks)
        lookID = looks[max(0, i - 1)].id
    }

    private func pushLook() {
        let l = look
        lampQueue.async { self.engine.look = l }
    }

    // The colours the lamps use now: the look's own, the cover's, or the scheme's.
    private func palette(for l: LampLook) -> [RGB] {
        switch l.source {
        case .own: return l.colours.isEmpty ? coverPalette : l.colours
        case .cover: return coverPalette
        case .scheme:
            let s = Int(analyzer.scheme)
            return s > 0 && s <= schemeLampColours.count ? schemeLampColours[s - 1] : coverPalette
        }
    }

    // A kick, from the audio thread the moment it's heard: the display first (its packet goes now),
    // then the lamps' next frame and the Matter lights, each held back to land with the sound.
    private func kick(_ strength: Float) {
        analyzer.sendKick()
        guard lampsOn else { return }
        lampQueue.async {
            self.engine.kick(strength)
            self.lampFrame(dt: 0)
        }
        let high = Double(look.rhythmHigh) / 100, low = baseLevel
        Sync.later(own: matterOwn) { [weak self] in
            self?.matter.kick(level: high)
            self?.lampQueue.asyncAfter(deadline: .now() + 0.15) { self?.matter.send(["level": low, "fade": 0.3]) }
        }
    }

    private var energy = 0.0, baseLevel = 0.4, lastTick = Date(), quietFor = 0.0, lastBar = -1, levelAt = Date.distantPast

    private func startLamps() {
        let t = DispatchSource.makeTimerSource(queue: lampQueue)
        t.schedule(deadline: .now(), repeating: 1.0 / 30, leeway: .milliseconds(2))
        t.setEventHandler { [weak self] in self?.lampTick() }
        t.resume()
        lampTimer = t
    }

    private func quietLamps() {
        lampQueue.async {
            self.govee.stop()
            self.matter.send(["restore": true])
            self.lastBar = -1
        }
    }

    // 30 times a second, on the lamp queue.
    private func lampTick() {
        let now = Date(), dt = now.timeIntervalSince(lastTick)
        lastTick = now
        let r = analyzer.reading
        guard lampsOn, r.playing else {  // ten seconds of quiet: the lamps go back to how they were
            if govee.streaming || lastBar >= 0 {
                quietFor += dt
                if quietFor > 10 { quietLamps() }
            }
            return
        }
        quietFor = 0
        if !govee.streaming {
            govee.start()
            matter.send(["save": true])
        }
        let loud = Double(r.bands.reduce(0) { $0 + Int($1) }) / Double(255 * r.bands.count)
        energy += (loud - energy) * min(1, dt * 6)
        lampFrame(dt: dt)

        // The Matter lights: the bar's colour, and their resting level with the loudness.
        let l = engine.look
        let bar = engine.beats / 4
        baseLevel = (Double(l.rhythmLow) + Double(l.rhythmHigh - l.rhythmLow) * min(1, energy * 1.2)) / 100
        if bar != lastBar {
            lastBar = bar
            let colours = engine.palette
            let c = colours[bar % max(1, colours.count)], level = baseLevel
            Sync.later(own: matterOwn) { [weak self] in self?.matter.colour(c, level: level, fade: 0.2) }
        } else if now.timeIntervalSince(levelAt) > 1 {
            levelAt = now
            matter.send(["level": baseLevel, "fade": 0.8])
        }
    }

    private func lampFrame(dt: Double) {
        engine.palette = palette(for: engine.look)
        let frame = engine.frame(dt: dt, energy: energy)
        Sync.later(own: lampOwn) { [weak self] in self?.govee.show(frame) }
        if windowVisible && page == .lamps { DispatchQueue.main.async { self.segments = frame } }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var window: NSWindow!
    private var show: Show!
    private var activity: NSObjectProtocol?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Keep running at full rate with the window closed: no App Nap.
        activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiatedAllowingIdleSystemSleep, .latencyCritical],
                                                         reason: "Running the show")
        Sentia.registerFonts()
        show = Show()
        let menu = NSMenu(), appItem = NSMenuItem()
        menu.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Quit Show Control", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        NSApp.mainMenu = menu

        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1180, height: 760),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.title = "Show Control"
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.isReleasedWhenClosed = false
        window.backgroundColor = NSColor(Sentia.ink)
        window.appearance = NSAppearance(named: .darkAqua)
        window.minSize = NSSize(width: 1080, height: 700)
        window.contentView = NSHostingView(rootView: RootView(show: show))
        window.delegate = self
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.show.windowVisible = self.window.isVisible && self.window.occlusionState.contains(.visible)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationWillTerminate(_ notification: Notification) { show.shutDown() }  // the lights as they were
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        window.makeKeyAndOrderFront(nil)
        return true
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
