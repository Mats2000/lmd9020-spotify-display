// Spotify's sound on this Mac: captured (ScreenCaptureKit, audio only), analysed 60 times a second
// into 16 bands and a waveform for the display, and watched sample by sample for kicks, which go
// out the moment they land instead of waiting for the next analysis.

import Accelerate
import CoreMedia
import Foundation
import ScreenCaptureKit

let displayPort: UInt16 = 4210  // the display listens here and announces itself on the next port
let bandCount = 16
let fftSize = 2048
let updatesPerSecond = 60.0
let waveCount = 64

// Mono samples in; 60 times a second, 16 log-spaced bands (40 Hz to 16 kHz) with automatic gain,
// fast attack and slower release; kicks from the bass as they happen (see kick()).
final class Analyzer {
    struct Reading {
        var bands = [UInt8](repeating: 0, count: bandCount)
        var beat: UInt8 = 0
        var wave = [UInt8](repeating: 0, count: waveCount)  // signed bytes
        var playing = false
        var at = Date.distantPast
    }

    private var ring = [Float](repeating: 0, count: fftSize)
    private var at = 0
    private var pending = 0
    private let setup: FFTSetup
    private let log2n = vDSP_Length(log2(Double(fftSize)))
    private var window = [Float](repeating: 0, count: fftSize)
    private var bands = [Float](repeating: 0, count: bandCount)
    private var peakDb: Float = -40
    private var beat: Float = 0
    private var silentFor = 0
    private var sampleRate = 48000.0
    private let lock = NSLock()
    private var last = Reading()
    private var wavePeak: Float = 0.05
    private let send: ([UInt8]) -> Void
    private var chosen: UInt8 = 0
    private var colours: UInt8 = 0
    private var on = true
    private var oneColour = RGB(255, 0, 255)

    // A kick, the moment it's found (on the audio thread): its strength 0...1, when Spotify played
    // it (capture clock, see Sync.now), and the beat's period in seconds once the tempo is sure (else 0).
    var onKick: ((_ strength: Float, _ played: Double, _ period: Double) -> Void)?
    private var sampleTime = 0.0  // when the sample being looked at was played
    // Kicks, snares and hats by the learned model (Drums.swift); the kick code below then only keeps
    // the tempo.
    private let drums = Drums()
    private let learned = DrumModel.current != nil

    var style: UInt8 {  // the visualizer the display should draw
        get { lock.lock(); defer { lock.unlock() }; return chosen }
        set { lock.lock(); chosen = newValue; lock.unlock() }
    }
    var scheme: UInt8 {  // the colours it should draw in
        get { lock.lock(); defer { lock.unlock() }; return colours }
        set { lock.lock(); colours = newValue; lock.unlock() }
    }
    var one: RGB {  // the colour for the display's one-colour scheme
        get { lock.lock(); defer { lock.unlock() }; return oneColour }
        set { lock.lock(); oneColour = newValue; lock.unlock() }
    }
    var enabled: Bool {  // false: the display keeps its normal background
        get { lock.lock(); defer { lock.unlock() }; return on }
        set { lock.lock(); on = newValue; lock.unlock() }
    }

    init(send: @escaping ([UInt8]) -> Void) {
        self.send = send
        setup = vDSP_create_fftsetup(log2n, FFTRadix(kFFTRadix2))!
        vDSP_hann_window(&window, vDSP_Length(fftSize), Int32(vDSP_HANN_NORM))
        drums.onHit = { [weak self] kind, strength, at in
            guard let self else { return }
            switch kind {
            case .kick:
                self.beat = 1
                self.onKick?(strength, at, self.tempoSure ? self.beatPeriod : 0)
            case .snare: self.onAccent?(strength)
            case .hat: self.onAccent?(strength * 0.6)
            }
        }
    }

    // The latest reading (silence once the audio stops).
    var reading: Reading {
        lock.lock()
        defer { lock.unlock() }
        return Date().timeIntervalSince(last.at) < 0.5 ? last : Reading()
    }

