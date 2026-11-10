// Lamps: looks (presets you design, like a venue's), the effect engine that turns a look and the
// music into colours for every segment, and the fixtures: Govee lamps streamed segment by segment
// over the LAN, Matter lights through the helper in matter/.

import Foundation

struct RGB: Codable, Equatable, Hashable {
    var r: UInt8, g: UInt8, b: UInt8

    init(_ r: Int, _ g: Int, _ b: Int) {
        self.r = UInt8(max(0, min(255, r)))
        self.g = UInt8(max(0, min(255, g)))
        self.b = UInt8(max(0, min(255, b)))
    }

    init(hue h: Double, saturation s: Double, value v: Double = 1) {
        let c = v * s, x = c * (1 - abs((h / 60).truncatingRemainder(dividingBy: 2) - 1)), m = v - c
        let (r, g, b): (Double, Double, Double) = h < 60 ? (c, x, 0) : h < 120 ? (x, c, 0) : h < 180 ? (0, c, x) : h < 240 ? (0, x, c) : h < 300 ? (x, 0, c) : (c, 0, x)
        self.init(Int((r + m) * 255), Int((g + m) * 255), Int((b + m) * 255))
    }

    func scaled(_ k: Double) -> RGB { RGB(Int(Double(r) * k), Int(Double(g) * k), Int(Double(b) * k)) }
    var level: Int { max(Int(r), Int(g), Int(b)) }

    static func mix(_ a: RGB, _ b: RGB, _ t: Double) -> RGB {
        RGB(Int(Double(a.r) + (Double(b.r) - Double(a.r)) * t), Int(Double(a.g) + (Double(b.g) - Double(a.g)) * t),
            Int(Double(a.b) + (Double(b.b) - Double(a.b)) * t))
    }
    static let black = RGB(0, 0, 0)
}

// The colours each scheme gives the lights (ColourScheme order, after Cover Colours).
let schemeLampColours: [[RGB]] = [
    [RGB(255, 0, 255), RGB(148, 171, 249), RGB(106, 34, 192)],    // Sentia: pink, light blue, purple
    [RGB(120, 188, 210), RGB(197, 235, 245), RGB(40, 140, 200)],  // Ice
    [RGB(250, 160, 50), RGB(170, 70, 10), RGB(255, 210, 140)],    // Amber
    [RGB(150, 130, 210), RGB(150, 220, 200), RGB(250, 200, 225)], // Soft Club
]

// A look: how the lamps move with the music. Saved as presets.
struct LampLook: Codable, Equatable, Identifiable {
    enum Movement: String, Codable, CaseIterable { case flow, twinkle, flexing, stacking, strike, gradient, chase, pulse }
    enum Source: String, Codable, CaseIterable { case own, cover, scheme }  // own: the colours below
    enum Direction: String, Codable, CaseIterable { case up, down, inward, outward, random }

    var id = UUID()
    var name: String
    var movement: Movement = .twinkle
    var source: Source = .own
    var colours: [RGB] = [RGB(255, 0, 128), RGB(112, 48, 192), RGB(170, 187, 255)]
    var random = false          // the order the colours come in
    var spotsLow = 2, spotsHigh = 5      // 1...8
    var sizeLow = 3, sizeHigh = 7        // 1...10
    var speed = 2                        // 1...5
    var rhythmLow = 18, rhythmHigh = 100 // % brightness, quiet to on the beat
    var direction: Direction = .up

    static let builtIn: [LampLook] = [
        LampLook(name: "Your Govee Twinkle"),
        LampLook(name: "Sentia Pulse", movement: .pulse, source: .own,
                 colours: [RGB(255, 0, 255), RGB(148, 171, 249), RGB(106, 34, 192)], rhythmLow: 10),
        LampLook(name: "Warehouse Strike", movement: .strike, colours: [RGB(255, 255, 255), RGB(148, 171, 249)], speed: 5, rhythmLow: 4),
        LampLook(name: "Slow Bloom", movement: .gradient, colours: [RGB(150, 130, 210), RGB(150, 220, 200), RGB(250, 200, 225)], speed: 1, rhythmLow: 40, rhythmHigh: 90),
        LampLook(name: "Cover Colours Chase", movement: .chase, source: .cover, spotsLow: 2, spotsHigh: 4, speed: 3),
        LampLook(name: "Stacking Up", movement: .stacking, source: .scheme, rhythmLow: 30),
        LampLook(name: "Flowing", movement: .flow, source: .cover, speed: 2, rhythmLow: 35, rhythmHigh: 95),
        LampLook(name: "Breathing", movement: .flexing, source: .scheme, rhythmLow: 25),
    ]
}

