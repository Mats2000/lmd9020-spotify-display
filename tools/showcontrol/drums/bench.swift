// The test bench: runs Drums.swift over a recording (32-bit float mono WAV) in 10 ms pieces, as
// the app gets them, and writes each hit (when it was found, where it says it started, strength)
// and, with a third argument, every step's measurements for train.py.
//   swiftc -O -parse-as-library -o bench bench.swift ../Drums.swift ../DrumModel.swift
//   ./bench rec.wav rec.hits.json [rec.feat]
import Foundation

@main
enum Bench {
    static func main() {
        let a = CommandLine.arguments
        let data = try! Data(contentsOf: URL(fileURLWithPath: a[1]))
        var rate = 48000.0
        let x: [Float] = data.withUnsafeBytes { raw in
            var off = 12, out: [Float] = []
            while off + 8 <= raw.count {
                let id = String(decoding: raw[off..<off + 4], as: UTF8.self)
                let size = Int(raw.loadUnaligned(fromByteOffset: off + 4, as: UInt32.self))
                if id == "fmt " { rate = Double(raw.loadUnaligned(fromByteOffset: off + 12, as: UInt32.self)) }
                if id == "data" {
                    out = (0..<size / 4).map { raw.loadUnaligned(fromByteOffset: off + 8 + $0 * 4, as: Float.self) }
                    break
                }
                off += 8 + size
            }
            return out
        }
        var hits: [String: [[Double]]] = ["kick": [], "snare": [], "hat": []]
        var i = 0, n = 480
        let d = Drums()
        d.onHit = { k, s, at in hits["\(k)"]!.append([Double(i + n) / rate, at, Double(s)]) }
        var dump = Data()
        if a.count > 3 { d.onFeatures = { f, t in let row = [Float(t)] + f; row.withUnsafeBytes { dump.append(contentsOf: $0) } } }
        while i < x.count {
            n = min(480, x.count - i)
            d.push(Array(x[i..<i + n]), rate: rate, played: Double(i) / rate)
            i += n
        }
        try! JSONSerialization.data(withJSONObject: hits).write(to: URL(fileURLWithPath: a[2]))
        if a.count > 3 { try! dump.write(to: URL(fileURLWithPath: a[3])) }
        print("\(hits["kick"]!.count) kicks, \(hits["snare"]!.count) snares, \(hits["hat"]!.count) hats")
    }
}
