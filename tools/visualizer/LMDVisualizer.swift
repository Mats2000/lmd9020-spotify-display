// LMD Visualizer: listens to Spotify on this Mac and sends the music's frequency bands to the
// LMD-9020 display, which draws its visualizer from them. The window previews the display
// live, drawn by the firmware's own scene code (demo.cpp). Build with build.sh.
//
// macOS asks once to allow screen and system audio recording (that's how one app's sound is
// captured; nothing of the screen is used), access to the local network, and control of
// Spotify (for the song and cover the preview shows).

import Accelerate
import AppKit
import CoreMedia
import ScreenCaptureKit
import ServiceManagement

let displayPort: UInt16 = 4210  // the display listens here and announces itself on the next port
let bandCount = 16
let fftSize = 2048
let updatesPerSecond = 60.0
let waveCount = 64
let styles = ["Classic Glow", "Spectrum", "Pulse Halo", "Lava", "Rave", "Fractal"]  // VizStyle order
let schemes = ["Cover Colours", "Sentia", "Ice", "Amber", "Soft Club"]  // ColourScheme order

// MARK: - Network

// Learns the display's address from its broadcast ("LMD9020 1" every 2 s), then sends it
// packets: "LMDV", version 1, flags (bit 0: music playing), level, beat, 16 bands, style, colour scheme,
// then 64 samples of the waveform (signed bytes, about 5 ms, scaled to fill).
final class Link {
    private let sock: Int32
    private var target: sockaddr_in?
    private let lock = NSLock()
    private(set) var address: String?

    init(host: String?) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        var yes: Int32 = 1
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &yes, socklen_t(MemoryLayout<Int32>.size))
        var local = sockaddr_in()
        local.sin_family = sa_family_t(AF_INET)
        local.sin_port = (displayPort + 1).bigEndian
        local.sin_addr.s_addr = INADDR_ANY
        _ = withUnsafePointer(to: &local) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(sock, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        if let host, !host.isEmpty {
            var a = sockaddr_in()
            a.sin_family = sa_family_t(AF_INET)
            a.sin_port = displayPort.bigEndian
            inet_pton(AF_INET, host, &a.sin_addr)
            target = a
            address = host
        }
        Thread { self.listen() }.start()
    }

    private func listen() {
        var buf = [UInt8](repeating: 0, count: 64)
        while true {
            var from = sockaddr_in()
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = withUnsafeMutablePointer(to: &from) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(sock, &buf, buf.count, 0, $0, &len) }
            }
            guard n >= 7, String(decoding: buf[0..<7], as: UTF8.self) == "LMD9020" else { continue }
            from.sin_port = displayPort.bigEndian
            var text = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
            inet_ntop(AF_INET, &from.sin_addr, &text, socklen_t(INET_ADDRSTRLEN))
            lock.lock()
            target = from
            address = String(cString: text)
            lock.unlock()
        }
    }

    func send(_ packet: [UInt8]) {
        lock.lock()
        guard var t = target else {
            lock.unlock()
            return
        }
        lock.unlock()
        _ = withUnsafePointer(to: &t) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                sendto(sock, packet, packet.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }
}

// MARK: - Analysis

// Mono samples in; 60 times a second, 16 log-spaced bands (40 Hz to 16 kHz) with automatic
// gain, fast attack and slower release, plus a beat from jumps in the bass.
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
    private var bassAverage: Float = 0
    private var beat: Float = 0
    private var sinceBeat = 0
    private var silentFor = 0
    private var sampleRate = 48000.0
    private let lock = NSLock()
    private var last = Reading()
    private var wavePeak: Float = 0.05
    private let send: ([UInt8]) -> Void
    private var chosen: UInt8 = 0
    private var colours: UInt8 = 0
    private var on = true

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

    // The latest reading, for the preview and the meter (silence once the audio stops).
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

    func push(_ samples: [Float], rate: Double) {
        sampleRate = rate
        let hop = Int(rate / updatesPerSecond)
        for s in samples {
            ring[at] = s
            at = (at + 1) % fftSize
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

        let bass = (bands[0] + bands[1] + bands[2]) / 3
        sinceBeat += 1
        if bass > bassAverage * 1.3 + 0.08 && sinceBeat > 7 {
            beat = 1
            sinceBeat = 0
        } else {
            beat *= 0.85
        }
        bassAverage = bassAverage * 0.95 + bass * 0.05

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
        last = Reading(bands: quantised, beat: UInt8(beat * 255), wave: wave, playing: playing, at: Date())
        lock.unlock()
    }
}

