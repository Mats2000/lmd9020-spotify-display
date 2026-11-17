// A Novation Launchkey MK4 as part of the show (its Programmer's Reference Guide, v2): in DAW mode
// on its DAW port the 16 pads take an RGB colour each and its screen shows the song. Taken while
// the show plays, handed back (standalone mode) ten quiet seconds after. Pads play patterns from
// the music in the lamps' colours, a new one every 16 kicks; with the desk on, the bottom row is
// the desk instead (Show.pad) and the patterns fold onto the top row.

import CoreMIDI
import Foundation

final class LaunchkeyLink {
    private var client = MIDIClientRef(), outPort = MIDIPortRef(), inPort = MIDIPortRef()
    private let lock = NSLock()
    private var dawIn: MIDIEndpointRef = 0, dawOut: MIDIEndpointRef = 0, connected: MIDIEndpointRef = 0
    private var sku: UInt8 = 0x14       // 13h on the Minis
    private(set) var name: String?
    private var taken = false, titleShown = ""
    var enabled = UserDefaults.standard.object(forKey: "launchkey") as? Bool ?? true {
        didSet { UserDefaults.standard.set(enabled, forKey: "launchkey") }
    }
    var onPad: ((Int, Bool) -> Void)?  // a pad down or up, 0-7 the top row, 8-15 the bottom (from MIDI's thread)
    private var hits: [Int] = []

    // The desk on the bottom row: each pad's state, lit full when on, dim when off.
    struct Desk {
        var on = true
        var flash = false, blackout = false, freeze = false, impacts = false, lights = true, screen = false
        var style = 0  // 0 off, then club, lounge, rave, dubstep
    }
    private var desk = Desk()
    func setDesk(_ d: Desk) { lock.lock(); desk = d; lock.unlock() }
    private static let styleColours = [RGB(90, 90, 90), RGB(255, 0, 255), RGB(255, 150, 40), RGB(0, 220, 255), RGB(130, 40, 255)]

    init() {
        MIDIClientCreateWithBlock("Show Control" as CFString, &client) { [weak self] _ in self?.find() }
        MIDIOutputPortCreate(client, "Launchkey" as CFString, &outPort)
        MIDIInputPortCreateWithProtocol(client, "Launchkey in" as CFString, ._1_0, &inPort) { [weak self] list, _ in
            self?.read(list)
        }
        find()
    }

    var on: Bool { lock.lock(); defer { lock.unlock() }; return enabled && dawIn != 0 }

