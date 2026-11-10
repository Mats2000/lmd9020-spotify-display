// The autopilot: a show director. It reads the music once a second (how loud against the song's
// own range, its character changes, the beat, how bright it sounds), sorts the moment into calm,
// groove or peak, and from its style's pool picks a whole-rig recipe (the lamps' look, the bulb's
// style, the display's visualizer and colours, TILES' animation), changing on new sections, on
// drops and every so many bars. Recipes lie over the settings while it runs; nothing is saved.

import Foundation

final class Autopilot {
    enum Style: String, CaseIterable, Codable { case club, lounge, rave }
    enum Tier: String { case calm, groove, peak }

    struct Recipe: Equatable {
        var name: String
        var movement: LampLook.Movement
        var speed: Int
        var source: LampLook.Source
        var direction: LampLook.Direction = .up
        var rhythm: (Int, Int)
        var random = false
        var bulb: BulbStyle
        var kick: Int
        var display: String  // a visualizer name (styles)
        var scheme: String   // a colour scheme name (schemes)
        var tiles: TilesLink.Animation
        var glow: TilesLink.Glow
        static func == (a: Recipe, b: Recipe) -> Bool { a.name == b.name }
    }

    private(set) var style: Style
    private(set) var tier = Tier.groove
    private(set) var recipe: Recipe?
    private(set) var changedAt = Date.distantPast
    private(set) var drops = 0
    var onChange: ((Recipe) -> Void)?

    private var low = 0.1, high = 0.5           // the song's quiet and loud, lately
    private var slow = 0.3                        // its loudness over ~3 s
    private var tierSince = Date.distantPast, lastCalm = Date.distantPast
    private var seenSongStep = 0, title = ""
    private var history: [String] = []
    private var clock = Date()

    init(style: Style) { self.style = style }

    func set(_ s: Style) {
        guard s != style else { return }
        style = s
        change(reason: "style")
    }

    func newSong(_ t: String) {
        guard t != title else { return }
        title = t
        tierSince = clock
        low = slow * 0.6
        high = max(slow * 1.2, low + 0.1)
        change(reason: "song")
    }

    // Once a second, from the rig's reading (now: for replaying a song in a test).
    func step(_ m: Rig.Timing, now: Date = Date()) {
        guard m.playing else { return }
        clock = now
        slow += (m.energy - slow) * 0.3
        low = min(slow, low + (slow - low) * 0.01)
        high = max(slow, high + (slow - high) * 0.01)
        let rel = (slow - low) / max(0.08, high - low)
        let t: Tier = rel > 0.72 ? .peak : rel < 0.32 ? .calm : .groove

        if tier == .calm { lastCalm = now }
        let bar = (m.period > 0.25 ? m.period : 0.5) * 4
        let since = now.timeIntervalSince(changedAt)
        if t != tier, now.timeIntervalSince(tierSince) > 4 {
            // A drop: the peak within ~25 s of being calm (a build between is fine).
            let drop = t == .peak && now.timeIntervalSince(lastCalm) < 25
            tier = t
            tierSince = now
            if drop { drops += 1 }
            // Lounge keeps its slow pace even across the song's arc.
            if drop || style != .lounge || since > minimumGap(bar) { change(reason: drop ? "drop" : "tier") }
            return
        }
        if m.songSteps != seenSongStep {
            seenSongStep = m.songSteps
            if since > minimumGap(bar) { change(reason: "section"); return }
        }
        if since > everyBars(bar) { change(reason: "time") }
    }

    private func minimumGap(_ bar: Double) -> Double {
        switch style {
        case .club: return bar * 8
        case .lounge: return 45
        case .rave: return bar * 2
        }
    }

    private func everyBars(_ bar: Double) -> Double {
        switch style {
        case .club: return bar * 32
        case .lounge: return 100
        case .rave: return bar * 8
        }
    }

    private func change(reason: String) {
        let pool = Self.pool(style, tier)
        // Not one of the last few, so it keeps moving.
        let fresh = pool.filter { !history.suffix(min(pool.count - 1, 3)).contains($0.name) }
        guard let r = (fresh.isEmpty ? pool : fresh).randomElement() else { return }
        history.append(r.name)
        if history.count > 12 { history.removeFirst() }
        recipe = r
        changedAt = clock
        onChange?(r)
    }