    // The capture stopped: tell the display, so it goes back to its background now.
    func quiet() {
        var packet: [UInt8] = Array("LMDV".utf8) + [1, 0, 0, 0]
        packet += [UInt8](repeating: 0, count: bandCount)
        packet.append(style)
        packet.append(scheme)
        packet += [UInt8](repeating: 0, count: waveCount)
        send(packet)
    }

    // Kicks: the kick drum's own band (30-150 Hz; its attack starts high and drops) in 64-sample
    // slices (1.3 ms at 48 kHz), averaged over 5 ms. A kick is a sharp rise there (dB over ~10 ms,
    // not a level, so sustained bass and rumble don't count) near the band's recent peak, and, once
    // the tempo is found (see tempo()), on the beat, so offbeat bass notes don't count either. Snares
    // and hats (the top band rising on its own) are accents: detail, never the main pulse.
    private struct Biquad {
        var b0: Float = 1, b1: Float = 0, b2: Float = 0, a1: Float = 0, a2: Float = 0
        var x1: Float = 0, x2: Float = 0, y1: Float = 0, y2: Float = 0
        init() {}
        init(low: Bool, hz: Double, rate: Double) {
            let w = 2 * Double.pi * hz / rate, alpha = sin(w) / (2 * 0.7071), c = cos(w), a0 = 1 + alpha
            if low {
                b0 = Float((1 - c) / 2 / a0); b1 = Float((1 - c) / a0); b2 = b0
            } else {
                b0 = Float((1 + c) / 2 / a0); b1 = Float(-(1 + c) / a0); b2 = b0
            }
            a1 = Float(-2 * c / a0); a2 = Float((1 - alpha) / a0)
        }
        mutating func run(_ x: Float) -> Float {
            let y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2 = x1; x1 = x; y2 = y1; y1 = y
            return y
        }
    }
    private var kickLow = Biquad(), kickHigh = Biquad(), top = Biquad(), filtersAt = 0.0
    private var lowSum: Float = 0, topSum: Float = 0, sliceAt = 0
    private var lowRecent = [Float](repeating: 0, count: 4), topRecent = [Float](repeating: 0, count: 4), recentAt = 0
    private var lowDb = [Float](repeating: -120, count: 16), topDb = [Float](repeating: -120, count: 16), dbAt = 0
    private var lowPeak: Float = 1e-6, topPeak: Float = 1e-6
    private var armed = 0, armedBest: Float = 0, armedEnergy: Float = 0
    private var sinceKick = 1e9, sinceAccent = 1e9
    private var kickLevel: Float = 0  // recent kicks' own peak energy
    private var clock = 0.0, lowMean: Float = 0, candidates: [(at: Double, energy: Float)] = []
    // The tempo: how strongly the band rose, in 5 ms bins over the last 4 s; twice a second the
    // period and phase of its strongest repeating rise (the kick) are found from it.
    private var onsets = [Float](repeating: 0, count: 768), onsetAt = 0, binRise: Float = 0, binSlices = 0, binsSinceTempo = 0
    private var beatPeriod = 0.0, nextBeat = 0.0, tempoSure = false
    var onAccent: ((Float) -> Void)?

