// Drum hits in a mix as they happen: kicks (and, as detail, snares and hats). Every 128 samples
// (2.7 ms at 48 kHz) the newest 1024 samples are measured in nine bands: each band's energy, how
// much it rose over the last 11 and 32 ms, how loud it is against its own recent peak and against
// the whole track's loudness over the last ~8 s (which doesn't fade in a break without kicks). A small
// model learned from real songs (DrumModel.swift, made by tools/showcontrol/drums/train.py against
// drum tracks separated offline) turns those measurements into the chance that a hit just started.

import Accelerate
import Foundation

final class Drums {
    enum Kind: Int { case kick, snare, hat }
    // A hit (its strength 0...1) and when it started, on the clock push() was given.
    var onHit: ((Kind, Float, Double) -> Void)?
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
        guard let model = DrumModel.current else { return }
        recent.insert(f, at: 0)
        if recent.count > model.steps { recent.removeLast() }
        guard recent.count == model.steps else { return }
        let p = model.chances(Array(recent.joined()))
        for k in 0..<3 where p[k] >= model.thresholds[k] {
            let gap = k == Kind.hat.rawValue ? 0.04 : 0.07
            guard clock - lastHit[k] > gap else { continue }
            lastHit[k] = clock
            onHit?(Kind(rawValue: k)!, min(1, p[k]), clock - model.lead)
        }
    }
}
