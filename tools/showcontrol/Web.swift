// The phone page: Show Control serves web/index.html (port 8770) with a small JSON API behind it,
// at home and, through a tunnel, at the public address. A phone signs in once with the password
// set on the Scenes page and keeps a long random key; every API call carries it.

import CommonCrypto
import CryptoKit
import Foundation
import Network

// The password (PBKDF2, salted) and the phones' keys (only their hashes) in the app's defaults.
// Wrong passwords lock sign-in for longer and longer.
final class WebAuth {
    static let shared = WebAuth()
    private let d = UserDefaults.standard
    private let lock = NSLock()
    private var fails = 0, lockedUntil = Date.distantPast

    var hasPassword: Bool { d.data(forKey: "webPass") != nil }
    var keys: [String] { d.stringArray(forKey: "webKeys") ?? [] }

    private func derive(_ password: String, _ salt: Data) -> Data {
        var out = Data(count: 32)
        let pw = Array(password.utf8)
        out.withUnsafeMutableBytes { o in
            salt.withUnsafeBytes { s in
                _ = CCKeyDerivationPBKDF(CCPBKDFAlgorithm(kCCPBKDF2), pw.map { CChar(bitPattern: $0) }, pw.count,
                                         s.bindMemory(to: UInt8.self).baseAddress, salt.count, CCPseudoRandomAlgorithm(kCCPRFHmacAlgSHA256),
                                         200_000, o.bindMemory(to: UInt8.self).baseAddress, 32)
            }
        }
        return out
    }
    private static func random(_ n: Int) -> Data { Data((0..<n).map { _ in UInt8.random(in: 0...255) }) }
    private static func hash(_ s: String) -> String { SHA256.hash(data: Data(s.utf8)).map { String(format: "%02x", $0) }.joined() }

    func setPassword(_ p: String) -> Bool {
        guard p.count >= 10 else { return false }
        let salt = Self.random(16)
        d.set(salt, forKey: "webSalt")
        d.set(derive(p, salt), forKey: "webPass")
        d.set([String](), forKey: "webKeys")  // a new password signs every phone out
        return true
    }

    // 200 and a new key, 401 wrong, 429 locked for now, 403 no password set yet.
    func signIn(_ p: String) -> (Int, String?) {
        lock.lock()
        defer { lock.unlock() }
        guard let want = d.data(forKey: "webPass"), let salt = d.data(forKey: "webSalt") else { return (403, nil) }
        guard Date() >= lockedUntil else { return (429, nil) }
        let got = derive(p, salt)
        guard got.count == want.count, zip(got, want).reduce(0, { $0 | ($1.0 ^ $1.1) }) == 0 else {
            fails += 1
            if fails >= 5 { lockedUntil = Date().addingTimeInterval(min(3600, 30 * pow(2, Double(fails - 5)))) }
            return (401, nil)
        }
        fails = 0
        let key = Self.random(32).map { String(format: "%02x", $0) }.joined()
        d.set(Array((keys + [Self.hash(key)]).suffix(20)), forKey: "webKeys")
        return (200, key)
    }

    func valid(_ key: String?) -> Bool {
        guard let key, key.count == 64 else { return false }
        return keys.contains(Self.hash(key))
    }

    func signOutAll() { d.set([String](), forKey: "webKeys") }
}

final class WebServer {
    static let port: UInt16 = 8770
    private let show: Show
    private let queue = DispatchQueue(label: "web")
    private var listener: NWListener?

    init(show: Show) {
        self.show = show
        show.webAddress = localAddress().map { "http://\($0):\(Self.port)" }
        guard let l = try? NWListener(using: .tcp, on: NWEndpoint.Port(rawValue: Self.port)!) else { return }
        l.newConnectionHandler = { [weak self] c in self?.serve(c) }
        l.start(queue: queue)
        listener = l
    }

    private func serve(_ c: NWConnection) {
        c.start(queue: queue)
        read(c, Data())
    }