    private func kick(_ s: Float) {
        if filtersAt != sampleRate {
            filtersAt = sampleRate
            kickLow = Biquad(low: true, hz: 150, rate: sampleRate)
            kickHigh = Biquad(low: false, hz: 30, rate: sampleRate)
            top = Biquad(low: false, hz: 2500, rate: sampleRate)
        }
        let l = kickHigh.run(kickLow.run(s)), t = top.run(s)
        lowSum += l * l
        topSum += t * t
        sliceAt += 1
        guard sliceAt == 64 else { return }
        // Over the last 4 slices (5 ms): a single slice is shorter than one cycle down here, so on
        // its own it jumps about even in plain noise.
        lowRecent[recentAt] = lowSum / 64
        topRecent[recentAt] = topSum / 64
        recentAt = (recentAt + 1) % 4
        let lowE = lowRecent.reduce(0, +) / 4, topE = topRecent.reduce(0, +) / 4
        lowSum = 0; topSum = 0; sliceAt = 0
        let slice = 64 / sampleRate
        clock += slice
        sinceKick += slice
        sinceAccent += slice

        // Each band's peak, falling back over ~2 s, as the yardstick for "loud enough".
        lowPeak = max(lowE, lowPeak * Float(exp(-slice / 2)), 1e-9)
        topPeak = max(topE, topPeak * Float(exp(-slice / 2)), 1e-9)
        let ld = 10 * log10f(lowE + 1e-12), td = 10 * log10f(topE + 1e-12)
        // The rise: now against the quietest of 3-12 slices back (4-16 ms).
        var lowBase: Float = 0, topBase: Float = 0
        for k in 3...12 {
            let i = (dbAt - k + 32) % 16
            lowBase = k == 3 ? lowDb[i] : min(lowBase, lowDb[i])
            topBase = k == 3 ? topDb[i] : min(topBase, topDb[i])
        }
        lowDb[dbAt] = ld
        topDb[dbAt] = td
        dbAt = (dbAt + 1) % 16
        let lowRise = ld - lowBase, topRise = td - topBase

        if armed > 0 {  // waiting for the kick's top
            armedBest = max(armedBest, lowRise)
            armedEnergy = max(armedEnergy, lowE)
            armed -= 1
            guard armed == 0 else { return }
            // On the beat once the tempo is found (an offbeat bass note isn't a kick), unless far louder
            // than the kicks; before that, at least half the strongest hit of the last 1.5 s.
            candidates.removeAll { clock - $0.at > 1.5 }
            candidates.append((clock, armedEnergy))
            let strongest = candidates.map(\.energy).max() ?? armedEnergy
            if tempoSure {
                var d = (clock - nextBeat).truncatingRemainder(dividingBy: beatPeriod)
                if d < 0 { d += beatPeriod }
                let off = min(d, beatPeriod - d)
                guard off < beatPeriod * 0.15 || armedEnergy > kickLevel * 2 else { return }
            } else {
                guard armedEnergy >= strongest * 0.5 else { return }
            }
            kickLevel = kickLevel == 0 ? armedEnergy : kickLevel + (armedEnergy - kickLevel) * 0.25
            sinceKick = 0
            beat = 1
            // Its start: the armed slices and the 5 ms average back.
            if !learned {
                onKick?(min(1, sqrtf(armedEnergy / max(kickLevel, 1e-9)) * 0.8) * min(1, 0.55 + armedBest / 40),
                        sampleTime - 6 * slice, tempoSure ? beatPeriod : 0)
            }
            return
        }
        kickLevel *= Float(exp(-slice / 8))  // forgets slowly, so a quieter section is still heard
        lowMean += (lowE - lowMean) * Float(slice / 0.4)  // the band's own level, so hiss never counts
        tempo(max(0, lowRise - 3) * (lowE > lowMean ? 1 : 0.3))
        let hold = tempoSure ? max(0.11, beatPeriod * 0.3) : 0.11
        if sinceKick > hold && lowRise > 8 && lowE > lowPeak * 0.15 && lowE > lowMean * 2.5 && lowE > 1e-6 {
            armed = 2
            armedBest = lowRise
            armedEnergy = lowE
        } else if sinceAccent > 0.07 && sinceKick > 0.03 && topRise > 9 && topE > topPeak * 0.2 && topE > 1e-8 {
            sinceAccent = 0
            if !learned { onAccent?(min(1, sqrtf(topE / topPeak))) }
        }
    }