    // The pools. Visualizers: Classic Glow, Spectrum, Pulse Halo, Lava, Rave, Fractal. Schemes:
    // Cover Colours, Sentia, Ice, Amber, Soft Club.
    static func pool(_ s: Style, _ t: Tier) -> [Recipe] {
        switch (s, t) {
        case (.club, .calm):
            return [
                Recipe(name: "Low tide", movement: .flow, speed: 1, source: .cover, rhythm: (25, 70), bulb: .wash, kick: 60, display: "Lava", scheme: "Cover Colours", tiles: .ripple, glow: .second),
                Recipe(name: "Breathing room", movement: .flexing, speed: 1, source: .cover, rhythm: (20, 65), bulb: .wash, kick: 50, display: "Fractal", scheme: "Cover Colours", tiles: .spectrum, glow: .second),
                Recipe(name: "Haze", movement: .gradient, speed: 2, source: .scheme, rhythm: (30, 75), bulb: .wash, kick: 60, display: "Classic Glow", scheme: "Soft Club", tiles: .ripple, glow: .main),
            ]
        case (.club, .groove):
            return [
                Recipe(name: "Groove twinkle", movement: .twinkle, speed: 3, source: .cover, rhythm: (15, 100), bulb: .pulse, kick: 100, display: "Classic Glow", scheme: "Cover Colours", tiles: .meter, glow: .white),
                Recipe(name: "Running lights", movement: .chase, speed: 3, source: .cover, rhythm: (15, 95), bulb: .pulse, kick: 100, display: "Spectrum", scheme: "Cover Colours", tiles: .chase, glow: .white),
                Recipe(name: "Pulse floor", movement: .pulse, speed: 3, source: .scheme, rhythm: (10, 100), bulb: .pulse, kick: 110, display: "Pulse Halo", scheme: "Sentia", tiles: .pulse, glow: .main),
                Recipe(name: "Stack it", movement: .stacking, speed: 3, source: .cover, rhythm: (20, 100), bulb: .pulse, kick: 100, display: "Spectrum", scheme: "Cover Colours", tiles: .spectrum, glow: .white),
            ]
        case (.club, .peak):
            return [
                Recipe(name: "Warehouse", movement: .strike, speed: 5, source: .scheme, rhythm: (4, 100), bulb: .strobe, kick: 140, display: "Rave", scheme: "Ice", tiles: .pulse, glow: .white),
                Recipe(name: "Full flex", movement: .flexing, speed: 5, source: .cover, rhythm: (8, 100), bulb: .strobe, kick: 130, display: "Pulse Halo", scheme: "Cover Colours", tiles: .rain, glow: .white),
                Recipe(name: "Lift off", movement: .chase, speed: 5, source: .cover, direction: .outward, rhythm: (5, 100), bulb: .strobe, kick: 140, display: "Rave", scheme: "Cover Colours", tiles: .sparkle, glow: .white),
            ]
        case (.lounge, .calm), (.lounge, .groove), (.lounge, .peak):
            let warm = t == .peak ? 3 : 1
            return [
                Recipe(name: "Slow bloom", movement: .gradient, speed: warm, source: .cover, rhythm: (40, 85), bulb: .wash, kick: 40, display: "Lava", scheme: "Cover Colours", tiles: .ripple, glow: .second),
                Recipe(name: "Tide", movement: .flow, speed: warm, source: .scheme, rhythm: (35, 80), bulb: .wash, kick: 40, display: "Fractal", scheme: "Soft Club", tiles: .spectrum, glow: .second),
                Recipe(name: "Breath", movement: .flexing, speed: 1, source: .cover, rhythm: (30, 80), bulb: .wash, kick: 30, display: "Classic Glow", scheme: "Cover Colours", tiles: .meter, glow: .main),
                Recipe(name: "Amber room", movement: .gradient, speed: 1, source: .scheme, rhythm: (40, 75), bulb: .wash, kick: 30, display: "Lava", scheme: "Amber", tiles: .ripple, glow: .main),
                Recipe(name: "Ice drift", movement: .flow, speed: 2, source: .scheme, direction: .inward, rhythm: (35, 80), bulb: t == .peak ? .pulse : .wash, kick: 60, display: "Fractal", scheme: "Ice", tiles: .spectrum, glow: .second),
            ]
        case (.rave, .calm):
            return [
                Recipe(name: "Charge up", movement: .stacking, speed: 4, source: .scheme, rhythm: (10, 90), bulb: .pulse, kick: 110, display: "Spectrum", scheme: "Sentia", tiles: .rain, glow: .main),
                Recipe(name: "Static", movement: .twinkle, speed: 4, source: .scheme, direction: .random, rhythm: (5, 85), random: true, bulb: .pulse, kick: 110, display: "Fractal", scheme: "Ice", tiles: .sparkle, glow: .second),
            ]
        case (.rave, .groove), (.rave, .peak):
            return [
                Recipe(name: "Laser", movement: .strike, speed: 5, source: .scheme, rhythm: (3, 100), bulb: .strobe, kick: 150, display: "Rave", scheme: "Sentia", tiles: .chase, glow: .white),
                Recipe(name: "Hyper", movement: .chase, speed: 5, source: .scheme, direction: .outward, rhythm: (5, 100), random: true, bulb: .strobe, kick: 140, display: "Rave", scheme: "Soft Club", tiles: .sparkle, glow: .white),
                Recipe(name: "Shock", movement: .strike, speed: 5, source: .cover, direction: .down, rhythm: (2, 100), bulb: .strobe, kick: 150, display: "Pulse Halo", scheme: "Ice", tiles: .pulse, glow: .white),
                Recipe(name: "Glitter", movement: .twinkle, speed: 5, source: .scheme, rhythm: (5, 100), random: true, bulb: .strobe, kick: 130, display: "Spectrum", scheme: "Amber", tiles: .rain, glow: .main),
                Recipe(name: "Flex", movement: .flexing, speed: 5, source: .scheme, direction: .inward, rhythm: (5, 100), bulb: .strobe, kick: 140, display: "Rave", scheme: "Sentia", tiles: .spectrum, glow: .white),
            ]
        }
    }
}
