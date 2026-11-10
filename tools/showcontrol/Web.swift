// The phone page: Show Control serves web/index.html on the home network (port 8770) with a small
// JSON API behind it, so a phone can run the show. Every request carries the code shown on the
// app's Venue page.

import Foundation
import Network

final class WebServer {
    static let port: UInt16 = 8770
    private let show: Show
    private let queue = DispatchQueue(label: "web")
    private var listener: NWListener?
    private let code: String

    init(show: Show) {
        self.show = show
        let d = UserDefaults.standard
        if let c = d.string(forKey: "webCode"), c.count == 6 { code = c } else {
            code = String(format: "%06d", Int.random(in: 0..<1_000_000))
            d.set(code, forKey: "webCode")
        }
        show.webCode = code
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
                let length = Int(headers["content-length"] ?? "0") ?? 0
                if buf.count - end.upperBound >= length || done {
                    let body = buf[end.upperBound...].prefix(length)
                    let method = request.first.map(String.init) ?? "GET", path = request.count > 1 ? String(request[1]) : "/"
                    self.respond(c, self.handle(method, path, headers, Data(body)))
                    return
                }
            }
            if done || error != nil || buf.count > 1 << 20 { c.cancel() } else { self.read(c, buf) }
        }
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
        guard bare.hasPrefix("/api/") else { return (404, "text/plain", Data("Not here".utf8)) }
        guard headers["x-show-code"] == code else { return (401, "application/json", Data(#"{"error":"code"}"#.utf8)) }
        let state: [String: Any] = DispatchQueue.main.sync {
            if bare == "/api/do", method == "POST",
               let action = try? JSONSerialization.jsonObject(with: body) as? [String: Any] {
                show.perform(action)
            }
            show.phoneWatching = Date()
            return show.webState()
        }
        let data = (try? JSONSerialization.data(withJSONObject: state)) ?? Data("{}".utf8)
        return (200, "application/json", data)
    }

    private func file(_ url: URL, _ type: String) -> (Int, String, Data) {
        guard let data = try? Data(contentsOf: url) else { return (404, "text/plain", Data()) }
        return (200, type, data)
    }

    private func respond(_ c: NWConnection, _ r: (Int, String, Data)) {
        let reason = [200: "OK", 401: "Unauthorized", 404: "Not Found"][r.0] ?? "OK"
        let head = "HTTP/1.1 \(r.0) \(reason)\r\nContent-Type: \(r.1)\r\nContent-Length: \(r.2.count)\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"
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
            ["id": l.id.uuidString, "name": l.name, "movement": l.movement.rawValue, "source": l.source.rawValue,
             "colours": l.colours.map(\.hex), "random": l.random, "spotsLow": l.spotsLow, "spotsHigh": l.spotsHigh,
             "sizeLow": l.sizeLow, "sizeHigh": l.sizeHigh, "speed": l.speed, "rhythmLow": l.rhythmLow,
             "rhythmHigh": l.rhythmHigh, "direction": l.direction.rawValue]
        }
        return [
            "title": title, "artist": artist, "status": status,
            "lampsOn": lampsOn, "style": styleName, "scheme": schemeName,
            "styles": ["Shuffle"] + styles + ["Off"], "schemes": schemes,
            "lookID": lookID.uuidString, "looks": looks.map(lookJSON),
            "preview": previewSegments.map(\.hex),
            "fixtures": fixtures.map { f in
                ["id": f.id, "name": f.name, "kind": f.kind.rawValue, "segments": f.segments, "on": f.settings.on,
                 "lookID": f.settings.lookID?.uuidString ?? "", "brightness": f.settings.brightness, "held": f.held,
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
        let uuid = UUID(uuidString: id)
        switch a["action"] as? String {
        case "lamps": if let on = a["on"] as? Bool { lampsOn = on }
        case "display":
            if let s = a["style"] as? String, s == "Shuffle" || s == "Off" || styles.contains(s) { styleName = s }
            if let s = a["scheme"] as? String, schemes.contains(s) { schemeName = s }
        case "fixture":
            update(id) { f in
                if let on = a["on"] as? Bool { f.settings.on = on }
                if let l = a["lookID"] as? String { f.settings.lookID = UUID(uuidString: l) }
                if let b = a["brightness"] as? Int { f.settings.brightness = max(0, min(100, b)) }
                if let n = a["name"] as? String, !n.isEmpty { f.name = String(n.prefix(40)) }
            }
        case "hold": hold(id)
        case "take": takeBack(id)
        case "open": if let u = uuid, looks.contains(where: { $0.id == u }) { lookID = u }
        case "look":
            guard let u = uuid, var l = looks.first(where: { $0.id == u }) else { return }
            if let v = a["name"] as? String, !v.isEmpty { l.name = String(v.prefix(40)) }
            if let v = (a["movement"] as? String).flatMap(LampLook.Movement.init) { l.movement = v }
            if let v = (a["source"] as? String).flatMap(LampLook.Source.init) { l.source = v }
            if let v = (a["direction"] as? String).flatMap(LampLook.Direction.init) { l.direction = v }
            if let v = a["colours"] as? [String] { l.colours = Array(v.compactMap { RGB(hex: $0) }.prefix(6)) }
            if let v = a["random"] as? Bool { l.random = v }
            func int(_ k: String, _ r: ClosedRange<Int>) -> Int? { (a[k] as? Int).map { max(r.lowerBound, min(r.upperBound, $0)) } }
            if let v = int("spotsLow", 1...8) { l.spotsLow = v; l.spotsHigh = max(l.spotsHigh, v) }
            if let v = int("spotsHigh", 1...8) { l.spotsHigh = v; l.spotsLow = min(l.spotsLow, v) }
            if let v = int("sizeLow", 1...10) { l.sizeLow = v; l.sizeHigh = max(l.sizeHigh, v) }
            if let v = int("sizeHigh", 1...10) { l.sizeHigh = v; l.sizeLow = min(l.sizeLow, v) }
            if let v = int("speed", 1...5) { l.speed = v }
            if let v = int("rhythmLow", 0...100) { l.rhythmLow = v; l.rhythmHigh = max(l.rhythmHigh, v) }
            if let v = int("rhythmHigh", 0...100) { l.rhythmHigh = v; l.rhythmLow = min(l.rhythmLow, v) }
            if let i = looks.firstIndex(where: { $0.id == u }) { looks[i] = l }
        case "lookNew": saveAsNew()
        case "lookDelete": deleteLook(uuid)
        case "useOnAll": useOnAll()
        case "sceneSave": saveScene(a["name"] as? String ?? "")
        case "sceneRecall": if let u = uuid { recall(u) }
        case "sceneDelete": if let u = uuid { deleteScene(u) }
        default: break
        }
    }
}