    private func tempo(_ rise: Float) {
        binRise = max(binRise, rise)
        binSlices += 1
        guard binSlices == 4 else { return }
        onsets[onsetAt] = binRise
        onsetAt = (onsetAt + 1) % onsets.count
        binRise = 0
        binSlices = 0
        binsSinceTempo += 1
        guard binsSinceTempo >= 96 else { return }
        binsSinceTempo = 0
        let bin = 256 / sampleRate, n = onsets.count
        let o = (0..<n).map { onsets[(onsetAt + $0) % n] }  // oldest first
        let energy = o.reduce(0) { $0 + $1 * $1 }
        guard energy > 0 else { tempoSure = false; return }
        // Period: the autocorrelation's best lag between 60 and 190 BPM, leaning toward ~120.
        var best = 0, bestScore: Float = 0
        for lag in Int(0.315 / bin)...Int(1.0 / bin) {
            var sum: Float = 0
            for i in lag..<n { sum += o[i] * o[i - lag] }
            let lean = Float(exp(-pow(log2(Double(lag) * bin / 0.5), 2) / 2))
            if sum * lean > bestScore { bestScore = sum * lean; best = lag }
        }
        guard best > 0 else { tempoSure = false; return }
        var peak: Float = 0, at = best
        for lag in max(1, best - 1)...(best + 1) {  // a finer look either side
            var sum: Float = 0
            for i in lag..<n { sum += o[i] * o[i - lag] }
            if sum > peak { peak = sum; at = lag }
        }
        tempoSure = peak > energy * 0.25
        // Phase: where along the period the rises stack up most.
        var phase = 0, phaseScore: Float = -1
        for p in 0..<at {
            var sum: Float = 0
            var i = n - 1 - p
            while i >= 1 {
                sum += max(o[i], o[i - 1], i + 1 < n ? o[i + 1] : 0)
                i -= at
            }
            if sum > phaseScore { phaseScore = sum; phase = p }
        }
        beatPeriod = Double(at) * bin
        nextBeat = clock - Double(phase) * bin
    }

    // played: when the first sample was played (capture clock).
    func push(_ samples: [Float], rate: Double, played: Double) {
        if learned { drums.push(samples, rate: rate, played: played) }
        sampleRate = rate
        let hop = Int(rate / updatesPerSecond)
        sampleTime = played
        for s in samples {
            sampleTime += 1 / rate
            ring[at] = s
            at = (at + 1) % fftSize
            kick(s)
            pending += 1
            if pending >= hop {
                pending = 0
                analyze()
            }
        }
    }

    // The latest 256 samples, every 4th: the waveform's shape, scaled by a slow peak so quiet
    // passages still fill the bytes.
    private func waveform() -> [UInt8] {
        var peak: Float = 0
        var picked = [Float](repeating: 0, count: waveCount)
        for i in 0..<waveCount {
            let v = ring[(at - waveCount * 4 + i * 4 + fftSize * 2) % fftSize]
            picked[i] = v
            peak = max(peak, abs(v))
        }
        wavePeak = max(peak, wavePeak * 0.97, 0.01)
        return picked.map { UInt8(bitPattern: Int8(max(-127, min(127, $0 / wavePeak * 120)))) }
    }

    // The packet the display gets, from the latest analysis and the beat as it is now.
    private var lastPacket: [UInt8] = []

    // Sent straight away on a kick, rather than at the next analysis.
    func sendKick() {
        lock.lock()
        var p = lastPacket
        lock.unlock()
        guard p.count > 7 else { return }
        p[7] = 255
        send(p)
    }