// MARK: - Capture

enum CaptureError: Error { case noSpotify, noDisplay }

// Spotify's audio only, through ScreenCaptureKit (the picture is a token 2x2 at 1 fps).
final class Capture: NSObject, SCStreamOutput, SCStreamDelegate {
    private(set) var running = false
    private var stream: SCStream?
    private let analyzer: Analyzer
    private let queue = DispatchQueue(label: "audio")

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

// MARK: - Govee lamps

// Govee's LAN API (turn on "LAN Control" for the lamp in the Govee Home app): lamps answer a
// scan sent to 239.255.255.250:4001 on that group's port 4002 (so the listener must join the
// group), and take commands on their port 4003. Their own state is read first and put back
// when the show stops.
final class GoveeLights {
    private let sock: Int32
    private let lock = NSLock()
    private var found: [String: String] = [:]  // address -> model
    private var colour: [UInt8] = []
    private var brightness = -1
    private var brightnessAt = Date.distantPast
    private var saved: [String: [String: Any]] = [:]  // address -> the lamp's own colour and brightness
    private(set) var showing = false

    init() {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        var yes: Int32 = 1
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &yes, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, socklen_t(MemoryLayout<Int32>.size))
        var local = sockaddr_in()
        local.sin_family = sa_family_t(AF_INET)
        local.sin_port = UInt16(4002).bigEndian
        local.sin_addr.s_addr = INADDR_ANY
        _ = withUnsafePointer(to: &local) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(sock, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        var group = ip_mreq(imr_multiaddr: in_addr(s_addr: inet_addr("239.255.255.250")), imr_interface: in_addr(s_addr: INADDR_ANY))
        setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, socklen_t(MemoryLayout<ip_mreq>.size))
        Thread { self.listen() }.start()
    }

    var lamps: [String: String] {
        lock.lock()
        defer { lock.unlock() }
        return found
    }

    func discover() {
        let scan = #"{"msg":{"cmd":"scan","data":{"account_topic":"reserve"}}}"#
        send(scan, to: "239.255.255.250", port: 4001)
        send(scan, to: "255.255.255.255", port: 4001)
    }

    // The whole lamp: a colour (sent when it changes) and a brightness 1...100 (at most ten a second).
    func set(_ rgb: [UInt8], brightness b: Int) {
        let targets = Array(lamps.keys)
        guard !targets.isEmpty else { return }
        if rgb != colour {
            colour = rgb
            let cmd = #"{"msg":{"cmd":"colorwc","data":{"color":{"r":\#(rgb[0]),"g":\#(rgb[1]),"b":\#(rgb[2])},"colorTemInKelvin":0}}}"#
            for ip in targets { send(cmd, to: ip, port: 4003) }
        }
        let b = max(1, min(100, b))
        if abs(b - brightness) >= 4 && Date().timeIntervalSince(brightnessAt) > 0.1 {
            brightness = b
            brightnessAt = Date()
            let cmd = #"{"msg":{"cmd":"brightness","data":{"value":\#(b)}}}"#
            for ip in targets { send(cmd, to: ip, port: 4003) }
        }
    }

