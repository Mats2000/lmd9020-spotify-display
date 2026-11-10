// The rig: every light in the show, each driven by its own engine and look, 30 times a second
// on its own queue, with kicks going out the moment they're heard. A light someone else takes
// (Alexa turns it off, a routine changes it) is let go and left alone until it's taken back.

import Foundation

final class Rig {
    struct Plan {
        var fixtures: [Fixture] = []
        var looks: [UUID: LampLook] = [:]
        var editing: LampLook = LampLook.builtIn[0]  // the look open on the Looks page (and the default)
        var lampsOn = true
    }

    let govee = GoveeLamps()
    let matter = MatterLights()
    var onFrames: (([String: [RGB]], [RGB]) -> Void)?  // each light's colours now, and the edited look's preview
    var onHeld: ((String) -> Void)?                     // someone else took this light
    var wantsFrames = false

    private let analyzer: Analyzer
    private let queue = DispatchQueue(label: "lamps", qos: .userInteractive)
    private var timer: DispatchSourceTimer?
    private var plan = Plan()
    private var engines: [String: LampEngine] = [:]
    private let preview = LampEngine(count: 14, look: LampLook.builtIn[0])
    private var driving = Set<String>()
    private var lastBar: [String: Int] = [:], levelAt: [String: Date] = [:], matterColour: [String: RGB] = [:]
    private var cover: [RGB] = [RGB(255, 236, 210)]
    private var energy = 0.0, quietFor = 0.0, lastTick = Date(), statusAt = Date()

    // Each output's own delay, held back less by it (see Sync).
    private let lampOwn = 0.03, matterOwn = 0.15