    private func analyze() {
        var frame = [Float](repeating: 0, count: fftSize)
        for i in 0..<fftSize { frame[i] = ring[(at + i) % fftSize] * window[i] }
        var rms: Float = 0
        vDSP_rmsqv(frame, 1, &rms, vDSP_Length(fftSize))

        let half = fftSize / 2
        var real = [Float](repeating: 0, count: half), imag = [Float](repeating: 0, count: half)
        var power = [Float](repeating: 0, count: half)
        real.withUnsafeMutableBufferPointer { re in
            imag.withUnsafeMutableBufferPointer { im in
                var split = DSPSplitComplex(realp: re.baseAddress!, imagp: im.baseAddress!)
                frame.withUnsafeBufferPointer {
                    $0.baseAddress!.withMemoryRebound(to: DSPComplex.self, capacity: half) {
                        vDSP_ctoz($0, 2, &split, 1, vDSP_Length(half))
                    }
                }
                vDSP_fft_zrip(setup, &split, 1, log2n, FFTDirection(FFT_FORWARD))
                vDSP_zvmags(&split, 1, &power, 1, vDSP_Length(half))
            }
        }

        let binHz = Float(sampleRate) / Float(fftSize)
        var levels = [Float](repeating: 0, count: bandCount)
        var loudest: Float = -200
        for b in 0..<bandCount {
            let f0 = 40 * powf(400, Float(b) / Float(bandCount)), f1 = 40 * powf(400, Float(b + 1) / Float(bandCount))
            let i0 = max(1, Int(f0 / binHz)), i1 = max(i0 + 1, Int(f1 / binHz))
            var sum: Float = 0
            for i in i0..<min(i1, half) { sum += power[i] }
            // Lift the highs a little: music has far less energy up there.
            let db = 10 * log10f(sum + 1e-12) + 3 * log2f(sqrtf(f0 * f1) / 250)
            levels[b] = db
            loudest = max(loudest, db)
        }
        // Automatic gain: the loudest band sets the top, falling back slowly.
        peakDb = max(peakDb - 0.02, loudest)
        let floorDb = peakDb - 42
        for b in 0..<bandCount {
            let v = min(1, max(0, (levels[b] - floorDb) / (peakDb - floorDb)))
            bands[b] = v > bands[b] ? v : bands[b] * 0.82 + v * 0.18
        }
        beat *= 0.85

        silentFor = rms > 1e-4 ? 0 : silentFor + 1
        let playing = silentFor < Int(updatesPerSecond * 1.5)
        let quantised = bands.map { UInt8($0 * 255) }
        let shown = playing && enabled
        var packet: [UInt8] = Array("LMDV".utf8) + [1, shown ? 1 : 0, UInt8(min(255, rms * 2000)), UInt8(beat * 255)]
        packet += quantised
        packet.append(style)
        packet.append(scheme)
        let wave = waveform()
        packet += wave
        let c = one
        packet += [c.r, c.g, c.b]
        send(packet)

        lock.lock()
        lastPacket = packet
        last = Reading(bands: quantised, beat: UInt8(beat * 255), wave: wave, playing: playing, at: Date())
        lock.unlock()
    }
}

enum CaptureError: Error { case noSpotify, noDisplay }

// Spotify's audio only, through ScreenCaptureKit (the picture is a token 2x2 at 1 fps).
// `defaults write com.sentiainstruments.lmd-visualizer recordSeconds -int 180`: the next capture's
// first seconds of sound (mono, as the analyzer hears it) go to ~/Library/Logs/Show Control as a
// WAV, with the time its first sample was played, to study the drum detection against.
final class Recorder {
    private var samples: [Float] = []
    private var first = 0.0, rate = 48000.0
    private let want: Double
    private var done = false

    init?() {
        let s = UserDefaults.standard.integer(forKey: "recordSeconds")
        guard s > 0 else { return nil }
        want = Double(s)
    }