    // Starting the show: remember each lamp as it is, and make sure it's on.
    func start() {
        guard !showing else { return }
        showing = true
        colour = []
        brightness = -1
        for ip in lamps.keys {
            send(#"{"msg":{"cmd":"devStatus","data":{}}}"#, to: ip, port: 4003)
            send(#"{"msg":{"cmd":"turn","data":{"value":1}}}"#, to: ip, port: 4003)
        }
    }

    // Stopping it: each lamp back to its own colour, brightness and on or off.
    func stop() {
        guard showing else { return }
        showing = false
        lock.lock()
        let states = saved
        lock.unlock()
        for (ip, st) in states {
            if let c = st["color"] as? [String: Any], let r = c["r"] as? Int, let g = c["g"] as? Int, let b = c["b"] as? Int {
                let k = st["colorTemInKelvin"] as? Int ?? 0
                send(#"{"msg":{"cmd":"colorwc","data":{"color":{"r":\#(r),"g":\#(g),"b":\#(b)},"colorTemInKelvin":\#(k)}}}"#, to: ip, port: 4003)
            }
            if let level = st["brightness"] as? Int { send(#"{"msg":{"cmd":"brightness","data":{"value":\#(level)}}}"#, to: ip, port: 4003) }
            if st["onOff"] as? Int == 0 { send(#"{"msg":{"cmd":"turn","data":{"value":0}}}"#, to: ip, port: 4003) }
        }
    }

    private func send(_ text: String, to host: String, port: UInt16) {
        var a = sockaddr_in()
        a.sin_family = sa_family_t(AF_INET)
        a.sin_port = port.bigEndian
        inet_pton(AF_INET, host, &a.sin_addr)
        let bytes = Array(text.utf8)
        _ = withUnsafePointer(to: &a) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                sendto(sock, bytes, bytes.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    private func listen() {
        var buf = [UInt8](repeating: 0, count: 2048)
        while true {
            var from = sockaddr_in()
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = withUnsafeMutablePointer(to: &from) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(sock, &buf, buf.count, 0, $0, &len) }
            }
            guard n > 0,
                  let json = try? JSONSerialization.jsonObject(with: Data(buf[0..<n])) as? [String: Any],
                  let msg = json["msg"] as? [String: Any], let data = msg["data"] as? [String: Any] else { continue }
            var text = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
            inet_ntop(AF_INET, &from.sin_addr, &text, socklen_t(INET_ADDRSTRLEN))
            if msg["cmd"] as? String == "devStatus" {  // a lamp's own state, to put back later
                lock.lock()
                if saved[String(cString: text)] == nil { saved[String(cString: text)] = data }
                lock.unlock()
                continue
            }
            guard msg["cmd"] as? String == "scan" else { continue }
            let ip = (data["ip"] as? String) ?? String(cString: text)
            lock.lock()
            let isNew = found[ip] == nil
            found[ip] = (data["sku"] as? String) ?? "Govee"
            lock.unlock()
            if isNew { DispatchQueue.main.async { self.onFound?() } }
        }
    }

    var onFound: (() -> Void)?
}

// The cover's strongest colours (up to three, 40 degrees apart), bright, for the lamps.
func coverColours(_ image: CGImage) -> [[UInt8]] {
    let size = 24
    let px = rgb(image, size: size)
    var weight = [Double](repeating: 0, count: 36), sat = [Double](repeating: 0, count: 36)
    for i in 0..<(size * size) {
        let r = Double(px[i * 3]) / 255, g = Double(px[i * 3 + 1]) / 255, b = Double(px[i * 3 + 2]) / 255
        let mx = max(r, g, b), mn = min(r, g, b), c = mx - mn
        guard c > 0.1, mx > 0.15 else { continue }
        var h = mx == r ? (g - b) / c : (mx == g ? (b - r) / c + 2 : (r - g) / c + 4)
        h = (h * 60).truncatingRemainder(dividingBy: 360)
        if h < 0 { h += 360 }
        let bin = Int(h / 10) % 36
        weight[bin] += c * c
        sat[bin] += c * c * c / mx
    }
    var picked: [Int] = []
    while picked.count < 3 {
        var best = -1
        for i in 0..<36 where !picked.contains(where: { min(abs($0 - i), 36 - abs($0 - i)) < 4 }) {
            if best < 0 || weight[i] > weight[best] { best = i }
        }
        guard best >= 0, weight[best] > 0, picked.isEmpty || weight[best] > weight[picked[0]] / 6 else { break }
        picked.append(best)
    }
    if picked.isEmpty { return [[255, 236, 210]] }  // a grey cover: warm white
    return picked.map { bin in
        let s = max(0.6, min(1, sat[bin] / max(weight[bin], 1e-6)))
        let h = Double(bin) * 10 + 5, c = s, x = c * (1 - abs((h / 60).truncatingRemainder(dividingBy: 2) - 1)), m = 1 - c
        let (r, g, b): (Double, Double, Double) = h < 60 ? (c, x, 0) : h < 120 ? (x, c, 0) : h < 180 ? (0, c, x) : h < 240 ? (0, x, c) : h < 300 ? (x, 0, c) : (c, 0, x)
        return [UInt8((r + m) * 255), UInt8((g + m) * 255), UInt8((b + m) * 255)]
    }
}

// The lamps' colours for each fixed scheme (ColourScheme order, after Cover Colours).
let schemeLampColours: [[[UInt8]]] = [
    [[255, 0, 255], [148, 171, 249], [106, 34, 192]],    // Sentia: pink, light blue, purple
    [[120, 188, 210], [197, 235, 245], [40, 140, 200]],  // Ice
    [[250, 160, 50], [170, 70, 10], [255, 210, 140]],     // Amber
    [[150, 130, 210], [150, 220, 200], [250, 200, 225]], // Soft Club
]

// MARK: - Spotify's song and cover, for the preview

final class NowPlayingWatcher {
    struct Track: Equatable {
        var title = "", artist = "", artwork = ""
    }
    private(set) var track = Track()
    private(set) var playing = false, paused = false
    private(set) var reachable = true  // false: not allowed to ask Spotify
    var onNewTrack: ((Track, CGImage?) -> Void)?

    private let script = NSAppleScript(source: """
        if application "Spotify" is running then
            tell application "Spotify"
                if player state is stopped then return "stopped"
                return (player state as string) & linefeed & (name of current track) & linefeed & ¬
                    (artist of current track) & linefeed & (artwork url of current track)
            end tell
        end if
        return "stopped"
        """)

    func poll() {
        var error: NSDictionary?
        let result = script?.executeAndReturnError(&error)
        reachable = error == nil
        guard let text = result?.stringValue else { return }
        let parts = text.components(separatedBy: "\n")
        playing = parts.first == "playing"
        paused = parts.first == "paused"
        guard parts.count >= 4 else { return }
        let next = Track(title: parts[1], artist: parts[2], artwork: parts[3])
        guard next != track else { return }
        track = next
        guard let url = URL(string: next.artwork) else {
            onNewTrack?(next, nil)
            return
        }
        URLSession.shared.dataTask(with: url) { data, _, _ in
            let image = data.flatMap { NSImage(data: $0)?.cgImage(forProposedRect: nil, context: nil, hints: nil) }
            DispatchQueue.main.async { if self.track == next { self.onNewTrack?(next, image) } }
        }.resume()
    }
}

func rgb(_ image: CGImage, size: Int) -> [UInt8] {
    var rgba = [UInt8](repeating: 0, count: size * size * 4)
    rgba.withUnsafeMutableBytes { buf in
        let ctx = CGContext(data: buf.baseAddress, width: size, height: size, bitsPerComponent: 8, bytesPerRow: size * 4,
                            space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
        ctx.interpolationQuality = .high
        ctx.draw(image, in: CGRect(x: 0, y: 0, width: size, height: size))
    }
    var out = [UInt8](repeating: 0, count: size * size * 3)
    for i in 0..<(size * size) {
        out[i * 3] = rgba[i * 4]
        out[i * 3 + 1] = rgba[i * 4 + 1]
        out[i * 3 + 2] = rgba[i * 4 + 2]
    }
    return out
}

// MARK: - Window

func sora(_ weight: String, _ size: CGFloat) -> NSFont {
    NSFont(name: "Sora-\(weight)", size: size) ?? NSFont.systemFont(ofSize: size)
}

let ink = NSColor(calibratedRed: 0.95, green: 0.93, blue: 0.90, alpha: 1)
let dimInk = NSColor(calibratedRed: 0.62, green: 0.60, blue: 0.58, alpha: 1)
let paper = NSColor(calibratedRed: 0.07, green: 0.06, blue: 0.07, alpha: 1)
let glowInk = NSColor(calibratedRed: 0.95, green: 0.55, blue: 0.30, alpha: 1)

final class Meter: NSView {
    var levels = [UInt8](repeating: 0, count: bandCount) { didSet { needsDisplay = true } }
    override func draw(_ dirtyRect: NSRect) {
        let w = bounds.width / CGFloat(bandCount)
        for (i, v) in levels.enumerated() {
            let h = max(2, bounds.height * CGFloat(v) / 255)
            glowInk.withAlphaComponent(0.35 + 0.65 * CGFloat(v) / 255).setFill()
            NSBezierPath(roundedRect: NSRect(x: CGFloat(i) * w + 1.5, y: 0, width: w - 3, height: h), xRadius: 1.5, yRadius: 1.5).fill()
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow!
    private let screen = NSImageView()
    private let status = NSTextField(labelWithString: "Starting…")
    private let meter = Meter()
    private let allow = NSButton(title: "Allow Recording…", target: nil, action: nil)
    private let login = NSButton(checkboxWithTitle: "Open at login", target: nil, action: nil)
    private let lightsBox = NSButton(checkboxWithTitle: "Govee lights", target: nil, action: nil)
    private let govee = GoveeLights()
    private var lampColours: [[UInt8]] = [[255, 236, 210]]  // the cover's, for Cover Colours
    private var lampBeats = 0, lampHeld = false, lampFlash = 0.0, lampEnergy = 0.0
    private var lampQuietSince: Date?
    private let stylePicker = NSPopUpButton(frame: .zero, pullsDown: false)
    private let schemePicker = NSPopUpButton(frame: .zero, pullsDown: false)
    private var activity: NSObjectProtocol?
    private var link: Link!
    private var analyzer: Analyzer!
    private var capture: Capture!
    private let watcher = NowPlayingWatcher()
    private var problem: String?
    private var frame = [UInt8](repeating: 0, count: 256 * 240 * 3)
    private let started = Date()

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Keep streaming at full rate with the window closed: no App Nap.
        activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiatedAllowingIdleSystemSleep, .latencyCritical],
                                                         reason: "Streaming the music to the display")
        buildMenu()
        buildWindow()
        link = Link(host: UserDefaults.standard.string(forKey: "host"))
        analyzer = Analyzer { [weak self] in self?.link.send($0) }
        capture = Capture(analyzer: analyzer)
        applyStyle()
        watcher.onNewTrack = { [weak self] track, image in
            if self?.shuffling == true { self?.shuffle() }
            if let image { self?.lampColours = coverColours(image) }
            if let image {
                let cover = rgb(image, size: 320), thumb = rgb(image, size: 64)
                cover.withUnsafeBufferPointer { c in
                    thumb.withUnsafeBufferPointer { t in
                        lmd_demo_set_track(c.baseAddress, t.baseAddress, track.title, track.artist)
                    }
                }
            } else {
                lmd_demo_set_track(nil, nil, track.title, track.artist)
            }
        }
        if !CGPreflightScreenCaptureAccess() { CGRequestScreenCaptureAccess() }
        updateCapture()
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.updateCapture() }
        Timer.scheduledTimer(withTimeInterval: 1.0 / 30, repeats: true) { [weak self] _ in self?.tick() }
        Timer.scheduledTimer(withTimeInterval: 1.0 / 15, repeats: true) { [weak self] _ in self?.driveLamps() }
        govee.onFound = { [weak self] in self?.lampQuietSince = nil }
        govee.discover()
        var scans = 0
        Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in  // until a lamp answers, then each minute
            guard let self else { return }
            scans += 1
            if self.govee.lamps.isEmpty || scans % 6 == 0 { self.govee.discover() }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    func applicationWillTerminate(_ notification: Notification) { govee.stop() }  // the lamps as they were

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        window.makeKeyAndOrderFront(nil)
        return true
    }

    // Capture only while Spotify plays and a style is on, so macOS's recording indicator goes
    // away with the music. A short grace keeps it through track changes and quick pauses. If
    // the app may not ask Spotify what it's doing, it captures whenever Spotify is open.
    private var quietSince: Date?
    private var starting = false

    // The lamps follow the music: one of the colours a bar (the cover's, or the scheme's),
    // brightness with the loudness, a flash on each kick; dim and still when it's quiet.
    private func driveLamps() {
        guard lightsBox.state == .on, !govee.lamps.isEmpty else { return }
        let scheme = Int(analyzer.scheme)
        let colours = scheme == 0 || scheme > schemeLampColours.count ? lampColours : schemeLampColours[scheme - 1]
        let r = analyzer.reading
        guard r.playing, analyzer.enabled else {  // ten seconds of quiet: the lamps go back to how they were
            let since = lampQuietSince ?? Date()
            lampQuietSince = since
            if Date().timeIntervalSince(since) > 10 { govee.stop() }
            return
        }
        lampQuietSince = nil
        govee.start()
        let loud = Double(r.bands.reduce(0) { $0 + Int($1) }) / Double(255 * r.bands.count)
        lampEnergy += (loud - lampEnergy) * 0.2
        let beat = Double(r.beat) / 255
        if beat > 0.6 && !lampHeld {
            lampHeld = true
            lampBeats += 1
            lampFlash = 1
        }
        if beat < 0.35 { lampHeld = false }
        lampFlash *= 0.7
        govee.set(colours[(lampBeats / 4) % colours.count], brightness: Int(25 + 50 * min(1, lampEnergy * 1.6) + 25 * lampFlash))
    }

    @objc private func toggleLights() {
        UserDefaults.standard.set(lightsBox.state == .on, forKey: "lights")
        if lightsBox.state == .off { govee.stop() }
    }

    private func updateCapture() {
        watcher.poll()
        if shuffling && Date().timeIntervalSince(shuffledAt) > 60 { shuffle() }
        if analyzer.enabled && (watcher.playing || !watcher.reachable) {
            quietSince = nil
            startCapture()
        } else if capture.running {
            let since = quietSince ?? Date()
            quietSince = since
            if Date().timeIntervalSince(since) > 10 { stopCapture() }
        } else if !starting {
            problem = nil
        }
    }

    private func stopCapture() {
        quietSince = nil
        Task {
            await capture.stop()
            analyzer.quiet()
        }
    }

    private func startCapture() {
        guard !capture.running, !starting else { return }
        starting = true
        Task {
            defer { Task { @MainActor in self.starting = false } }
            do {
                try await capture.start()
                await MainActor.run { self.problem = nil }
            } catch {
                let text: String
                switch error {
                case CaptureError.noSpotify: text = "Open Spotify to start"
                case CaptureError.noDisplay: text = "Waiting for a screen to attach to"
                default: text = "Needs permission to record screen & system audio"
                }
                await MainActor.run {
                    self.problem = text
                    self.allow.isHidden = !text.hasPrefix("Needs")
                }
            }
        }
    }

    // The window's preview only; the display keeps getting the music either way.
    private var previewVisible: Bool { window.isVisible && window.occlusionState.contains(.visible) }

    private func tick() {
        guard previewVisible else { return }
        let r = analyzer.reading
        meter.levels = r.bands
        lmd_demo_set_playing(watcher.playing ? 1 : 0, watcher.paused ? 1 : 0)
        let now = Date(), cal = Calendar.current
        let c = cal.dateComponents([.hour, .minute, .second, .weekday, .month, .day], from: now)
        let ms = UInt32(truncatingIfNeeded: Int(now.timeIntervalSince(started) * 1000))
        r.bands.withUnsafeBufferPointer { b in
            r.wave.withUnsafeBufferPointer { w in
            frame.withUnsafeMutableBufferPointer { out in
                lmd_demo_render(b.baseAddress, UnsafeRawPointer(w.baseAddress!).assumingMemoryBound(to: Int8.self), r.beat, r.playing && analyzer.enabled ? 1 : 0, Int32(analyzer.style), Int32(analyzer.scheme), ms, Int32(c.hour ?? 0),
                                Int32(c.minute ?? 0), Int32((c.weekday ?? 1) - 1), Int32((c.month ?? 1) - 1), Int32(c.day ?? 1),
                                out.baseAddress)
            }
            }
        }
        let provider = CGDataProvider(data: Data(frame) as CFData)!
        if let image = CGImage(width: 256, height: 240, bitsPerComponent: 8, bitsPerPixel: 24, bytesPerRow: 256 * 3,
                               space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: 0), provider: provider,
                               decode: nil, shouldInterpolate: true, intent: .defaultIntent) {
            screen.image = NSImage(cgImage: image, size: NSSize(width: 512, height: 384))
        }

        var parts: [String] = []
        if let problem {
            parts.append(problem)
        } else if r.playing {
            parts.append("Listening to Spotify")
        } else if !analyzer.enabled {
            parts.append("Visualizer off")
        } else {
            parts.append(watcher.paused ? "Spotify is paused" : "Spotify is quiet")
        }
        parts.append(link.address.map { "display at \($0)" } ?? "looking for the display…")
        if lightsBox.state == .on {
            let lamps = govee.lamps
            parts.append(lamps.isEmpty ? "no Govee lamp heard" : lamps.values.sorted().joined(separator: ", "))
        }
        status.stringValue = parts.joined(separator: "  ·  ")
        login.state = SMAppService.mainApp.status == .enabled ? .on : .off
    }

    // "Off", then the styles (by name, so a saved choice survives the list changing).
    // Shuffle: a new style (never the same twice running) with each song and every minute.
    private var shuffling = false
    private var shuffledAt = Date.distantPast

    private func shuffle() {
        var next = Int.random(in: 0..<styles.count)
        if styles.count > 1 && next == Int(analyzer.style) { next = (next + 1 + Int.random(in: 0..<styles.count - 1)) % styles.count }
        analyzer.style = UInt8(next)
        shuffledAt = Date()
    }

    private func applyStyle() {
        let title = stylePicker.titleOfSelectedItem ?? styles[0]
        analyzer.enabled = title != "Off"
        shuffling = title == "Shuffle"
        if shuffling { shuffle() } else { analyzer.style = UInt8(styles.firstIndex(of: title) ?? 0) }
        analyzer.scheme = UInt8(schemes.firstIndex(of: schemePicker.titleOfSelectedItem ?? schemes[0]) ?? 0)
    }

    @objc private func pickScheme() {
        applyStyle()
        UserDefaults.standard.set(schemePicker.titleOfSelectedItem, forKey: "schemeName")
    }

    @objc private func pickStyle() {
        applyStyle()
        UserDefaults.standard.set(stylePicker.titleOfSelectedItem, forKey: "styleName")
    }

    @objc private func openPrivacy() {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")!)
    }

    @objc private func toggleLogin() {
        do {
            if SMAppService.mainApp.status == .enabled { try SMAppService.mainApp.unregister() } else { try SMAppService.mainApp.register() }
        } catch {
            problem = "Couldn't change Open at login: \(error.localizedDescription)"
        }
    }

    private func buildMenu() {
        let menu = NSMenu(), appItem = NSMenuItem()
        menu.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Quit LMD Visualizer", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        NSApp.mainMenu = menu
    }

    private func buildWindow() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 552, height: 520), styleMask: [.titled, .closable, .miniaturizable],
                          backing: .buffered, defer: false)
        window.title = "LMD Visualizer"
        window.isReleasedWhenClosed = false
        window.backgroundColor = paper
        window.titlebarAppearsTransparent = true
        window.appearance = NSAppearance(named: .darkAqua)
        let root = NSView(frame: window.contentRect(forFrameRect: window.frame))
        window.contentView = root

        let title = NSTextField(labelWithString: "LMD Visualizer")
        title.font = sora("SemiBold", 15)
        title.textColor = ink
        let subtitle = NSTextField(labelWithString: "What the display shows, live")
        subtitle.font = sora("Regular", 11)
        subtitle.textColor = dimInk
        screen.imageScaling = .scaleAxesIndependently
        screen.wantsLayer = true
        screen.layer?.backgroundColor = NSColor.black.cgColor
        screen.layer?.cornerRadius = 6
        screen.layer?.masksToBounds = true
        status.font = sora("Regular", 11)
        status.textColor = dimInk
        status.lineBreakMode = .byTruncatingTail
        allow.target = self
        allow.action = #selector(openPrivacy)
        allow.isHidden = true
        allow.font = sora("Regular", 11)
        login.target = self
        login.action = #selector(toggleLogin)
        login.font = sora("Regular", 11)
        lightsBox.target = self
        lightsBox.action = #selector(toggleLights)
        lightsBox.font = sora("Regular", 11)
        lightsBox.state = UserDefaults.standard.bool(forKey: "lights") ? .on : .off
        stylePicker.addItem(withTitle: "Off")
        stylePicker.addItem(withTitle: "Shuffle")
        stylePicker.menu?.addItem(.separator())
        stylePicker.addItems(withTitles: styles)
        let saved = UserDefaults.standard.string(forKey: "styleName") ?? styles[0]
        stylePicker.selectItem(withTitle: saved == "Off" || saved == "Shuffle" || styles.contains(saved) ? saved : styles[0])
        stylePicker.font = sora("Regular", 12)
        stylePicker.target = self
        stylePicker.action = #selector(pickStyle)
        schemePicker.addItems(withTitles: schemes)
        let savedScheme = UserDefaults.standard.string(forKey: "schemeName") ?? schemes[0]
        schemePicker.selectItem(withTitle: schemes.contains(savedScheme) ? savedScheme : schemes[0])
        schemePicker.font = sora("Regular", 12)
        schemePicker.target = self
        schemePicker.action = #selector(pickScheme)

        let place: [(NSView, NSRect)] = [
            (title, NSRect(x: 20, y: 482, width: 165, height: 20)),
            (subtitle, NSRect(x: 20, y: 464, width: 165, height: 16)),
            (schemePicker, NSRect(x: 190, y: 470, width: 168, height: 28)),
            (screen, NSRect(x: 20, y: 72, width: 512, height: 384)),
            (stylePicker, NSRect(x: 362, y: 470, width: 172, height: 28)),
            (meter, NSRect(x: 452, y: 42, width: 80, height: 18)),
            (status, NSRect(x: 20, y: 44, width: 420, height: 16)),
            (allow, NSRect(x: 14, y: 8, width: 150, height: 30)),
            (login, NSRect(x: 420, y: 12, width: 120, height: 22)),
            (lightsBox, NSRect(x: 296, y: 12, width: 116, height: 22)),
        ]
        for (view, rect) in place {
            view.frame = rect
            root.addSubview(view)
        }
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
