// The way out to the phone page on the web (showcontrol.matiascevallos.com, see site/): the Mac
// keeps a request open to the Vercel relay, which hands over a tap the moment one arrives. While a
// phone is looking (it wakes the Mac every 10 s) the state goes up a few times a second, otherwise
// the Mac only waits. Nothing is opened on the home network. The relay's address and secret are in
// the app's defaults (relayURL, relaySecret); without them this stays off.

import Foundation

final class Relay {
    private let show: Show
    private let url: URL
    private let secret: String
    private let session: URLSession
    private var lookingUntil = Date.distantPast
    private var sentAuth: [String: AnyHashable]?  // none set: the relay forgets any it had

    init?(show: Show) {
        let d = UserDefaults.standard
        guard let u = d.string(forKey: "relayURL").flatMap(URL.init(string:)), let s = d.string(forKey: "relaySecret"), s.count >= 32 else { return nil }
        self.show = show
        url = u.appendingPathComponent("api/mac")
        secret = s
        let c = URLSessionConfiguration.ephemeral
        c.timeoutIntervalForRequest = 45
        session = URLSession(configuration: c)
        Thread { self.loop() }.start()
    }

    private func loop() {
        while true {
            let looking = Date() < lookingUntil
            var body: [String: Any] = ["wait": looking ? 0.6 : 25]
            let (state, auth): ([String: Any]?, [String: AnyHashable]) = DispatchQueue.main.sync {
                if looking { show.phoneWatching = Date() }
                return (looking ? show.webState() : nil, WebAuth.shared.relayCheck)
            }
            if let state { body["state"] = state }
            if auth != sentAuth { body["auth"] = auth }
            guard let taps = post(body) else {
                Thread.sleep(forTimeInterval: 5)  // offline or the relay is down: try again shortly
                continue
            }
            if body["auth"] != nil { sentAuth = auth }
            if !taps.isEmpty {
                lookingUntil = Date().addingTimeInterval(30)
                let real = taps.filter { $0["action"] as? String != "wake" }
                if !real.isEmpty { DispatchQueue.main.sync { for t in real { show.perform(t) } } }
            }
        }
    }

    private func post(_ body: [String: Any]) -> [[String: Any]]? {
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.setValue(secret, forHTTPHeaderField: "X-Mac-Secret")
        req.httpBody = try? JSONSerialization.data(withJSONObject: body)
        let done = DispatchSemaphore(value: 0)
        var taps: [[String: Any]]?
        session.dataTask(with: req) { data, response, _ in
            if (response as? HTTPURLResponse)?.statusCode == 200, let data,
               let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                taps = json["taps"] as? [[String: Any]] ?? []
            }
            done.signal()
        }.resume()
        done.wait()
        return taps
    }
}