    func add(_ mono: [Float], rate r: Double, played: Double) {
        guard !done else { return }
        if samples.isEmpty { first = played; rate = r }
        samples += mono
        guard Double(samples.count) / rate >= want else { return }
        done = true
        let logs = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0].appendingPathComponent("Logs/Show Control")
        let name = "recording-\(Int(Date().timeIntervalSince1970))"
        var wav = Data()
        func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { wav.append(contentsOf: $0) } }
        func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { wav.append(contentsOf: $0) } }
        let bytes = UInt32(samples.count * 4)
        wav.append(contentsOf: Array("RIFF".utf8)); u32(36 + bytes); wav.append(contentsOf: Array("WAVEfmt ".utf8))
        u32(16); u16(3); u16(1); u32(UInt32(rate)); u32(UInt32(rate) * 4); u16(4); u16(32)  // 32-bit float, mono
        wav.append(contentsOf: Array("data".utf8)); u32(bytes)
        samples.withUnsafeBufferPointer { wav.append(Data(buffer: $0)) }
        try? wav.write(to: logs.appendingPathComponent(name + ".wav"))
        try? String(format: "%.6f\n", first).write(to: logs.appendingPathComponent(name + ".start"), atomically: true, encoding: .utf8)
        samples = []
    }
}

final class Capture: NSObject, SCStreamOutput, SCStreamDelegate {
    private var recorder = Recorder()
    private(set) var running = false
    private var stream: SCStream?
    private let analyzer: Analyzer
    private let queue = DispatchQueue(label: "audio", qos: .userInteractive)

    init(analyzer: Analyzer) { self.analyzer = analyzer }

    func start() async throws {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        guard let app = content.applications.first(where: { $0.bundleIdentifier == "com.spotify.client" }) else {
            throw CaptureError.noSpotify
        }
        guard let display = content.displays.first else { throw CaptureError.noDisplay }
        let filter = SCContentFilter(display: display, including: [app], exceptingWindows: [])
        let config = SCStreamConfiguration()
        config.capturesAudio = true
        config.sampleRate = 48000
        config.channelCount = 2
        config.excludesCurrentProcessAudio = true
        config.width = 2
        config.height = 2
        config.minimumFrameInterval = CMTime(value: 1, timescale: 1)
        let s = SCStream(filter: filter, configuration: config, delegate: self)
        try s.addStreamOutput(self, type: .audio, sampleHandlerQueue: queue)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        try await s.startCapture()
        stream = s
        running = true
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer buffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .audio, buffer.isValid, let format = buffer.formatDescription,
              let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(format)?.pointee else { return }
        let planar = asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0
        let channels = Int(asbd.mChannelsPerFrame)
        var mono: [Float] = []
        try? buffer.withAudioBufferList { list, _ in
            if planar {
                let frames = Int(list[0].mDataByteSize) / 4
                mono = [Float](repeating: 0, count: frames)
                for ch in list {
                    guard let p = ch.mData?.assumingMemoryBound(to: Float.self) else { continue }
                    for i in 0..<frames { mono[i] += p[i] / Float(list.count) }
                }
            } else if let p = list[0].mData?.assumingMemoryBound(to: Float.self) {
                let frames = Int(list[0].mDataByteSize) / 4 / max(1, channels)
                mono = (0..<frames).map { i in (0..<channels).reduce(0) { $0 + p[i * channels + $1] } / Float(channels) }
            }
        }
        guard !mono.isEmpty else { return }
        // The buffer's timestamp is when Spotify played it; how long ago that was is the capture's lag.
        let now = Sync.now, length = Double(mono.count) / asbd.mSampleRate
        let pts = CMSampleBufferGetPresentationTimeStamp(buffer).seconds
        let lag = now - pts - length
        let played = pts.isFinite && lag > -0.05 && lag < 1.5 ? pts : now - length
        if played == pts { Sync.captureLag += (max(0, lag) - Sync.captureLag) * 0.05 }
        Sync.bufferLength += (length - Sync.bufferLength) * 0.05
        analyzer.push(mono, rate: asbd.mSampleRate, played: played)
        recorder?.add(mono, rate: asbd.mSampleRate, played: played)
    }

    func stop() async {
        guard let s = stream else { return }
        stream = nil
        running = false
        try? await s.stopCapture()
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        self.stream = nil
        running = false
    }
}
