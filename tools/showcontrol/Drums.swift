// Drum hits in a mix as they happen: kicks (and, as detail, snares and hats). Every 128 samples
// (2.7 ms at 48 kHz) the newest 1024 samples are measured in nine bands: each band's energy, how
// much it rose over the last 11 and 32 ms, how loud it is against its own recent peak and against
// the whole track's loudness over the last ~8 s (which doesn't fade in a break without kicks). A small
// model learned from real songs (DrumModel.swift, made by tools/showcontrol/drums/train.py against
// drum tracks separated offline) turns those measurements into the chance that a hit just started.

import Accelerate
import Foundation

final class Drums {
    enum Kind: Int { case kick, snare, hat, bass }
    // A hit (its strength 0...1) and when it started, on the clock push() was given. A bass note
    // the model took for a kick comes as .bass.
    var onHit: ((Kind, Float, Double) -> Void)?
    // Each kick the model found, as judged (tools only): its click, and how much its low end's
    // cycles lengthened after it (1: none).
    var onJudged: ((_ click: Float, _ fall: Double, _ kick: Bool, _ at: Double) -> Void)?
    static var clickKick: Float = 9, fallKick = 1.25, judgeHops = 16
    // Every step's measurements and the time they're for, to train the model (tools only).
    var onFeatures: (([Float], Double) -> Void)?

    static let size = 1024, hop = 128
    static let edges: [Double] = [30, 60, 100, 160, 300, 600, 1500, 4000, 8000, 16000]
    static let featureCount = (edges.count - 1) * 5 + 1

    private let setup: FFTSetup
    private let log2n = vDSP_Length(10)
    private var window = [Float](repeating: 0, count: size)
    private var ring = [Float](repeating: 0, count: size)
    private var at = 0, sinceHop = 0, rate = 0.0
    private var clock = 0.0
    private var bandBins: [Range<Int>] = []
    private var history: [[Float]] = []  // the last 13 frames of band energies (dB), newest last
    private var peak: [Float] = []
    private var lastRise: [Float] = []
    private var loudness: Float = -60  // the whole track's level (dB), over ~8 s
    private var lastHit = [Double](repeating: -1, count: 3)
    private var recent: [[Float]] = []  // the model's input: the last few steps' measurements, newest first
    private var x = [Float](repeating: 0, count: size)
    private var re = [Float](repeating: 0, count: size / 2), im = [Float](repeating: 0, count: size / 2)
    private var power = [Float](repeating: 0, count: size / 2)
    // A kick the model found, waiting to be told from a bass note by what its low end does next.
    private struct Pending { var at: Double, strength: Float, click: Float, sent: Bool, hops = 0 }
    private var pending: Pending?
    private var clicks: [Float] = []  // the top bands' rise, the last few steps
    private var lastKick = -1.0
    // The low end (25-180 Hz) as a waveform, and when it last crossed zero upward: its pitch, cycle by cycle.
    private struct Filter {
        var b0: Float = 1, b1: Float = 0, b2: Float = 0, a1: Float = 0, a2: Float = 0, x1: Float = 0, x2: Float = 0, y1: Float = 0, y2: Float = 0
        init() {}
        init(low: Bool, hz: Double, rate: Double) {
            let w = 2 * Double.pi * hz / rate, alpha = sin(w) / (2 * 0.7071), c = cos(w), a0 = 1 + alpha
            b0 = Float((low ? 1 - c : 1 + c) / 2 / a0); b1 = Float((low ? 1 - c : -(1 + c)) / a0); b2 = b0
            a1 = Float(-2 * c / a0); a2 = Float((1 - alpha) / a0)
        }
        mutating func run(_ x: Float) -> Float {
            let y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2 = x1; x1 = x; y2 = y1; y1 = y
            return y
        }
    }
    private var lows = [Filter](repeating: Filter(), count: 3), lowLast: Float = 0, crossings: [Double] = []

    init() {
        setup = vDSP_create_fftsetup(log2n, FFTRadix(kFFTRadix2))!
        vDSP_hann_window(&window, vDSP_Length(Drums.size), Int32(vDSP_HANN_NORM))
    }
    deinit { vDSP_destroy_fftsetup(setup) }

    private func configure(_ r: Double) {
        rate = r
        let hz = r / Double(Drums.size)
        bandBins = (0..<Drums.edges.count - 1).map { b in
            let lo = max(1, Int((Drums.edges[b] / hz).rounded())), hi = min(Drums.size / 2, max(lo + 1, Int((Drums.edges[b + 1] / hz).rounded())))
            return lo..<hi
        }
        history = []
        lows = [Filter(low: true, hz: 180, rate: r), Filter(low: true, hz: 180, rate: r), Filter(low: false, hz: 25, rate: r)]
        peak = [Float](repeating: -100, count: bandBins.count)
        lastRise = [Float](repeating: 0, count: bandBins.count)
    }

