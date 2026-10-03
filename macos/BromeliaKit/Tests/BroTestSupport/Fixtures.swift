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
    public static func runCases(_ relative: String, _ run: (_ id: String, _ given: JsonValue, _ expect: JsonValue) throws -> Bool) throws -> [String] {
        let doc = try json(relative)
        var failures: [String] = []
        let cases = doc["cases"]?.array ?? []
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

    /// Throws when the two differ, naming the value.
    public static func same<T: Equatable>(_ expected: T, _ actual: T, _ what: String) throws {
        if expected != actual { throw FixtureError("\(what): expected \(expected), got \(actual)") }
    }
}

public struct FixtureError: Error, CustomStringConvertible {
    public let description: String
    public init(_ description: String) { self.description = description }
}