    private func read(_ c: NWConnection, _ got: Data) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, done, error in
            guard let self else { return }
            var buf = got
            if let data { buf += data }
            if let end = buf.range(of: Data("\r\n\r\n".utf8)) {
                let head = String(decoding: buf[..<end.lowerBound], as: UTF8.self)
                var lines = head.components(separatedBy: "\r\n")
                let request = lines.removeFirst().split(separator: " ")
                var headers: [String: String] = [:]
                for l in lines {
                    guard let colon = l.firstIndex(of: ":") else { continue }
                    headers[l[..<colon].lowercased()] = l[l.index(after: colon)...].trimmingCharacters(in: .whitespaces)
                }
                let length = min(65536, Int(headers["content-length"] ?? "0") ?? 0)
                if buf.count - end.upperBound >= length || done {
                    let body = buf[end.upperBound...].prefix(length)
                    let method = request.first.map(String.init) ?? "GET", path = request.count > 1 ? String(request[1]) : "/"
                    self.respond(c, self.handle(method, path, headers, Data(body)))
                    return
                }
            }
            if done || error != nil || buf.count > 1 << 17 { c.cancel() } else { self.read(c, buf) }
        }
    }

    private func json(_ status: Int, _ object: Any) -> (Int, String, Data) {
        (status, "application/json", (try? JSONSerialization.data(withJSONObject: object)) ?? Data("{}".utf8))
    }

    private func handle(_ method: String, _ path: String, _ headers: [String: String], _ body: Data) -> (Int, String, Data) {
        let res = Bundle.main.resourceURL!
        let bare = String(path.split(separator: "?").first ?? "/")
        if bare == "/" || bare == "/index.html" {
            return file(res.appendingPathComponent("web/index.html"), "text/html; charset=utf-8")
        }
        if bare.hasPrefix("/fonts/") {
            let name = String(bare.dropFirst(7))
            guard !name.contains("/"), !name.contains("..") else { return (404, "text/plain", Data()) }
            return file(res.appendingPathComponent("Fonts/\(name)"), name.hasSuffix(".ttf") ? "font/ttf" : "font/otf")
        }
        if bare == "/icon.png" { return file(res.appendingPathComponent("web/icon.png"), "image/png") }
        if bare == "/api/login", method == "POST" {
            let p = (try? JSONSerialization.jsonObject(with: body) as? [String: Any])?["password"] as? String ?? ""
            let (status, key) = WebAuth.shared.signIn(p)
            if status == 200 { DispatchQueue.main.async { self.show.refreshWeb() } }
            return json(status, key.map { ["key": $0] } ?? ["error": status == 429 ? "wait" : status == 403 ? "nopass" : "wrong"])
        }
        guard bare.hasPrefix("/api/") else { return (404, "text/plain", Data("Not here".utf8)) }
        guard WebAuth.shared.valid(headers["x-show-key"]) else { return json(401, ["error": "key"]) }
        let state: [String: Any] = DispatchQueue.main.sync {
            if bare == "/api/do", method == "POST",
               let action = try? JSONSerialization.jsonObject(with: body) as? [String: Any] {
                show.perform(action)
            }
            show.phoneWatching = Date()
            return show.webState()
        }
        return json(200, state)
    }

    private func file(_ url: URL, _ type: String) -> (Int, String, Data) {
        guard let data = try? Data(contentsOf: url) else { return (404, "text/plain", Data()) }
        return (200, type, data)
    }

    private func respond(_ c: NWConnection, _ r: (Int, String, Data)) {
        let reason = [200: "OK", 401: "Unauthorized", 403: "Forbidden", 404: "Not Found", 429: "Too Many Requests"][r.0] ?? "OK"
        let head = "HTTP/1.1 \(r.0) \(reason)\r\nContent-Type: \(r.1)\r\nContent-Length: \(r.2.count)\r\nCache-Control: no-store\r\n" +
            "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n"
        c.send(content: Data(head.utf8) + r.2, completion: .contentProcessed { _ in c.cancel() })
    }
}

// This Mac's address on the home network (Wi-Fi or Ethernet).
func localAddress() -> String? {
    var list: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&list) == 0, let first = list else { return nil }
    defer { freeifaddrs(list) }
    var best: String?
    for p in sequence(first: first, next: { $0.pointee.ifa_next }) {
        let i = p.pointee
        guard let addr = i.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET) else { continue }
        let name = String(cString: i.ifa_name)
        guard name.hasPrefix("en") else { continue }
        var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
        getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
        let ip = String(cString: host)
        if name == "en0" { return ip }
        best = best ?? ip
    }
    return best
}

extension RGB {
    var hex: String { String(format: "#%02x%02x%02x", r, g, b) }
    init?(hex: String) {
        let h = hex.hasPrefix("#") ? String(hex.dropFirst()) : hex
        guard h.count == 6, let v = Int(h, radix: 16) else { return nil }
        self.init(v >> 16 & 255, v >> 8 & 255, v & 255)
    }
}

