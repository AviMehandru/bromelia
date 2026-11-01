import BroFoundation
import BroTestSupport
import Foundation
import Testing

/// Every shared scenario this runner selects (ScenarioCatalog), each its own test case named by its id; a selected scenario
/// that fails is a failure, never a skip. shared/fixtures/engine/scenario-runner.cases.json checks the runner itself: each
/// broken variant of a scenario must fail, with the failure text the case gives. One scenario at a time, as the C# runner
/// plays them (each waits in real time for its engine to settle).
@Suite(.serialized)
struct ScenarioRunnerTests {
    @Test(arguments: ScenarioCatalog.selected())
    func scenario(_ id: String) async throws {
        do {
            try await ScenarioRun.play(id)
        } catch {
            Issue.record("\(error)")
        }
    }

    @Test func theRunnerCatchesBrokenScenarios() async throws {
        let doc = try Fixtures.json("engine/scenario-runner.cases.json")
        let cases = doc["cases"]?.array ?? []
        #expect(!cases.isEmpty, "scenario-runner.cases.json has no cases")
        var failures: [String] = []
        for c in cases {
            let id = c["id"]?.string ?? "?"
            let given = c["given"] ?? .null
            let name = given["scenario"]?.string ?? ""
            let wanted = c["expect"]?["failure"]?.string ?? ""
            do {
                let broken = Self.set(try ScenarioCatalog.load(name), at: given["pointer"]?.string ?? "", to: given["value"] ?? .null)
                guard let failure = await Self.failure(name, broken) else {
                    failures.append("\(id): the broken \(name) passed; wanted a failure with: \(wanted)")
                    continue
                }
                if !Self.contains(failure, wanted) {
                    failures.append("\(id): the broken \(name) failed with:\n\(failure)\nwanted it to contain:\n\(wanted)")
                }
            } catch {
                failures.append("\(id): \(error)")
            }
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    /// The failure text of playing the scenario; nil when it passes.
    private static func failure(_ id: String, _ scenario: JsonValue) async -> String? {
        do {
            try await ScenarioRun.play(id, scenario)
            return nil
        } catch {
            return "\(error)"
        }
    }

    private static func contains(_ text: String, _ part: String) -> Bool {
        let t = Array(text.utf8), p = Array(part.utf8)
        if p.isEmpty { return true }
        guard t.count >= p.count else { return false }
        return (0...(t.count - p.count)).contains { Array(t[$0..<$0 + p.count]) == p }
    }

    /// The document with the value at the JSON pointer (RFC 6901) replaced, or added to its object.
    static func set(_ doc: JsonValue, at pointer: String, to value: JsonValue) -> JsonValue {
        let parts = pointer.split(separator: "/", omittingEmptySubsequences: false).dropFirst()
            .map { $0.replacingOccurrences(of: "~1", with: "/").replacingOccurrences(of: "~0", with: "~") }
        func set(_ at: JsonValue, _ i: Int) -> JsonValue {
            if i == parts.count { return value }
            let key = parts[i]
            if var items = at.array, let n = Int(key), n < items.count {
                items[n] = set(items[n], i + 1)
                return .array(items)
            }
            var members = at.members ?? []
            if let k = members.firstIndex(where: { $0.key == key }) {
                members[k] = (key, set(members[k].value, i + 1))
            } else {
                members.append((key, set(.object([]), i + 1)))
            }
            return .object(members)
        }
        return set(doc, 0)
    }
}
