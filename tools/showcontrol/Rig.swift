// The rig: every light in the show, each driven by its own engine and look, 30 times a second
// on its own queue. Kicks are timed to when they're heard: once the beat is steady, the next one
// is sent ahead by each light's own delay, so even a slow bulb lands on it. A light someone else
// takes (Alexa turns it off, a routine changes it) is let go and left alone until it's taken back.

import Foundation

final class Rig {
    struct Plan {
        var fixtures: [Fixture] = []
        var preview = LampLook.builtIn[0]  // the look open on the Lights page
        var previewSegments = 14
        var lampsOn = true
    }
    struct Timing: Equatable {
        var captureMs = 0, lateMs = 0
        var ahead = false  // kicks sent ahead on the beat
    }

    let govee = GoveeLamps()
    let matter = MatterLights()
    var onFrames: (([String: [RGB]], [RGB]) -> Void)?  // each light's colours now, and the open look's preview
    var onHeld: ((String) -> Void)?                     // someone else took this light
    var onTiming: ((Timing) -> Void)?
    var wantsFrames = false

    private let analyzer: Analyzer
    private let queue = DispatchQueue(label: "lamps", qos: .userInteractive)
    private var timer: DispatchSourceTimer?
    private var plan = Plan()
    private var engines: [String: LampEngine] = [:]
    private var preview = LampEngine(count: 14, look: LampLook.builtIn[0])
    private var driving = Set<String>()
    private var lastStep: [String: Int] = [:], levelAt: [String: Date] = [:], kickedAt: [String: Date] = [:]
    private var matterColour: [String: RGB] = [:], matterLevel: [String: Double] = [:]
    private var cover: [RGB] = [RGB(255, 236, 210)]
    private var energy = 0.0, quietFor = 0.0, lastTick = Date(), statusAt = Date(), timingAt = Date()
    private var live: [String: [RGB]] = [:]

    // The beat: the last kick heard, how many in a row fell on the beat, the one sent ahead.
    private var lastHeard = 0.0, streak = 0, ahead: Double?, aheadGen = 0, lastStrength: Float = 0.8
    private var late = 0.0

    private let goveeOwn = 0.03

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