    // played: when the first sample was played.
    func push(_ samples: [Float], rate r: Double, played: Double) {
        if r != rate { configure(r) }
        let dt = 1 / r
        clock = played
        for s in samples {
            ring[at] = s
            at = (at + 1) % Drums.size
            clock += dt
            let y = lows[2].run(lows[1].run(lows[0].run(s)))
            if lowLast <= 0 && y > 0 {
                crossings.append(clock - dt * Double(y / (y - lowLast)))
                if crossings.count > 24 { crossings.removeFirst() }
            }
            lowLast = y
            sinceHop += 1
            if sinceHop == Drums.hop {
                sinceHop = 0
                frame()
            }
        }
    }

    private func frame() {
        let n = Drums.size
        for i in 0..<n { x[i] = ring[(at + i) % n] * window[i] }
        re.withUnsafeMutableBufferPointer { rp in
            im.withUnsafeMutableBufferPointer { ip in
                var split = DSPSplitComplex(realp: rp.baseAddress!, imagp: ip.baseAddress!)
                x.withUnsafeBufferPointer { xp in
                    xp.baseAddress!.withMemoryRebound(to: DSPComplex.self, capacity: n / 2) { vDSP_ctoz($0, 2, &split, 1, vDSP_Length(n / 2)) }
                }
                vDSP_fft_zrip(setup, &split, 1, log2n, FFTDirection(FFT_FORWARD))
                vDSP_zvmags(&split, 1, &power, 1, vDSP_Length(n / 2))
            }
        }
        let dt = Float(Double(Drums.hop) / rate)
        let e: [Float] = bandBins.map { r in
            var sum: Float = 0
            for k in r { sum += power[k] }
            return 10 * log10f(sum + 1e-10)
        }
        history.append(e)
        if history.count > 13 { history.removeFirst() }
        guard history.count == 13 else { return }

        // Per band: rise over ~11 ms and ~32 ms (against the quietest frame back then), level against
        // its own peak (falling 4 dB a second), and how the short rise changed since the last step.
        var f = [Float](repeating: 0, count: Drums.featureCount)
        var total: Float = 0
        let level = 10 * log10f(e.reduce(0) { $0 + powf(10, $1 / 10) } + 1e-10)
        if level > -80 { loudness += (level - loudness) * min(1, dt / 8) }
        for b in 0..<e.count {
            var min4: Float = .infinity, min12: Float = .infinity
            for j in 0..<12 {
                let v = history[11 - j][b]
                if j < 4 { min4 = min(min4, v) }
                min12 = min(min12, v)
            }
            let rise = e[b] - min4
            peak[b] = max(e[b], peak[b] - 4 * dt)
            f[b * 5] = rise
            f[b * 5 + 1] = e[b] - min12
            f[b * 5 + 2] = e[b] - peak[b]
            f[b * 5 + 3] = rise - lastRise[b]
            f[b * 5 + 4] = e[b] - loudness
            lastRise[b] = rise
            total += max(0, rise)
        }
        f[Drums.featureCount - 1] = total
        onFeatures?(f, clock)
        // A kick's beater clicks in the top bands and its low end falls in pitch fast (150 Hz down to
        // 50 within a few tens of ms); a bass note holds its pitch. Measured: drums/README.md.
        clicks.insert(max(f[25], f[30], f[35]), at: 0)
        if clicks.count > 4 { clicks.removeLast() }
        if var p = pending {
            p.hops += 1
            pending = p
            if p.hops >= Drums.judgeHops {
                pending = nil
                // The cycles since it started: the last against the first.
                let cs = crossings.filter { $0 >= p.at - 0.004 }
                let fall = cs.count >= 3 ? (cs[cs.count - 1] - cs[cs.count - 2]) / (cs[1] - cs[0]) : 1
                let kick = p.sent || (fall >= Drums.fallKick && p.at - lastKick > 0.1)
                onJudged?(p.click, fall, kick, p.at)
                if !p.sent {
                    if kick { lastKick = p.at }
                    onHit?(kick ? .kick : .bass, p.strength, p.at)
                }
            }
        }
        guard let model = DrumModel.current else { return }
        recent.insert(f, at: 0)
        if recent.count > model.steps { recent.removeLast() }
        guard recent.count == model.steps else { return }
        let p = model.chances(Array(recent.joined()))
        for k in 0..<3 where p[k] >= model.thresholds[k] {
            let gap = k == Kind.hat.rawValue ? 0.04 : 0.07
            guard clock - lastHit[k] > gap else { continue }
            lastHit[k] = clock
            let at = clock - model.lead
            guard k == Kind.kick.rawValue else { onHit?(Kind(rawValue: k)!, min(1, p[k]), at); continue }
            // A clear click goes out now; a low thump alone waits to be judged.
            let click = clicks.max() ?? 0, sent = click >= Drums.clickKick && at - lastKick > 0.1
            if sent { lastKick = at; onHit?(.kick, min(1, p[k]), at) }
            pending = Pending(at: at, strength: min(1, p[k]), click: click, sent: sent)
        }
    }
}
