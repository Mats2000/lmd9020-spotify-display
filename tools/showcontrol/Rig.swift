// The rig: every light in the show, each driven by its own engine and look, 30 times a second
// on its own queue. Kicks are timed to when they're heard; once the kick pattern repeats, the next
// ones are sent ahead to each light by its own delay (and 10 ms more) so even a slow one lands on
// them. A light someone else takes (Alexa turns it off, a routine changes it) is let go and left
// alone until it's taken back.

import Foundation

final class Rig {
    struct Plan {
        var fixtures: [Fixture] = []
        var preview = LampLook.builtIn[0]  // the look open on the Lights page
        var previewSegments = 14
        var lampsOn = true
    }
    struct Timing: Equatable {
        var captureMs = 0, bufferMs = 0
        var lampMs = 0, bulbMs = 0  // how late kicks reach each kind of light, its own delay included
        var ahead = false           // kicks sent ahead, from the pattern
        var hitPercent = 0          // how many guessed kicks came true lately
        var songSteps = 0           // times the song changed character
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

    // The song's character: how its energy spreads over the bands, and how loud, over ~1.5 s against
    // ~10 s. When they part much further than they usually do in this song (a drop, a breakdown, a
    // new section), at least 8 s after the last, it's a new step.
    private var fastShape = [Double](repeating: 0, count: bandCount + 1), slowShape = [Double](repeating: 0, count: bandCount + 1)
    private var songStep = 0, songChangedAt = Date()
    private(set) var songChange = 0.0  // how far apart
    private var changeMean = 0.15, changeDev = 0.05  // how far apart they usually are, over ~20 s

    private func followSong(_ r: Analyzer.Reading, dt: Double, now: Date) {
        guard r.playing else { return }
        let total = Double(r.bands.reduce(0) { $0 + Int($1) })
        guard total > 0 else { return }
        var shape = r.bands.map { Double($0) / total }
        shape.append(energy * 0.5)
        for i in shape.indices {
            fastShape[i] += (shape[i] - fastShape[i]) * min(1, dt / 1.5)
            slowShape[i] += (shape[i] - slowShape[i]) * min(1, dt / 10)
        }
        songChange = zip(fastShape, slowShape).reduce(0) { $0 + abs($1.0 - $1.1) }
        let a = min(1, dt / 20)
        changeMean += (songChange - changeMean) * a
        changeDev += (abs(songChange - changeMean) - changeDev) * a
        if songChange > max(0.3, changeMean + 3 * changeDev) && now.timeIntervalSince(songChangedAt) > 8 {
            songStep += 1
            songChangedAt = now
            kickLog?.write(Data(String(format: "song %.4f %d %.3f\n", Sync.now, songStep, songChange).utf8))
            slowShape = fastShape  // the new section is the measure from here
        }
    }

    // Kicks ahead, for every light (a Matter bulb can't keep up otherwise, and the lamp's Wi-Fi is
    // often late): a beat grid is fitted through
    // the last 8 s of kicks (which averages out their few tens of ms of wobble), and each sixteenth
    // of it that had a kick in both of the last two bars is guessed to have one again. Guesses are
    // scored as they come true or not, and sent only while most do.
    private struct Guess { var at: Double; var sent: Bool; var id: Int }
    private var heardKicks: [Double] = [], guesses: [Guess] = [], guessID = 0
    private var hits = 0.0, scored = 0, lastStrength: Float = 0.8
    private var lateLamp = 0.0, lateBulb = 0.0, lastKickFrame = Date.distantPast
    private var sendingAhead: Bool { scored >= 8 && hits >= 0.7 }
    // Sent ahead, a guess goes this much earlier still than a light's own delay.
    private let spare = 0.01

    private let goveeOwn = 0.03
    // `defaults write com.sentiainstruments.lmd-visualizer logKicks -bool true`: every kick to
    // ~/Library/Logs/Show Control/kicks.log (heard, period, strength, found at, played), to study the timing.
    private lazy var kickLog: FileHandle? = {
        guard UserDefaults.standard.bool(forKey: "logKicks") else { return nil }
        let url = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0].appendingPathComponent("Logs/Show Control/kicks.log")
        FileManager.default.createFile(atPath: url.path, contents: nil)
        return try? FileHandle(forWritingTo: url)
    }()

