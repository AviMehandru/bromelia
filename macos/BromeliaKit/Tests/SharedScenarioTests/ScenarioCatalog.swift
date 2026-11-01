import BroFoundation
import BroTestSupport
import Foundation

/// The shared scenarios this runner plays (decisions 233, 234): the JSON copies in shared/scenarios/json, next to
/// shared/fixtures, read as JSON values. A runner selects every scenario with steps whose phase it has built (`builtPhase`)
/// and whose platforms, when given, name its own; realDisc scenarios are session I's.
enum ScenarioCatalog {
    /// The engine phase this runner can run (each phase raises it on all three platforms).
    static let builtPhase: Int64 = 3
    /// The platform name a scenario's platforms list uses for this runner.
    static let platform = "macos"

    /// shared/scenarios/json.
    static let folder = Fixtures.root.deletingLastPathComponent().appendingPathComponent("scenarios/json")

    /// The ids of the scenarios this runner plays, sorted.
    static func selected() -> [String] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        return names.filter { $0.hasSuffix(".json") }.map { String($0.dropLast(5)) }
            .filter { (try? load($0)).map(selects) ?? false }
            .sorted { $0.utf8.lexicographicallyPrecedes($1.utf8) }
    }

    /// Whether this runner plays the scenario.
    static func selects(_ scenario: JsonValue) -> Bool {
        guard scenario["steps"] != nil, scenario["realDisc"] == nil, let phase = scenario["phase"]?.int, phase <= builtPhase else { return false }
        guard let platforms = scenario["platforms"]?.array else { return true }
        return platforms.contains { $0.string == platform }
    }

    /// A scenario by id.
    static func load(_ id: String) throws -> JsonValue { try read(id + ".json") }

    /// A file under shared/scenarios/json; a path the scenario gives (tools/x.yaml) is read as its JSON copy.
    static func read(_ relative: String) throws -> JsonValue {
        var relative = relative
        if relative.hasSuffix(".yaml") { relative = String(relative.dropLast(5)) + ".json" }
        let bytes = Array(try Data(contentsOf: folder.appendingPathComponent(relative)))
        guard let value = JsonValue.parse(bytes) else { throw ScenarioFailure(relative + " isn't JSON") }
        return value
    }

    /// A disc: a name in discs/, or a definition written inline.
    static func disc(_ disc: JsonValue) throws -> JsonValue {
        if let name = disc.string { return try read("discs/" + name + ".json") }
        return disc
    }

    /// A tool script: a path (tools/x.yaml), or a script written inline.
    static func script(_ script: JsonValue) throws -> JsonValue {
        if let path = script.string { return try read(path) }
        return script
    }

    /// The named robot lines (tools/lines.yaml).
    static func lines() throws -> [String: String] {
        var named: [String: String] = [:]
        for m in try read("tools/lines.json").members ?? [] { named[m.key] = m.value.string ?? "" }
        return named
    }
}