// Presets live in ~/Library/Application Support/Show Control/looks.json.
enum LookStore {
    private static var file: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("Show Control")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("looks.json")
    }
    static func load() -> [LampLook] {
        guard let data = try? Data(contentsOf: file), let looks = try? JSONDecoder().decode([LampLook].self, from: data), !looks.isEmpty else {
            return LampLook.builtIn
        }
        return looks
    }
    static func save(_ looks: [LampLook]) {
        if let data = try? JSONEncoder().encode(looks) { try? data.write(to: file) }
    }
}

// Turns a look and the music into a colour per segment, frame by frame.
final class LampEngine {
    private struct Spot {
        var at: Double, size: Double, colour: RGB, age = 0.0, life: Double, drift: Double
    }

    let count: Int
    var look: LampLook
    var palette: [RGB] = [RGB(255, 236, 210)]
    private var spots: [Spot] = []
    private var strikes: [(at: Double, colour: RGB)] = []
    private(set) var beats = 0
    private var flash = 0.0, phase = 0.0, stacked = 0, nextColour = 0, sinceSpawn = 0.0

    init(count: Int, look: LampLook) {
        self.count = count
        self.look = look
    }

    private var colours: [RGB] { palette.isEmpty ? [RGB(255, 236, 210)] : palette }

    private func takeColour() -> RGB {
        let c = colours
        if look.random { return c.randomElement()! }
        defer { nextColour += 1 }
        return c[nextColour % c.count]
    }

    private var barColour: RGB { colours[(beats / 4) % colours.count] }
    private var speed: Double { [0.12, 0.3, 0.6, 1.1, 2.0][max(0, min(4, look.speed - 1))] }  // lamp lengths a second

    // The length the movement is drawn on: the whole lamp, or half of it mirrored (in, out).
    private var span: Int { look.direction == .inward || look.direction == .outward ? (count + 1) / 2 : count }

    // amount: the light's own kick setting (1 is a full flash, 0 none, the movement still steps).
    func kick(_ strength: Float, amount: Double = 1) {
        flash = max(flash, (0.55 + 0.45 * Double(strength)) * amount)
        beats += 1
        switch look.movement {
        case .twinkle:  // a new spot or two lands on the kick
            for _ in 0..<(strength > 0.6 ? 2 : 1) { spawn() }
        case .stacking:
            stacked = stacked >= span ? 0 : stacked + 1
        case .strike:
            strikes.append((at: 0, colour: takeColour()))
        default:
            break
        }
    }

    // A snare or hat: detail on top of the kick's pulse.
    func accent(_ strength: Float) {
        switch look.movement {
        case .twinkle: if spots.count < look.spotsHigh + 2 { spawn(short: true) }
        case .chase, .flow, .gradient: flash = max(flash, 0.2 * Double(strength))
        default: break
        }
    }

    private func spawn(short: Bool = false) {
        let size = Double(Int.random(in: look.sizeLow...max(look.sizeLow, look.sizeHigh))) / 10 * Double(span) * 0.5
        let drift: Double
        switch look.direction {
        case .random: drift = Bool.random() ? 1 : -1
        default: drift = 1
        }
        spots.append(Spot(at: Double.random(in: 0..<Double(span)), size: max(0.8, size), colour: takeColour(),
                          life: (short ? 0.25 : 0.5 + Double.random(in: 0...0.8)) / max(0.4, speed), drift: drift))
    }