    init(analyzer: Analyzer) {
        self.analyzer = analyzer
        govee.onStatus = { [weak self] ip, data in
            // Turned off by someone else, or dimmed from the full brightness the show set.
            let off = data["onOff"] as? Int == 0
            let dimmed = (data["brightness"] as? Int).map { $0 > 0 && $0 < 95 } ?? false
            if off || dimmed { DispatchQueue.main.async { self?.onHeld?("govee:\(ip)") } }
        }
        matter.onReleased = { [weak self] name in self?.onHeld?("matter:\(name)") }
        // A new helper drives no light yet: the next tick hands it the ones in the show again.
        matter.onStarted = { [weak self] in self?.queue.async { self?.driving = self?.driving.filter { !$0.hasPrefix("matter:") } ?? [] } }
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

    // A bulb between kicks: the loudness takes it under half way up the rhythm, so a kick always
    // stands out; a kick over 100% darkens it further, for a strobe.
    private func level(_ f: Fixture) -> Double {
        let l = f.settings.look, k = Double(f.settings.kick) / 100
        let base = (Double(l.rhythmLow) + Double(l.rhythmHigh - l.rhythmLow) * 0.45 * min(1, energy * 1.5)) / 100
        return base * Double(f.settings.brightness) / 100 * (k > 1 ? max(0.15, 1 - (k - 1) * 1.4) : 1)
    }

    // MARK: Kicks

    // A kick, from the audio thread: played is when Spotify played it, period the beat's (0: not sure yet).
    func kick(_ strength: Float, played: Double, period: Double) {
        queue.async {
            self.preview.kick(strength)
            let heard = Sync.heard(played)
            self.kickLog?.write(Data(String(format: "%.4f %.4f %.3f %.4f %.4f\n", heard, period, strength, Sync.now, played).utf8))
            self.lastStrength = strength
            var covered = false  // every light already had it, sent ahead
            for g in self.guesses where abs(g.at - heard) < 0.05 {
                covered = covered || g.sent
                self.score(true)
            }
            self.guesses.removeAll { abs($0.at - heard) < 0.05 }
            if !covered { self.flashAll(strength, heard: heard) }
            self.heardKicks.removeAll { heard - $0 > 8 }
            self.heardKicks.append(heard)
            if period > 0.25 && period < 1.1, let grid = self.fit(period) { self.guessAhead(grid, from: heard) }
        }
    }

    // The beat grid (a beat's time and the period) through the kicks near its beats, by least squares.
    private func fit(_ seed: Double) -> (t0: Double, period: Double)? {
        guard heardKicks.count >= 6, let last = heardKicks.last else { return nil }
        var t0 = last, p = seed
        for _ in 0..<3 {
            var n = 0.0, sk = 0.0, st = 0.0, skk = 0.0, skt = 0.0
            for t in heardKicks {
                let k = ((t - t0) / p).rounded()
                guard abs(t - t0 - k * p) < p * 0.15 else { continue }
                n += 1; sk += k; st += t; skk += k * k; skt += k * t
            }
            let v = n * skk - sk * sk
            guard n >= 4, v > 0 else { return nil }
            p = (n * skt - sk * st) / v
            t0 = (st - p * sk) / n
            guard p > 0.25 && p < 1.1 else { return nil }
        }
        return (t0, p)
    }

    // The next half second's sixteenths where both bars before had a kick.
    private func guessAhead(_ grid: (t0: Double, period: Double), from heard: Double) {
        let step = grid.period / 4
        func kicked(_ t: Double) -> Bool { heardKicks.contains { abs($0 - t) < 0.05 } }
        var s = ((heard - grid.t0) / step).rounded(.down) + 1
        while true {
            let at = grid.t0 + s * step
            s += 1
            if at <= heard + 0.03 { continue }
            if at > heard + 0.6 { break }
            guard kicked(at - 16 * step) && kicked(at - 32 * step), !guesses.contains(where: { abs($0.at - at) < 0.03 }) else { continue }
            guessID += 1
            let g = Guess(at: at, sent: sendingAhead && plan.lampsOn, id: guessID)
            guesses.append(g)
            guard g.sent else { continue }
            for f in plan.fixtures where driving.contains(f.id) {
                let wait = at - own(f) - spare - Sync.now
                guard wait > 0.005 else { continue }
                let id = f.id
                queue.asyncAfter(deadline: .now() + wait) { [weak self] in
                    guard let self, self.guesses.contains(where: { $0.id == g.id }),
                          let f = self.plan.fixtures.first(where: { $0.id == id }), self.driving.contains(id) else { return }
                    self.flash(f, self.lastStrength)
                }
            }
        }
    }

    private func score(_ hit: Bool) {
        scored += 1
        hits += ((hit ? 1 : 0) - hits) * 0.1
    }

    // Guesses no kick came for (allowing for the time a kick takes to be found).
    private func expireGuesses() {
        let late = Sync.now - 0.12
        for g in guesses where g.at < late { score(false) }
        guesses.removeAll { $0.at < late }
    }

    // A kick heard at `heard`: each light flashed as close to it as its own delay allows.
    private func flashAll(_ strength: Float, heard: Double) {
        guard plan.lampsOn else { return }
        for f in plan.fixtures where driving.contains(f.id) {
            let wait = heard - own(f) - Sync.now
            if f.kind == .govee { lateLamp += (max(0, -wait) - lateLamp) * 0.2 } else { lateBulb += (max(0, -wait) - lateBulb) * 0.2 }
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
        // A Matter bulb takes a kick as two commands of a round trip each: closer together than about
        // two round trips and they queue in it, run together (it looks like fading) and fall behind.
        if f.kind == .matter, Date().timeIntervalSince(kickedAt[f.id] ?? .distantPast) < max(0.3, 2.2 * matter.trip(f.address)) { return }
        let e = engine(for: f)
        e.kick(strength, amount: amount)
        kickedAt[f.id] = Date()
        switch f.kind {
        case .govee:
            lastKickFrame = Date()
            send(f, e.frame(dt: 0, energy: energy))
        case .matter:
            guard amount > 0 else { return }
            let l = f.settings.look, top = Double(l.rhythmHigh) / 100 * Double(f.settings.brightness) / 100
            let base = level(f), high = min(1, base + (top - base) * min(1, amount))
            // Over 100%: a strobe, so straight off after each kick (this bulb only dims slowly).
            matter.kick(f.address, level: high, then: base, fall: amount > 1 ? 0.05 : 0.2, off: amount > 1)
            matterLevel[f.id] = base
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
        followSong(r, dt: dt, now: now)

        if preview.count != plan.previewSegments { preview = LampEngine(count: plan.previewSegments, look: plan.preview) }
        preview.look = plan.preview
        preview.palette = palette(for: plan.preview)
        preview.songStep = songStep
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
            e.songStep = songStep
            switch f.kind {
            case .govee:
                let frame = e.frame(dt: dt, energy: energy)
                // Right after a kick's own frame, this one waits: more than the lamp takes would queue in it.
                guard now.timeIntervalSince(lastKickFrame) > 0.025 else { continue }
                Sync.later(own: goveeOwn) { [weak self] in self?.send(f, frame) }
            case .matter:
                tickMatter(f, e, now: now, dt: dt)
            }
        }
        expireGuesses()
        if now.timeIntervalSince(statusAt) > 3 {  // has anyone else taken a lamp?
            statusAt = now
            govee.checkStatus()
        }
        if now.timeIntervalSince(timingAt) > 1 {
            timingAt = now
            let t = Timing(captureMs: Int(Sync.captureLag * 1000), bufferMs: Int(Sync.bufferLength * 1000),
                           lampMs: Int(lateLamp * 1000), bulbMs: Int(lateBulb * 1000), ahead: sendingAhead,
                           hitPercent: Int(hits * 100), songSteps: songStep)
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
        let strobe = f.settings.kick > 100  // only hard changes: kicks, and colours landing while it's off
        let every = [8, 4, 2, 1, 1][max(0, min(4, l.speed - 1))]
        let step = l.bySong ? songStep : e.beats / every
        if step != lastStep[f.id] {
            lastStep[f.id] = step
            let colours = e.palette.isEmpty ? [RGB(255, 236, 210)] : e.palette
            let c = l.random ? colours.randomElement()! : colours[step % colours.count]
            if c != matterColour[f.id] { matter.colour(id, c, fade: strobe ? 0 : l.bySong ? 1.5 : l.speed >= 4 ? 0 : 0.2) }
            matterColour[f.id] = c
        } else if !strobe, now.timeIntervalSince(levelAt[f.id] ?? .distantPast) > 0.3,
                  now.timeIntervalSince(kickedAt[f.id] ?? .distantPast) > 1.0,  // the kicks set it meanwhile
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