// What the phone page sees, and what it can change.
extension Show {
    func webState() -> [String: Any] {
        func lookJSON(_ l: LampLook) -> [String: Any] {
            ["name": l.name, "movement": l.movement.rawValue, "source": l.source.rawValue,
             "colours": l.colours.map(\.hex), "random": l.random, "spotsLow": l.spotsLow, "spotsHigh": l.spotsHigh,
             "sizeLow": l.sizeLow, "sizeHigh": l.sizeHigh, "speed": l.speed, "rhythmLow": l.rhythmLow,
             "rhythmHigh": l.rhythmHigh, "direction": l.direction.rawValue]
        }
        return [
            "title": title, "artist": artist, "status": status,
            "lampsOn": lampsOn, "style": styleName, "scheme": schemeName,
            "styles": ["Shuffle"] + styles + ["Off"], "schemes": schemes,
            "presets": looks.map { ["id": $0.id.uuidString, "name": $0.name] },
            "fixtures": fixtures.map { f in
                ["id": f.id, "name": f.name, "kind": f.kind.rawValue, "segments": f.segments, "on": f.settings.on,
                 "brightness": f.settings.brightness, "kick": f.settings.kick, "held": f.held,
                 "look": lookJSON(f.settings.look), "preset": preset(of: f.settings.look)?.uuidString ?? "",
                 "colours": (live[f.id] ?? []).map(\.hex)] as [String: Any]
            },
            "scenes": scenes.map { ["id": $0.id.uuidString, "name": $0.name] },
            "movements": LampLook.Movement.allCases.map(\.rawValue),
            "sources": LampLook.Source.allCases.map(\.rawValue),
            "directions": LampLook.Direction.allCases.map(\.rawValue),
        ]
    }

    func perform(_ a: [String: Any]) {
        let id = a["id"] as? String ?? ""
        func int(_ k: String, _ r: ClosedRange<Int>) -> Int? { (a[k] as? Int).map { max(r.lowerBound, min(r.upperBound, $0)) } }
        switch a["action"] as? String {
        case "lamps": if let on = a["on"] as? Bool { lampsOn = on }
        case "display":
            if let s = a["style"] as? String, s == "Shuffle" || s == "Off" || styles.contains(s) { styleName = s }
            if let s = a["scheme"] as? String, schemes.contains(s) { schemeName = s }
        case "fixture":
            update(id) { f in
                if let on = a["on"] as? Bool { f.settings.on = on }
                if let b = int("brightness", 0...100) { f.settings.brightness = b }
                if let k = int("kick", 0...150) { f.settings.kick = k }
                if let n = a["name"] as? String, !n.isEmpty { f.name = String(n.prefix(40)) }
            }
        case "hold": hold(id)
        case "take": takeBack(id)
        case "look":
            update(id) { f in
                var l = f.settings.look
                if let v = a["name"] as? String, !v.isEmpty { l.name = String(v.prefix(40)) }
                if let v = (a["movement"] as? String).flatMap(LampLook.Movement.init) { l.movement = v }
                if let v = (a["source"] as? String).flatMap(LampLook.Source.init) { l.source = v }
                if let v = (a["direction"] as? String).flatMap(LampLook.Direction.init) { l.direction = v }
                if let v = a["colours"] as? [String] {
                    let cs = Array(v.compactMap { RGB(hex: $0) }.prefix(6))
                    if !cs.isEmpty { l.colours = cs }
                }
                if let v = a["random"] as? Bool { l.random = v }
                if let v = int("spotsLow", 1...8) { l.spotsLow = v; l.spotsHigh = max(l.spotsHigh, v) }
                if let v = int("spotsHigh", 1...8) { l.spotsHigh = v; l.spotsLow = min(l.spotsLow, v) }
                if let v = int("sizeLow", 1...10) { l.sizeLow = v; l.sizeHigh = max(l.sizeHigh, v) }
                if let v = int("sizeHigh", 1...10) { l.sizeHigh = v; l.sizeLow = min(l.sizeLow, v) }
                if let v = int("speed", 1...5) { l.speed = v }
                if let v = int("rhythmLow", 0...100) { l.rhythmLow = v; l.rhythmHigh = max(l.rhythmHigh, v) }
                if let v = int("rhythmHigh", 0...100) { l.rhythmHigh = v; l.rhythmLow = min(l.rhythmLow, v) }
                f.settings.look = l
            }
        case "presetLoad": if let p = (a["preset"] as? String).flatMap(UUID.init) { loadPreset(p, into: id) }
        case "presetSave": savePreset(from: id)
        case "presetDelete": if let p = (a["preset"] as? String).flatMap(UUID.init) { deletePreset(p) }
        case "sameOnAll": sameOnAll(from: id)
        case "sceneSave": saveScene(String((a["name"] as? String ?? "").prefix(40)))
        case "sceneRecall": if let u = UUID(uuidString: id) { recall(u) }
        case "sceneDelete": if let u = UUID(uuidString: id) { deleteScene(u) }
        default: break
        }
    }
}
