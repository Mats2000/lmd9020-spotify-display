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

    // A kick (strength 0...1), the moment it's heard: called on the audio thread.
    var onKick: ((Float) -> Void)?

    var style: UInt8 {  // the visualizer the display should draw
        get { lock.lock(); defer { lock.unlock() }; return chosen }
        set { lock.lock(); chosen = newValue; lock.unlock() }
    }
    var scheme: UInt8 {  // the colours it should draw in
        get { lock.lock(); defer { lock.unlock() }; return colours }
        set { lock.lock(); colours = newValue; lock.unlock() }
    }
    var enabled: Bool {  // false: the display keeps its normal background
        get { lock.lock(); defer { lock.unlock() }; return on }
        set { lock.lock(); on = newValue; lock.unlock() }
    }

    init(send: @escaping ([UInt8]) -> Void) {
        self.send = send
        setup = vDSP_create_fftsetup(log2n, FFTRadix(kFFTRadix2))!
        vDSP_hann_window(&window, vDSP_Length(fftSize), Int32(vDSP_HANN_NORM))
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

    // Kicks: the bass (two poles at ~150 Hz) in 64-sample slices, 1.3 ms at 48 kHz, against its own
    // recent level. A jump fires at once; then nothing for 120 ms.
    private var lowA: Float = 0, lowB: Float = 0, slice: Float = 0, sliceAt = 0
    private var bassLevel: Float = 1e-5, holdSlices = 0

    private func kick(_ s: Float) {
        let k = Float(1 - exp(-2 * Double.pi * 150 / sampleRate))
        lowA += k * (s - lowA)
        lowB += k * (lowA - lowB)
        slice += lowB * lowB
        sliceAt += 1
        guard sliceAt == 64 else { return }
        let energy = slice / 64
        slice = 0
        sliceAt = 0
        if holdSlices > 0 { holdSlices -= 1 }
        if holdSlices == 0 && energy > bassLevel * 2.4 && energy > 2e-6 {
            holdSlices = Int(0.12 * sampleRate / 64)
            beat = 1
            onKick?(min(1, energy / (bassLevel * 8)))
        }
        bassLevel += (energy - bassLevel) * 0.006  // ~0.2 s
    }

    func push(_ samples: [Float], rate: Double) {
        sampleRate = rate
        let hop = Int(rate / updatesPerSecond)
        for s in samples {
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
        send(packet)

        lock.lock()
        lastPacket = packet
        last = Reading(bands: quantised, beat: UInt8(beat * 255), wave: wave, playing: playing, at: Date())
        lock.unlock()
    }
}

enum CaptureError: Error { case noSpotify, noDisplay }

// Spotify's audio only, through ScreenCaptureKit (the picture is a token 2x2 at 1 fps).
final class Capture: NSObject, SCStreamOutput, SCStreamDelegate {
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
        if !mono.isEmpty { analyzer.push(mono, rate: asbd.mSampleRate) }
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
