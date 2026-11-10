// The venue: every light found (Govee lamps, Matter lights), each with its own look and settings,
// and scenes, the whole room saved under a name (each light's look, the display's style).

import Foundation

struct FixtureSettings: Codable, Equatable {
    var on = true
    var look = LampLook.builtIn[0]  // this light's own look (presets are copied in)
    var brightness = 100            // %, on top of the look's own rhythm
    var kick = 100                  // % of the kick's flash: 0 follows the music without it

    init() {}

    // Older saves had no look of their own (a shared one by id) and no kick: filled in.
    private enum Keys: String, CodingKey { case on, look, brightness, kick, lookID }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: Keys.self)
        on = try c.decodeIfPresent(Bool.self, forKey: .on) ?? true
        brightness = try c.decodeIfPresent(Int.self, forKey: .brightness) ?? 100
        kick = try c.decodeIfPresent(Int.self, forKey: .kick) ?? 100
        if let l = try c.decodeIfPresent(LampLook.self, forKey: .look) {
            look = l
        } else {
            // A shared look by id, or none: the one that was open then.
            let id = try c.decodeIfPresent(UUID.self, forKey: .lookID) ?? UUID(uuidString: UserDefaults.standard.string(forKey: "look") ?? "")
            if let l = LookStore.load().first(where: { $0.id == id }) { look = l }
        }
    }
    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: Keys.self)
        try c.encode(on, forKey: .on)
        try c.encode(look, forKey: .look)
        try c.encode(brightness, forKey: .brightness)
        try c.encode(kick, forKey: .kick)
    }
}

struct Fixture: Identifiable, Equatable {
    enum Kind: String { case govee, matter }
    let id: String  // "govee:<ip>" or "matter:<name>"
    var name: String
    var kind: Kind
    var segments: Int
    var settings = FixtureSettings()
    var held = false  // someone else (Alexa) has it: left alone until taken back
    var address: String { String(id.drop { $0 != ":" }.dropFirst()) }
}

struct ShowScene: Codable, Identifiable, Equatable {
    var id = UUID()
    var name: String
    var fixtures: [String: FixtureSettings]
    var style: String
    var scheme: String
}

// ~/Library/Application Support/Show Control/venue.json: each light's settings and the scenes.
enum VenueStore {
    struct Saved: Codable {
        var fixtures: [String: FixtureSettings] = [:]
        var names: [String: String] = [:]
        var scenes: [ShowScene] = []
        var held: [String] = []  // lights someone else has
    }
    private static var file: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("Show Control")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("venue.json")
    }
    static func load() -> Saved {
        guard let data = try? Data(contentsOf: file), let saved = try? JSONDecoder().decode(Saved.self, from: data) else { return Saved() }
        return saved
    }
    static func save(_ saved: Saved) {
        if let data = try? JSONEncoder().encode(saved) { try? data.write(to: file) }
    }
}
