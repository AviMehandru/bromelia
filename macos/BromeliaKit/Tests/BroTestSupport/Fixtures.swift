import BroFoundation
import Foundation

/// Reads the shared golden fixtures (shared/fixtures) and runs their cases.
public enum Fixtures {
    /// BROMELIA_FIXTURES (as on Linux), else the repository this file was compiled from.
    public static let root: URL = {
        if let env = ProcessInfo.processInfo.environment["BROMELIA_FIXTURES"], !env.isEmpty {
            return URL(fileURLWithPath: env)
        }
        var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        while dir.path != "/" {
            let candidate = dir.appendingPathComponent("shared/fixtures")
            if FileManager.default.fileExists(atPath: candidate.path) { return candidate }
            dir = dir.deletingLastPathComponent()
        }
        fatalError("shared/fixtures not found above \(#filePath)")
    }()

    public static func url(_ relative: String) -> URL { root.appendingPathComponent(relative) }

    public static func bytes(_ relative: String) throws -> [UInt8] { Array(try Data(contentsOf: url(relative))) }

    public static func text(_ relative: String) throws -> String { String(decoding: try bytes(relative), as: UTF8.self) }

    /// A file elsewhere in shared/ (schema/common.json, messages/codes.json).
    public static func sharedJson(_ relative: String) throws -> JsonValue { try json("../" + relative) }

    public static func json(_ relative: String) throws -> JsonValue {
        guard let v = JsonValue.parse(try bytes(relative)) else { throw FixtureError("\(relative) isn't JSON") }
        return v
    }

    /// Runs every case of a `*.cases.json` file. `run` returns false for a case it doesn't know, which fails:
    /// no case is skipped silently. Returns the failures, one line each (empty when all pass).
    /// `only` picks the cases by id, for a file whose other cases belong to a module that isn't built yet.
    public static func runCases(_ relative: String, only: (String) -> Bool = { _ in true },
                                _ run: (_ id: String, _ given: JsonValue, _ expect: JsonValue) throws -> Bool) throws -> [String] {
        let doc = try json(relative)
        var failures: [String] = []
        let cases = (doc["cases"]?.array ?? []).filter { only($0["id"]?.string ?? "") }
        if cases.isEmpty { return ["\(relative) has no cases"] }
        for c in cases {
            let id = c["id"]?.string ?? "?"
            do {
                if try !run(id, c["given"] ?? .null, c["expect"] ?? .null) { failures.append("\(id): no test handles this case") }
            } catch {
                failures.append("\(id): \(error)")
            }
        }
        return failures.map { "\(relative): \($0)" }
    }

    /// The robot-mode listing of a disc definition (shared/scenarios/README.md, "A generated listing"):
    /// `{volume, type, titles}`, each title `{duration, source, size, chapters}` or `[duration, source]`.
    public static func generatedListing(_ disc: JsonValue) -> String {
        let volume = disc["volume"]?.string ?? ""
        let type = disc["type"]?.string ?? "bluray"
        let titles = disc["titles"]?.array ?? []
        var s = "MSG:1005,0,1,\"MakeMKV v1.18.1 darwin(arm64-release) started\",\"%1 started\",\"MakeMKV v1.18.1 darwin(arm64-release)\"\n"
        s += "TCOUNT:\(titles.count)\n"
        switch type {
        case "dvd": s += "CINFO:1,6206,\"DVD disc\"\n"
        case "hddvd": s += "CINFO:1,6207,\"HD-DVD disc\"\n"
        default: s += "CINFO:1,6209,\"Blu-ray disc\"\n"
        }
        s += "CINFO:2,0,\"\(volume)\"\nCINFO:32,0,\"\(volume)\"\n"
        for (i, t) in titles.enumerated() {
            let pair = t.array
            let duration = pair?[0].string ?? t["duration"]?.string ?? ""
            let source = pair?[1].int ?? t["source"]?.int ?? 0
            let size = pair == nil ? t["size"]?.int : nil
            let chapters = pair == nil ? t["chapters"]?.int ?? 2 : 2
            s += "TINFO:\(i),8,0,\"\(chapters)\"\nTINFO:\(i),9,0,\"\(duration)\"\nTINFO:\(i),16,0,\"0000\(source).mpls\"\n"
            s += "TINFO:\(i),24,0,\"\(source)\"\nTINFO:\(i),26,0,\"\(source)\"\nTINFO:\(i),27,0,\"title_t0\(i).mkv\"\n"
            if let size { s += "TINFO:\(i),11,0,\"\(size)\"\n" }
            s += "SINFO:\(i),0,1,6201,\"Video\"\n"
            if type == "uhd" { s += "SINFO:\(i),0,19,0,\"3840x2160\"\n" }
            s += "SINFO:\(i),1,1,6202,\"Audio\"\n"
        }
        s += "MSG:5011,0,0,\"Operation successfully completed\",\"Operation successfully completed\"\n"
        return s
    }

    /// Throws unless `condition` holds.
    public static func check(_ condition: Bool, _ what: @autoclosure () -> String) throws {
        if !condition { throw FixtureError(what()) }
    }

    /// Throws when the two differ, naming the value.
    public static func same<T: Equatable>(_ expected: T, _ actual: T, _ what: String) throws {
        if expected != actual { throw FixtureError("\(what): expected \(expected), got \(actual)") }
    }
}

public struct FixtureError: Error, CustomStringConvertible {
    public let description: String
    public init(_ description: String) { self.description = description }
}
