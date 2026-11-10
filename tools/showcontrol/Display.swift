// The LMD-9020 display (found by its broadcast, sent the music as packets), Spotify's song and
// cover, and keeping every output in time with what you actually hear.

import AppKit
import CoreAudio
import CoreMedia
import Foundation

// Learns the display's address from its broadcast ("LMD9020 1" every 2 s), then sends it
// packets: "LMDV", version 1, flags (bit 0: music playing), level, beat, 16 bands, style, colour
// scheme, then 64 samples of the waveform (signed bytes, about 5 ms, scaled to fill).
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

// MARK: - In time with what you hear

// The music is captured before it reaches the speakers; Bluetooth ones add 150-250 ms. Each
// output is held back by the speakers' delay (from Core Audio) plus the trim, less its own delay
// (the bulb's own ~150 ms means it is held back least), so the light lands on the sound.
enum Sync {
    static var trim = 0.0  // seconds, the user's adjustment
    private static var speakers = 0.0
    private static var checked = Date.distantPast
    private static let queue = DispatchQueue(label: "sync", qos: .userInteractive)

    static var speakerDelay: Double {
        if Date().timeIntervalSince(checked) > 2 {
            checked = Date()
            speakers = outputLatency()
        }
        return speakers
    }

    // How long the captured sound takes to reach the app (measured on every buffer).
    static var captureLag = 0.0
    // The clock capture timestamps are on, in seconds.
    static var now: Double { CMClockGetTime(CMClockGetHostTimeClock()).seconds }

    // Runs `work` once the sound has caught up, given how long this output takes itself.
    static func later(own: Double, _ work: @escaping () -> Void) {
        let wait = speakerDelay + trim - own - captureLag
        if wait <= 0.002 {
            work()
        } else {
            queue.asyncAfter(deadline: .now() + wait, execute: work)
        }
    }

    // When a sound Spotify played at `played` (capture clock) is heard.
    static func heard(_ played: Double) -> Double { played + speakerDelay + trim }

    private static func outputLatency() -> Double {
        var device = AudioObjectID(0)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultOutputDevice,
                                              mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &device) == noErr else { return 0 }
        func frames(_ selector: AudioObjectPropertySelector) -> UInt32 {
            var value: UInt32 = 0
            var s = UInt32(MemoryLayout<UInt32>.size)
            var a = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioDevicePropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
            return AudioObjectGetPropertyData(device, &a, 0, nil, &s, &value) == noErr ? value : 0
        }
        var rate = 48000.0
        var rs = UInt32(MemoryLayout<Double>.size)
        var ra = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyNominalSampleRate,
                                            mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        AudioObjectGetPropertyData(device, &ra, 0, nil, &rs, &rate)
        // The first output stream's own latency.
        var streamLatency: UInt32 = 0
        var sa = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreams, mScope: kAudioDevicePropertyScopeOutput,
                                            mElement: kAudioObjectPropertyElementMain)
        var ss: UInt32 = 0
        if AudioObjectGetPropertyDataSize(device, &sa, 0, nil, &ss) == noErr, ss >= UInt32(MemoryLayout<AudioStreamID>.size) {
            var streams = [AudioStreamID](repeating: 0, count: Int(ss) / MemoryLayout<AudioStreamID>.size)
            AudioObjectGetPropertyData(device, &sa, 0, nil, &ss, &streams)
            var la = AudioObjectPropertyAddress(mSelector: kAudioStreamPropertyLatency, mScope: kAudioObjectPropertyScopeGlobal,
                                                mElement: kAudioObjectPropertyElementMain)
            var ls = UInt32(MemoryLayout<UInt32>.size)
            AudioObjectGetPropertyData(streams[0], &la, 0, nil, &ls, &streamLatency)
        }
        let total = frames(kAudioDevicePropertyLatency) + frames(kAudioDevicePropertySafetyOffset) +
            frames(kAudioDevicePropertyBufferFrameSize) + streamLatency
        return rate > 0 ? Double(total) / rate : 0
    }
}

// MARK: - Spotify's song and cover

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

// The cover's strongest colours (up to three, 40 degrees apart), bright, for the lights.
func coverColours(_ image: CGImage) -> [RGB] {
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
    if picked.isEmpty { return [RGB(255, 236, 210)] }  // a grey cover: warm white
    return picked.map { bin in
        RGB(hue: Double(bin) * 10 + 5, saturation: max(0.6, min(1, sat[bin] / max(weight[bin], 1e-6))))
    }
}
