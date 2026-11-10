// TILES as part of the show: Show Control takes over its LEDs over MIDI (SysEx, manufacturer
// 7D, sub-ID 02; the format is in the TILES firmware, services/README.md, "Show link"). The
// underglow strobes on the kicks and the pads play an animation chosen here, from the music's
// loudness and six bands. TILES lets go by itself two seconds after the messages stop.

import CoreMIDI
import Foundation

final class TilesLink {
    enum Animation: Int, CaseIterable, Codable { case meter, pulse, ripple, sparkle, rain, chase, spectrum }
    enum Glow: Int, CaseIterable, Codable { case white, main, second, off }

    // Everything chosen on the TILES page. The sliders are 0-100; 50 is the firmware's own pace
    // for tail, sensitivity and speed.
    struct Mode: Equatable, Codable {
        var on = false
        var animation = Animation.meter
        var main = RGB(255, 0, 255)
        var second = RGB(148, 171, 249)
        var accent = RGB(255, 255, 255)
        var glow = Glow.white
        var strobe = 13       // the underglow's flash, 20-400 ms
        var brightness = 100  // the pads
        var tail = 50         // how long the pads' lights hang on
        var bed = 33          // the dim light under the hits
        var sensitivity = 50  // how high the meter's bars reach
        var speed = 50        // ripple, rain and chase
        var meterColours = false  // the meter in the colours, not white/yellow/red
    }

    private var client = MIDIClientRef()
    private var port = MIDIPortRef()
    private var destination: MIDIEndpointRef = 0
    private let lock = NSLock()
    private(set) var name: String?  // the TILES found, or nil
    private var mode = Mode()
    private var modeSentAt = Date.distantPast
    private var accent: UInt8 = 0

    init() {
        MIDIClientCreateWithBlock("Show Control" as CFString, &client) { [weak self] _ in self?.find() }
        MIDIOutputPortCreate(client, "TILES" as CFString, &port)
        find()
    }

    // The TILES main MIDI port ("SENTIA TILES n MIDI"), looked up again whenever MIDI changes.
    private func find() {
        var found: MIDIEndpointRef = 0, foundName: String?
        for i in 0..<MIDIGetNumberOfDestinations() {
            let d = MIDIGetDestination(i)
            var cf: Unmanaged<CFString>?
            MIDIObjectGetStringProperty(d, kMIDIPropertyDisplayName, &cf)
            let n = (cf?.takeRetainedValue() as String?) ?? ""
            var offline: Int32 = 0
            MIDIObjectGetIntegerProperty(d, kMIDIPropertyOffline, &offline)
            if offline == 0, n.uppercased().contains("SENTIA TILES"), !n.uppercased().contains("DAW") {
                found = d
                foundName = n
                break
            }
        }
        lock.lock()
        destination = found
        name = foundName
        modeSentAt = .distantPast
        lock.unlock()
    }

    func set(_ m: Mode) {
        lock.lock()
        let changed = m != mode
        mode = m
        if changed { modeSentAt = .distantPast }
        lock.unlock()
        if changed && !m.on { sendMode() }  // let go at once rather than in two seconds
    }

    private static func seven(_ v: Double) -> UInt8 { UInt8(max(0, min(127, Int((v * 127).rounded())))) }
    private static func seven(_ c: UInt8) -> UInt8 { c >> 1 }

    private static func seven(percent v: Int) -> UInt8 { UInt8(max(0, min(127, Int((Double(v) / 100 * 127).rounded())))) }

    private func sendMode() {
        lock.lock()
        let m = mode
        modeSentAt = Date()
        lock.unlock()
        send([0x01, m.on ? 1 : 0, UInt8(m.animation.rawValue),
              Self.seven(m.main.r), Self.seven(m.main.g), Self.seven(m.main.b),
              Self.seven(m.second.r), Self.seven(m.second.g), Self.seven(m.second.b)])
        guard m.on else { return }
        send([0x03, UInt8(m.glow.rawValue), Self.seven(percent: m.strobe), Self.seven(percent: m.brightness),
              Self.seven(percent: m.tail), Self.seven(percent: m.bed), Self.seven(percent: m.sensitivity),
              Self.seven(percent: m.speed), m.meterColours ? 1 : 0,
              Self.seven(m.accent.r), Self.seven(m.accent.g), Self.seven(m.accent.b)])
    }

    var on: Bool { lock.lock(); defer { lock.unlock() }; return mode.on && destination != 0 }

    // One frame: a kick's strength (0 none), the loudness 0-1, the 16 bands (0-255, low to high).
    func frame(kick: Float, level: Double, bands: [UInt8]) {
        guard on else { return }
        if Date().timeIntervalSince(modeSentAt) > 1 { sendMode() }  // and so it's known after a replug
        let groups = [0..<2, 2..<5, 5..<8, 8..<11, 11..<14, 14..<16]
        let six = groups.map { g -> UInt8 in
            guard g.upperBound <= bands.count else { return 0 }
            let peak = g.map { bands[$0] }.max() ?? 0
            return peak >> 1
        }
        lock.lock()
        let a = accent
        accent = 0
        lock.unlock()
        send([0x02, Self.seven(Double(kick)), a, Self.seven(level)] + six)
    }

    func noteAccent(_ strength: Float) {
        lock.lock()
        accent = max(accent, Self.seven(Double(strength)))
        lock.unlock()
    }

    private func send(_ body: [UInt8]) {
        lock.lock()
        let d = destination
        lock.unlock()
        guard d != 0 else { return }
        let bytes: [UInt8] = [0xF0, 0x7D, 0x02] + body + [0xF7]
        var list = MIDIPacketList()
        let packet = MIDIPacketListInit(&list)
        _ = MIDIPacketListAdd(&list, MemoryLayout<MIDIPacketList>.size, packet, 0, bytes.count, bytes)
        MIDISend(port, d, &list)
    }
}
