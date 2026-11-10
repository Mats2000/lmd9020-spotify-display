// The venue: every light found (Govee lamps, Matter lights), each with its own settings, and
// scenes, the whole room saved under a name (each light's look, the display's style).

import Foundation

struct FixtureSettings: Codable, Equatable {
    var on = true
    var lookID: UUID?  // nil: the look open on the Looks page
    var brightness = 100  // %, on top of the look's own rhythm
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