    init(analyzer: Analyzer) {
        self.analyzer = analyzer
        govee.onStatus = { [weak self] ip, data in
            // Turned off by someone else, or dimmed from the full brightness the show set.
            let off = data["onOff"] as? Int == 0
            let dimmed = (data["brightness"] as? Int).map { $0 > 0 && $0 < 95 } ?? false
            if off || dimmed { DispatchQueue.main.async { self?.onHeld?("govee:\(ip)") } }
        }
        matter.onReleased = { [weak self] name in self?.onHeld?("matter:\(name)") }
        matter.start()
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: 1.0 / 30, leeway: .milliseconds(2))
        t.setEventHandler { [weak self] in self?.tick() }
        t.resume()
        timer = t
    }

    func set(_ p: Plan) { queue.async { self.plan = p } }
    func setCover(_ colours: [RGB]) { queue.async { self.cover = colours } }

    // Let go of a light (someone else has it), or take it back for the show.
    func hold(_ f: Fixture) {
        queue.async {
            self.driving.remove(f.id)
            switch f.kind {
            case .govee: self.govee.release(f.address)
            case .matter: self.matter.release(f.address)
            }
        }
    }
    func takeBack(_ f: Fixture) {
        if f.kind == .matter { matter.take(f.address) }
    }

    func shutDown() {
        queue.sync {
            govee.stopAll()
            driving.removeAll()
        }
        matter.stop()  // the helper puts the Matter lights back as it exits
    }

    private func look(for f: Fixture) -> LampLook {
        f.settings.lookID.flatMap { plan.looks[$0] } ?? plan.editing
    }

    private func palette(for l: LampLook) -> [RGB] {
        switch l.source {
        case .own: return l.colours.isEmpty ? cover : l.colours
        case .cover: return cover
        case .scheme:
            let s = Int(analyzer.scheme)
            return s > 0 && s <= schemeLampColours.count ? schemeLampColours[s - 1] : cover
        }
    }

    private func engine(for f: Fixture) -> LampEngine {
        if let e = engines[f.id] { return e }
        let e = LampEngine(count: f.kind == .govee ? f.segments : 1, look: look(for: f))
        engines[f.id] = e
        return e
    }

    private func level(_ l: LampLook, _ f: Fixture) -> Double {
        (Double(l.rhythmLow) + Double(l.rhythmHigh - l.rhythmLow) * min(1, energy * 1.2)) / 100 * Double(f.settings.brightness) / 100
    }

    // A kick, from the audio thread: every light's next frame now, the Matter lights flashed.
    func kick(_ strength: Float) {
        queue.async {
            self.preview.kick(strength)
            guard self.plan.lampsOn else { return }
            for f in self.plan.fixtures where self.driving.contains(f.id) {
                let e = self.engine(for: f), l = self.look(for: f)
                e.kick(strength)
                switch f.kind {
                case .govee: self.frame(f, e, dt: 0)
                case .matter:
                    let high = Double(l.rhythmHigh) / 100 * Double(f.settings.brightness) / 100, low = self.level(l, f), id = f.address
                    Sync.later(own: self.matterOwn) { [weak self] in
                        self?.matter.kick(id, level: high)
                        self?.queue.asyncAfter(deadline: .now() + 0.15) { self?.matter.level(id, low, fade: 0.3) }
                    }
                }
            }
        }
    }

    // A snare or hat: a little detail, never the pulse.
    func accent(_ strength: Float) {
        queue.async {
            self.preview.accent(strength)
            for f in self.plan.fixtures where self.driving.contains(f.id) { self.engine(for: f).accent(strength) }
        }
    }

    private func frame(_ f: Fixture, _ e: LampEngine, dt: Double) {
        let k = Double(f.settings.brightness) / 100
        let colours = e.frame(dt: dt, energy: energy).map { $0.scaled(k) }
        let ip = f.address
        Sync.later(own: lampOwn) { [weak self] in self?.govee.show(colours, to: ip) }
        live[f.id] = colours
    }

    private var live: [String: [RGB]] = [:]

    private func start(_ f: Fixture) {
        switch f.kind {
        case .govee: govee.start(f.address)
        case .matter: matter.save(f.address)
        }
        lastBar[f.id] = -1
    }

    private func stop(_ f: Fixture) {
        switch f.kind {
        case .govee: govee.stop(f.address)
        case .matter: matter.restore(f.address)
        }
        live[f.id] = nil
    }

    private func tick() {
        let now = Date(), dt = now.timeIntervalSince(lastTick)
        lastTick = now
        let r = analyzer.reading
        quietFor = r.playing ? 0 : quietFor + dt
        let loud = r.playing ? Double(r.bands.reduce(0) { $0 + Int($1) }) / Double(255 * r.bands.count) : 0
        energy += (loud - energy) * min(1, dt * 6)

        preview.look = plan.editing
        preview.palette = palette(for: plan.editing)
        let shown = preview.frame(dt: dt, energy: r.playing ? energy : 0.35)

        for f in plan.fixtures {
            let on = plan.lampsOn && f.settings.on && !f.held
            let want = on && (r.playing || (driving.contains(f.id) && quietFor < 10))  // ten quiet seconds, then back as they were
            if !want {
                if driving.remove(f.id) != nil && !f.held { stop(f) }
                continue
            }
            if driving.insert(f.id).inserted { start(f) }
            let e = engine(for: f), l = look(for: f)
            e.look = l
            e.palette = palette(for: l)
            switch f.kind {
            case .govee:
                frame(f, e, dt: dt)
            case .matter:
                _ = e.frame(dt: dt, energy: energy)
                let bar = e.beats / 4, base = level(l, f), id = f.address
                if bar != lastBar[f.id] {
                    lastBar[f.id] = bar
                    let c = e.palette[bar % max(1, e.palette.count)]
                    matterColour[f.id] = c
                    Sync.later(own: matterOwn) { [weak self] in self?.matter.colour(id, c, level: base, fade: 0.2) }
                } else if now.timeIntervalSince(levelAt[f.id] ?? .distantPast) > 1 {
                    levelAt[f.id] = now
                    matter.level(id, base, fade: 0.8)
                }
                live[f.id] = [(matterColour[f.id] ?? .black).scaled(base)]
            }
        }
        if now.timeIntervalSince(statusAt) > 3 {  // has anyone else taken a lamp?
            statusAt = now
            govee.checkStatus()
        }
        if wantsFrames {
            let frames = live
            DispatchQueue.main.async { self.onFrames?(frames, shown) }
        }
    }
}