    // Its DAW In and DAW Out ("Launchkey MK4 61 DAW In"), looked up again whenever MIDI changes.
    private func find() {
        func named(_ e: MIDIEndpointRef) -> String? {
            var cf: Unmanaged<CFString>?, offline: Int32 = 0
            MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &cf)
            MIDIObjectGetIntegerProperty(e, kMIDIPropertyOffline, &offline)
            let n = (cf?.takeRetainedValue() as String?) ?? ""
            return offline == 0 && n.contains("Launchkey MK4") && n.contains("DAW") ? n : nil
        }
        var dst: MIDIEndpointRef = 0, src: MIDIEndpointRef = 0, found: String?
        for i in 0..<MIDIGetNumberOfDestinations() { let d = MIDIGetDestination(i); if let n = named(d) { dst = d; found = n; break } }
        for i in 0..<MIDIGetNumberOfSources() { let s = MIDIGetSource(i); if named(s) != nil { src = s; break } }
        lock.lock()
        let wasIn = connected
        dawIn = dst
        dawOut = src
        name = found.map { $0.replacingOccurrences(of: " DAW In", with: "") }
        sku = found?.contains("Mini") == true ? 0x13 : 0x14
        taken = false  // a replug comes back in standalone: take it again
        if wasIn != src {
            if wasIn != 0 { MIDIPortDisconnectSource(inPort, wasIn) }
            if src != 0 { MIDIPortConnectSource(inPort, src, nil) }
            connected = src
        }
        lock.unlock()
    }

    // Pads in DAW mode: notes 60h-67h (top) and 70h-77h (bottom) on channel 1.
    private func read(_ list: UnsafePointer<MIDIEventList>) {
        for packet in list.unsafeSequence() {
            let word = packet.pointee.words.0, status = UInt8((word >> 16) & 0xFF), note = Int((word >> 8) & 0x7F), vel = word & 0x7F
            guard status == 0x90 || status == 0x80, (0x60...0x67).contains(note) || (0x70...0x77).contains(note) else { continue }
            let pad = note >= 0x70 ? 8 + note - 0x70 : note - 0x60, down = status == 0x90 && vel > 0
            if down {
                lock.lock()
                hits.append(pad)
                lock.unlock()
            }
            onPad?(pad, down)
        }
    }

    private func send(_ messages: [[UInt8]]) {
        lock.lock()
        let d = dawIn
        lock.unlock()
        guard d != 0, !messages.isEmpty else { return }
        let size = 64 + messages.reduce(0) { $0 + $1.count + 16 }
        let buffer = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 8)
        defer { buffer.deallocate() }
        let list = buffer.assumingMemoryBound(to: MIDIPacketList.self)
        var packet = MIDIPacketListInit(list)
        for m in messages { packet = MIDIPacketListAdd(list, size, packet, 0, m.count, m) }
        MIDISend(outPort, d, list)
    }
    private func sysex(_ body: [UInt8]) -> [UInt8] { [0xF0, 0x00, 0x20, 0x29, 0x02, sku] + body + [0xF7] }

    private func take() {
        guard !taken else { return }
        taken = true
        titleShown = ""
        send([[0x9F, 0x0C, 0x7F], [0xB6, 0x1D, 0x02]])  // DAW mode, the pads in their DAW layout
    }

    // Back to standalone: its own lights, its pads its own again.
    func release() {
        guard taken else { return }
        taken = false
        send([[0x9F, 0x0C, 0x00]])
    }

    // The screen's stationary display: two lines, a name and a value.
    func show(title: String, artist: String) {
        guard taken else { return }
        let clean = { (s: String) in Array(s.unicodeScalars.map { $0.value >= 32 && $0.value <= 126 ? UInt8($0.value) : 0x20 }.prefix(20)) }
        titleShown = title
        send([sysex([0x04, 0x20, 0x01]), sysex([0x06, 0x20, 0x00] + clean(title.isEmpty ? "Show Control" : title)),
              sysex([0x06, 0x20, 0x01] + clean(artist.isEmpty ? "Show Control" : artist)), sysex([0x04, 0x20, 0x7F])])
    }
    // A moment on its screen (the global temporary display): what a desk pad just did.
    func notice(_ name: String, _ value: String) {
        guard taken else { return }
        let clean = { (s: String) in Array(s.unicodeScalars.map { $0.value >= 32 && $0.value <= 126 ? UInt8($0.value) : 0x20 }.prefix(20)) }
        send([sysex([0x04, 0x21, 0x01]), sysex([0x06, 0x21, 0x00] + clean(name)), sysex([0x06, 0x21, 0x01] + clean(value)), sysex([0x04, 0x21, 0x7F])])
    }

    // MARK: Patterns (on the rig's queue)

    private var cells = [Double](repeating: 0, count: 48)  // 16 pads, r g b 0-1
    private var prog = 0, kicks = 0, step = 0, burst = 0.0, accentAt = 0.0, t = 0.0, colourAt = 0
    private var palette: [RGB] = [RGB(255, 0, 255)]
    private var deskGlow = [Double](repeating: 0, count: 8)
    private func colour(_ k: Int) -> RGB { palette[((k % palette.count) + palette.count) % palette.count] }
    private func put(_ pad: Int, _ c: RGB, _ a: Double) {
        guard (0..<16).contains(pad), a > 0 else { return }
        let i = pad * 3
        cells[i] = max(cells[i], Double(c.r) / 255 * a)
        cells[i + 1] = max(cells[i + 1], Double(c.g) / 255 * a)
        cells[i + 2] = max(cells[i + 2], Double(c.b) / 255 * a)
    }
    private static let ring = [0, 1, 2, 3, 4, 5, 6, 7, 15, 14, 13, 12, 11, 10, 9, 8]  // round the two rows

    func kick(_ s: Float) {
        guard taken else { return }
        let a = 0.55 + 0.45 * Double(s)
        step += 1
        switch prog {
        case 1: put(Self.ring[step % 16], colour(step / 16), 1)
        case 2: for p in 0..<16 { put(p, colour(step / 4), a) }
        case 4: for p in 0..<8 { put(step % 2 == 0 ? p : p + 8, colour(step), a) }
        default: burst = max(burst, a * 0.5)
        }
        kicks += 1
        if kicks >= 16 { kicks = 0; prog = (prog + 1 + Int.random(in: 0..<4)) % 5; colourAt += 1 }
    }
    func accent(_ s: Float) {
        guard taken, prog == 3 || prog == 0 else { return }
        put(Int.random(in: 0..<16), colour(Int.random(in: 0..<3)), Double(s))
    }
    func impact() { burst = 1.4 }

    // One frame: take it if needed, draw the pattern, send all 16 pads.
    func frame(dt: Double, bands: [UInt8], level: Double, colours: [RGB], flash: Bool, blackout: Bool, title: String, artist: String) {
        guard on else { release(); return }
        take()
        if title != titleShown { show(title: title, artist: artist) }
        palette = colours.isEmpty ? [RGB(255, 0, 255)] : colours
        t += dt
        let decay = exp(-dt * (prog == 2 ? 5 : prog == 1 ? 4 : 9))
        for i in cells.indices { cells[i] *= decay }
        burst *= exp(-dt * 6)
        let band = { (col: Int) -> Double in
            guard bands.count >= 16 else { return 0 }
            return Double(max(bands[col * 2], bands[col * 2 + 1])) / 255
        }
        switch prog {
        case 0:  // the spectrum: bass left, the bottom row from a little, the top from a lot
            for c in 0..<8 {
                let b = band(c)
                put(8 + c, colour(colourAt + c / 3), min(1, b * 1.8))
                put(c, colour(colourAt + c / 3 + 1), max(0, (b - 0.45) * 2.2))
            }
        case 3:  // a slow wave through the colours under the sparks
            for p in 0..<16 {
                let x = Double(p % 8) / 8 + (p >= 8 ? 0.06 : 0)
                put(p, colour(colourAt + Int((x + t * 0.15) * 3) % 3), 0.15 + 0.35 * level * (0.5 + 0.5 * sin((x + t * 0.4) * 6.28)))
            }
        default: break
        }
        lock.lock()
        let pressed = hits
        hits.removeAll()
        lock.unlock()
        for p in pressed where !(desk.on && p >= 8) { put(p, RGB(255, 255, 255), 1) }
        if burst > 0.02 { for p in 0..<16 { put(p, RGB.mix(colour(colourAt), RGB(255, 255, 255), 0.5), burst) } }

        lock.lock()
        let d = desk
        lock.unlock()
        var lit = cells
        if d.on {
            for c in 0..<8 { for k in 0..<3 { lit[c * 3 + k] = max(lit[c * 3 + k], lit[(8 + c) * 3 + k]) } }  // the patterns on the top row
            let white = RGB(255, 255, 255)
            let pads: [(RGB, Bool)] = [(white, d.flash), (RGB(255, 20, 20), d.blackout), (RGB(120, 200, 255), d.freeze),
                                       (RGB(255, 220, 0), d.impacts), (RGB(255, 0, 255), false),
                                       (Self.styleColours[max(0, min(4, d.style))], d.style != 0), (RGB(0, 200, 160), d.screen),
                                       (RGB(40, 255, 60), d.lights)]
            for (k, (c, on)) in pads.enumerated() {
                let i = (8 + k) * 3, press = pressed.contains(8 + k) ? 1.0 : 0
                deskGlow[k] = max(deskGlow[k] * exp(-dt * 5), press)
                let base = on ? 1 : 0.13
                lit[i] = min(1, Double(c.r) / 255 * base + deskGlow[k] * 0.8)
                lit[i + 1] = min(1, Double(c.g) / 255 * base + deskGlow[k] * 0.8)
                lit[i + 2] = min(1, Double(c.b) / 255 * base + deskGlow[k] * 0.8)
            }
        }
        var out: [[UInt8]] = []
        for p in 0..<16 {
            var r = lit[p * 3], g = lit[p * 3 + 1], b = lit[p * 3 + 2]
            if flash && !(d.on && p >= 8) { r = 1; g = 1; b = 1 } else if blackout && !(d.on && p >= 8) { r = 0; g = 0; b = 0 }
            // LEDs are linear: the weaker channels taken down (gamma 2.2 against the strongest) so a hue
            // reads as it does on the lamps and the screen, its brightness kept.
            let top = max(r, g, b)
            if top > 0 { r = top * pow(r / top, 2.2); g = top * pow(g / top, 2.2); b = top * pow(b / top, 2.2) }
            let id = UInt8(p < 8 ? 0x60 + p : 0x70 + p - 8)
            out.append(sysex([0x01, 0x43, id, Self.seven(r), Self.seven(g), Self.seven(b)]))
        }
        send(out)
    }
    private static func seven(_ v: Double) -> UInt8 { UInt8(max(0, min(127, Int((v * 127).rounded())))) }
}