    private func own(_ f: Fixture) -> Double { f.kind == .govee ? goveeOwn : matter.own(f.address) }

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
        let e = LampEngine(count: f.kind == .govee ? f.segments : 1, look: f.settings.look)
        engines[f.id] = e
        return e
    }

    private func level(_ f: Fixture) -> Double {
        let l = f.settings.look
        return (Double(l.rhythmLow) + Double(l.rhythmHigh - l.rhythmLow) * min(1, energy * 1.2)) / 100 * Double(f.settings.brightness) / 100
    }

    // MARK: Kicks

    // A kick, from the audio thread: played is when Spotify played it, period the beat's (0: not sure yet).
    func kick(_ strength: Float, played: Double, period: Double) {
        queue.async {
            self.preview.kick(strength)
            let heard = Sync.heard(played)
            let steady = period > 0.3 && period < 1.0
            if steady && self.lastHeard > 0 {
                let gap = heard - self.lastHeard, beats = (gap / period).rounded()
                self.streak = beats >= 1 && beats <= 2 && abs(gap - beats * period) < period * 0.08 ? self.streak + 1 : 0
            } else {
                self.streak = 0
            }
            self.lastHeard = heard
            self.lastStrength = strength
            // Already sent ahead for this beat?
            let covered = self.ahead.map { abs($0 - heard) < max(0.06, period * 0.12) } ?? false
            self.ahead = nil
            if !covered { self.flashAll(strength, heard: heard) }
            if steady && self.streak >= 3 { self.sendAhead(heard + period, period: period) }
        }
    }

    // The next beat, sent to each light its own delay early. If no kick comes for it, the run is over.
    private func sendAhead(_ heard: Double, period: Double) {
        aheadGen += 1
        let gen = aheadGen
        ahead = heard
        for f in plan.fixtures where driving.contains(f.id) {
            let wait = heard - own(f) - Sync.now
            guard wait > 0.005 else { continue }
            let id = f.id
            queue.asyncAfter(deadline: .now() + wait) { [weak self] in
                guard let self, self.aheadGen == gen, let f = self.plan.fixtures.first(where: { $0.id == id }), self.driving.contains(id) else { return }
                self.flash(f, self.lastStrength)
            }
        }
        queue.asyncAfter(deadline: .now() + max(0, heard - Sync.now) + period * 0.3) { [weak self] in
            guard let self, self.aheadGen == gen, self.ahead == heard else { return }
            self.ahead = nil  // nothing came: wait for a new run before sending ahead again
            self.streak = 0
        }
    }

    // A kick heard at `heard`: each light flashed as close to it as its own delay allows.
    private func flashAll(_ strength: Float, heard: Double) {
        guard plan.lampsOn else { return }
        for f in plan.fixtures where driving.contains(f.id) {
            let wait = heard - own(f) - Sync.now
            late += (max(0, -wait) - late) * 0.2
            let id = f.id
            let work = { [weak self] in
                guard let self, let f = self.plan.fixtures.first(where: { $0.id == id }), self.driving.contains(id) else { return }
                self.flash(f, strength)
            }
            if wait <= 0.003 { work() } else { queue.asyncAfter(deadline: .now() + wait, execute: work) }
        }
    }

    private func flash(_ f: Fixture, _ strength: Float) {
        let amount = Double(f.settings.kick) / 100
        let e = engine(for: f)
        e.kick(strength, amount: amount)
        kickedAt[f.id] = Date()
        switch f.kind {
        case .govee:
            send(f, e.frame(dt: 0, energy: energy))
        case .matter:
            guard amount > 0 else { return }
            let l = f.settings.look, k = Double(f.settings.brightness) / 100
            let base = level(f), top = Double(l.rhythmHigh) / 100 * k
            let high = min(1, base + (top - base) * min(1, amount) + (amount > 1 ? (1 - top) * (amount - 1) : 0))
            let id = f.address
            matter.kick(id, level: high)
            queue.asyncAfter(deadline: .now() + 0.12) { [weak self] in
                self?.matterLevel[f.id] = base
                self?.matter.level(id, base, fade: 0.3)
            }
            live[f.id] = [(matterColour[f.id] ?? .black).scaled(high)]
        }
    }

    // A snare or hat: a little detail, never the pulse.
    func accent(_ strength: Float) {
        queue.async {
            self.preview.accent(strength)
            for f in self.plan.fixtures where self.driving.contains(f.id) && f.settings.kick > 0 { self.engine(for: f).accent(strength) }
        }
    }

    // MARK: Frames

    private func send(_ f: Fixture, _ frame: [RGB]) {
        let colours = frame.map { $0.scaled(Double(f.settings.brightness) / 100) }
        govee.show(colours, to: f.address)
        live[f.id] = colours
    }

    private func start(_ f: Fixture) {
        switch f.kind {
        case .govee: govee.start(f.address)
        case .matter: matter.save(f.address)
        }
        lastStep[f.id] = -1
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

        if preview.count != plan.previewSegments { preview = LampEngine(count: plan.previewSegments, look: plan.preview) }
        preview.look = plan.preview
        preview.palette = palette(for: plan.preview)
        let shown = preview.frame(dt: dt, energy: r.playing ? energy : 0.35)

        for f in plan.fixtures {
            let on = plan.lampsOn && f.settings.on && !f.held
            let want = on && (r.playing || (driving.contains(f.id) && quietFor < 10))  // ten quiet seconds, then back as they were
            if !want {
                if driving.remove(f.id) != nil && !f.held { stop(f) }
                continue
            }
            if driving.insert(f.id).inserted { start(f) }
            let e = engine(for: f), l = f.settings.look
            e.look = l
            e.palette = palette(for: l)
            switch f.kind {
            case .govee:
                let frame = e.frame(dt: dt, energy: energy)
                Sync.later(own: goveeOwn) { [weak self] in self?.send(f, frame) }
            case .matter:
                tickMatter(f, e, now: now, dt: dt)
            }
        }
        if now.timeIntervalSince(statusAt) > 3 {  // has anyone else taken a lamp?
            statusAt = now
            govee.checkStatus()
        }
        if now.timeIntervalSince(timingAt) > 1 {
            timingAt = now
            let t = Timing(captureMs: Int(Sync.captureLag * 1000), lateMs: Int(late * 1000), ahead: streak >= 3)
            DispatchQueue.main.async { self.onTiming?(t) }
        }
        if wantsFrames {
            let frames = live
            DispatchQueue.main.async { self.onFrames?(frames, shown) }
        }
    }

    // A Matter light: a new colour every few beats (faster with the look's speed), and its level
    // following the music between kicks.
    private func tickMatter(_ f: Fixture, _ e: LampEngine, now: Date, dt: Double) {
        _ = e.frame(dt: dt, energy: energy)
        let l = f.settings.look, base = level(f), id = f.address
        let every = [8, 4, 2, 1, 1][max(0, min(4, l.speed - 1))]
        let step = e.beats / every
        if step != lastStep[f.id] {
            lastStep[f.id] = step
            let colours = e.palette.isEmpty ? [RGB(255, 236, 210)] : e.palette
            let c = l.random ? colours.randomElement()! : colours[step % colours.count]
            matterColour[f.id] = c
            matter.colour(id, c, level: base, fade: l.speed >= 4 ? 0 : 0.2)
            matterLevel[f.id] = base
            levelAt[f.id] = now
        } else if now.timeIntervalSince(levelAt[f.id] ?? .distantPast) > 0.25,
                  now.timeIntervalSince(kickedAt[f.id] ?? .distantPast) > 0.45,
                  abs(base - (matterLevel[f.id] ?? -1)) > 0.03 {
            levelAt[f.id] = now
            matterLevel[f.id] = base
            matter.level(id, base, fade: 0.25)
        }
        if now.timeIntervalSince(kickedAt[f.id] ?? .distantPast) > 0.2 {
            live[f.id] = [(matterColour[f.id] ?? .black).scaled(base)]
        }
    }
}