    // One frame: dt seconds since the last, energy 0...1 (the music's loudness).
    func frame(dt: Double, energy: Double) -> [RGB] {
        flash *= exp(-dt * 11)  // a sharp fall: the kick reads as a hit, not a swell
        phase += dt * speed
        let n = span
        var line = [RGB](repeating: .black, count: n)
        let c = colours

        switch look.movement {
        case .twinkle:
            let want = Double(look.spotsLow) + Double(max(0, look.spotsHigh - look.spotsLow)) * min(1, energy * 1.4)
            sinceSpawn += dt
            if Double(spots.count) < want && sinceSpawn > 0.25 / max(0.4, speed) {
                spawn()
                sinceSpawn = 0
            }
            for i in spots.indices {
                spots[i].age += dt
                spots[i].at += spots[i].drift * dt * speed * Double(n) * 0.25
            }
            spots.removeAll { $0.age > $0.life }
            for s in spots {
                let t = s.age / s.life
                let env = t < 0.12 ? t / 0.12 : (t > 0.6 ? max(0, (1 - t) / 0.4) : 1)
                for i in 0..<n {
                    let d = abs(Double(i) - s.at)
                    let w = d < s.size / 2 ? 1 : max(0, 1 - (d - s.size / 2))  // flat top, soft edge
                    if w > 0 { line[i] = brighter(line[i], s.colour.scaled(w * env)) }
                }
            }
        case .flow:
            for i in 0..<n {
                let u = (Double(i) / Double(n) - phase).truncatingRemainder(dividingBy: 1) + 1
                let at = u.truncatingRemainder(dividingBy: 1) * Double(c.count)
                let k = Int(at) % c.count
                line[i] = RGB.mix(c[k], c[(k + 1) % c.count], at - Double(Int(at)))
            }
        case .flexing:
            let half = (0.15 + 0.55 * min(1, energy * 1.5) + 0.3 * flash) * Double(n)
            for i in 0..<n where Double(i) < half { line[i] = barColour.scaled(i == Int(half) ? half - Double(Int(half)) : 1) }
        case .stacking:
            for i in 0..<min(stacked, n) { line[i] = c[i % c.count] }
        case .strike:
            for i in 0..<n { line[i] = c[0].scaled(0.08) }
            for k in strikes.indices { strikes[k].at += dt * Double(n) * (1.5 + speed * 2) }
            strikes.removeAll { $0.at > Double(n) + 4 }
            for s in strikes {
                for i in 0..<n {
                    let d = s.at - Double(i)
                    if d >= 0 && d < 4 { line[i] = brighter(line[i], s.colour.scaled(1 - d / 4)) }
                }
            }
        case .gradient:
            for i in 0..<n {
                let at = (Double(i) / Double(max(1, n - 1)) * Double(c.count - 1) + sin(phase * 2) * 0.3)
                let k = max(0, min(c.count - 1, Int(at)))
                line[i] = RGB.mix(c[k], c[min(c.count - 1, k + 1)], max(0, min(1, at - Double(k))))
            }
        case .chase:
            let dots = max(1, look.spotsHigh)
            for d in 0..<dots {
                let at = (phase + Double(d) / Double(dots)).truncatingRemainder(dividingBy: 1) * Double(n)
                for i in 0..<n {
                    let dist = abs(Double(i) - at)
                    if dist < 1.2 { line[i] = brighter(line[i], c[d % c.count].scaled(1 - dist / 1.2)) }
                }
            }
        case .pulse:
            for i in 0..<n { line[i] = barColour }
        }

        // Brightness with the rhythm: from rhythmLow, quiet, to rhythmHigh on the beat.
        let drive = min(1, 0.45 * min(1, energy * 1.5) + 0.75 * flash)
        let level = (Double(look.rhythmLow) + Double(look.rhythmHigh - look.rhythmLow) * drive) / 100
        return place(line).map { $0.scaled(level) }
    }

    private func brighter(_ a: RGB, _ b: RGB) -> RGB { b.level > a.level ? b : a }

    // The drawn line onto the lamp's segments, in the look's direction (0 is the bottom).
    private func place(_ line: [RGB]) -> [RGB] {
        let n = line.count
        return (0..<count).map { i in
            switch look.direction {
            case .up, .random: return line[i]
            case .down: return line[count - 1 - i]
            case .outward, .inward:
                let d = Int(abs(Double(i) - Double(count - 1) / 2))  // 0 in the middle
                return look.direction == .outward ? line[min(n - 1, d)] : line[max(0, n - 1 - d)]
            }
        }
    }
}

// MARK: - Govee

// Govee's LAN API (turn on "LAN Control" for the lamp in the Govee Home app): lamps answer a scan
// sent to 239.255.255.250:4001 on that group's port 4002 (so the listener joins the group), and take
// commands on their port 4003. For the show they're streamed a colour per segment ("razer" frames,
// 25-30 a second); their own state is read first and put back after.
final class GoveeLamps {
    struct Lamp { var model: String; var segments: Int }
    private let sock: Int32
    private let lock = NSLock()
    private var found: [String: Lamp] = [:]
    private var saved: [String: [String: Any]] = [:]
    private var live = Set<String>()
    var onFound: (() -> Void)?
    var onStatus: ((String, [String: Any]) -> Void)?  // a lamp in the show, as it says it is

    // Segments a model has (the H6076 floor lamp: 14); others fall back to 10, which every
    // streaming lamp takes.
    private static let segmentsByModel: [String: Int] = ["H6076": 14]

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

    var lamps: [String: Lamp] {
        lock.lock()
        defer { lock.unlock() }
        return found
    }

    func discover() {
        send(#"{"msg":{"cmd":"scan","data":{"account_topic":"reserve"}}}"#, to: "239.255.255.250", port: 4001)
    }

    func isLive(_ ip: String) -> Bool { lock.lock(); defer { lock.unlock() }; return live.contains(ip) }

    // Takes a lamp for the show: its own state read first (to put back), then on, full, streaming.
    func start(_ ip: String) {
        lock.lock()
        let fresh = live.insert(ip).inserted
        lock.unlock()
        guard fresh else { return }
        send(#"{"msg":{"cmd":"devStatus","data":{}}}"#, to: ip, port: 4003)
        send(#"{"msg":{"cmd":"turn","data":{"value":1}}}"#, to: ip, port: 4003)
        send(#"{"msg":{"cmd":"brightness","data":{"value":100}}}"#, to: ip, port: 4003)
        razer(on: true, ip)
    }

    // Gives a lamp back as it was before the show.
    func stop(_ ip: String) {
        lock.lock()
        let was = live.remove(ip) != nil
        let st = saved.removeValue(forKey: ip)
        lock.unlock()
        guard was else { return }
        razer(on: false, ip)
        guard let st else { return }
        if let c = st["color"] as? [String: Any], let r = c["r"] as? Int, let g = c["g"] as? Int, let b = c["b"] as? Int {
            let k = st["colorTemInKelvin"] as? Int ?? 0
            send(#"{"msg":{"cmd":"colorwc","data":{"color":{"r":\#(r),"g":\#(g),"b":\#(b)},"colorTemInKelvin":\#(k)}}}"#, to: ip, port: 4003)
        }
        if let level = st["brightness"] as? Int { send(#"{"msg":{"cmd":"brightness","data":{"value":\#(level)}}}"#, to: ip, port: 4003) }
        if st["onOff"] as? Int == 0 { send(#"{"msg":{"cmd":"turn","data":{"value":0}}}"#, to: ip, port: 4003) }
    }

    // Lets go of a lamp someone else (Alexa) took: streaming off, nothing put back.
    func release(_ ip: String) {
        lock.lock()
        let was = live.remove(ip) != nil
        saved.removeValue(forKey: ip)
        lock.unlock()
        if was { razer(on: false, ip) }
    }

    func stopAll() { for ip in lamps.keys { stop(ip) } }

    // Asks the lamps in the show how they are; replies go to onStatus.
    func checkStatus() {
        lock.lock()
        let ips = live
        lock.unlock()
        for ip in ips { send(#"{"msg":{"cmd":"devStatus","data":{}}}"#, to: ip, port: 4003) }
    }

    // One frame for one lamp: a colour per segment (spread or cut to its own segment count).
    func show(_ colours: [RGB], to ip: String) {
        guard !colours.isEmpty, isLive(ip), let lamp = lamps[ip] else { return }
        let n = lamp.segments
        var frame: [UInt8] = [0xBB, 0x00, UInt8(2 + 3 * n), 0xB0, 0x01, UInt8(n)]
        for i in 0..<n {
            let c = colours[i * colours.count / n]
            frame += [c.r, c.g, c.b]
        }
        frame.append(frame.reduce(0, ^))
        send(#"{"msg":{"cmd":"razer","data":{"pt":"\#(Data(frame).base64EncodedString())"}}}"#, to: ip, port: 4003)
    }

    private func razer(on: Bool, _ ip: String) {
        var frame: [UInt8] = [0xBB, 0x00, 0x01, 0xB1, on ? 1 : 0]
        frame.append(frame.reduce(0, ^))
        send(#"{"msg":{"cmd":"razer","data":{"pt":"\#(Data(frame).base64EncodedString())"}}}"#, to: ip, port: 4003)
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
            let sender = String(cString: text)
            if msg["cmd"] as? String == "devStatus" {  // a lamp's own state, to put back later
                lock.lock()
                let first = live.contains(sender) && saved[sender] == nil
                if first { saved[sender] = data }
                lock.unlock()
                if !first { onStatus?(sender, data) }
                continue
            }
            guard msg["cmd"] as? String == "scan" else { continue }
            let ip = (data["ip"] as? String) ?? sender
            let model = (data["sku"] as? String) ?? "Govee"
            lock.lock()
            let isNew = found[ip] == nil
            found[ip] = Lamp(model: model, segments: Self.segmentsByModel[model] ?? 10)
            lock.unlock()
            if isNew { DispatchQueue.main.async { self.onFound?() } }
        }
    }
}

// MARK: - Matter

// Matter lights through matter/lights.js (Node and matter.js): it is paired once as an extra
// controller (`node lights.js pair <code>`), then run here and sent {"r","g","b","level","fade"} on
// UDP 127.0.0.1:4005. A light takes 40-400 ms a command, so it follows beats and kicks, not frames;
// the helper reports each light's round trip ("Lag: <id> <ms>") so its kicks can be sent that much early.
final class MatterLights {
    private var process: Process?
    private let sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
    private(set) var names: [String] = []
    private(set) var running = false
    var onReleased: ((String) -> Void)?  // someone else (Alexa) took this light
    private let lock = NSLock()
    private var trips: [String: Double] = [:]

    // How long a light takes to show a command (half the round trip, and a little for the bulb).
    func own(_ id: String) -> Double {
        lock.lock()
        defer { lock.unlock() }
        return min(0.4, (trips[id] ?? 0.2) / 2 + 0.02)
    }

    static var helper: URL? {
        // The project's matter/ folder (its node_modules are too big to copy into the app).
        if let path = Bundle.main.object(forInfoDictionaryKey: "MatterHelper") as? String,
           FileManager.default.fileExists(atPath: path + "/lights.js") { return URL(fileURLWithPath: path) }
        return nil
    }
    static var node: String? {
        ["/opt/homebrew/bin/node", "/usr/local/bin/node"].first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    func start() {
        guard !running, let dir = Self.helper, let node = Self.node else { return }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: node)
        p.arguments = ["lights.js", "serve"]
        p.currentDirectoryURL = dir
        let out = Pipe()
        p.standardOutput = out
        p.standardError = out
        // Its output also goes to ~/Library/Logs/Show Control/matter.log, for when a light won't come.
        let logs = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0].appendingPathComponent("Logs/Show Control")
        try? FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
        let logFile = logs.appendingPathComponent("matter.log")
        FileManager.default.createFile(atPath: logFile.path, contents: nil)
        let log = try? FileHandle(forWritingTo: logFile)
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let data = h.availableData
            log?.write(data)
            let text = String(decoding: data, as: UTF8.self)
            for line in text.split(separator: "\n") {
                if line.hasPrefix("Ready: ") {
                    let name = String(line.dropFirst(7))
                    DispatchQueue.main.async { if !(self?.names.contains(name) ?? true) { self?.names.append(name) } }
                } else if line.hasPrefix("Lag: ") {
                    let parts = line.split(separator: " ")
                    if parts.count == 3, let ms = Double(parts[2]) {
                        self?.lock.lock()
                        self?.trips[String(parts[1])] = ms / 1000
                        self?.lock.unlock()
                    }
                } else if line.hasPrefix("Released: ") {
                    let name = String(line.dropFirst(10).split(separator: " ").first ?? "")
                    DispatchQueue.main.async { self?.onReleased?(name) }
                }
            }
        }
        p.terminationHandler = { [weak self] _ in DispatchQueue.main.async { self?.running = false } }
        do {
            try p.run()
            process = p
            running = true
        } catch {}
    }

    func stop() {
        process?.terminate()
        process = nil
        running = false
    }

    func send(_ command: [String: Any]) {
        guard running, let data = try? JSONSerialization.data(withJSONObject: command) else { return }
        var a = sockaddr_in()
        a.sin_family = sa_family_t(AF_INET)
        a.sin_port = UInt16(4005).bigEndian
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr)
        let bytes = [UInt8](data)
        _ = withUnsafePointer(to: &a) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                sendto(sock, bytes, bytes.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    func colour(_ id: String, _ c: RGB, fade: Double) { send(["id": id, "r": Int(c.r), "g": Int(c.g), "b": Int(c.b), "fade": fade]) }
    func level(_ id: String, _ level: Double, fade: Double) { send(["id": id, "level": level, "fade": fade]) }
    // Straight to `level`, then down to `then` over `fall` seconds as soon as the light has it.
    func kick(_ id: String, level: Double, then: Double, fall: Double) {
        send(["id": id, "level": level, "then": then, "fall": fall, "kick": true])
    }
    func save(_ id: String) { send(["id": id, "save": true]) }
    func restore(_ id: String) { send(["id": id, "restore": true]) }
    func release(_ id: String) { send(["id": id, "release": true]) }
    func take(_ id: String) { send(["id": id, "take": true]) }
}
