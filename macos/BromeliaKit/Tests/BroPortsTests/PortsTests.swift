import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Foundation
import Testing

/// A keystore in memory, through the port.
final class MemoryKeystore: Keystore, @unchecked Sendable {
    private var secrets: [String: String] = [:]
    private let lock = NSLock()

    func get(_ name: String) throws(BroError) -> String? { lock.withLock { secrets[name] } }
    func set(_ name: String, value: String) throws(BroError) { lock.withLock { secrets[name] = value } }
    func remove(_ name: String) throws(BroError) { _ = lock.withLock { secrets.removeValue(forKey: name) } }
}

/// The values a column's CHECK (column IN (…)) allows in shared/schema/db/0001_init.sql.
func allowed(_ table: String, _ column: String) throws -> [String] {
    let sql = try Fixtures.text("../schema/db/0001_init.sql")
    func first(_ pattern: String, _ text: String, options: NSRegularExpression.Options = []) -> String? {
        let re = try! NSRegularExpression(pattern: pattern, options: options)
        guard let m = re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)), let r = Range(m.range(at: 1), in: text) else { return nil }
        return String(text[r])
    }
    let block = first("CREATE TABLE \(table) \\((.*?)\\n\\) STRICT;", sql, options: [.dotMatchesLineSeparators]) ?? ""
    let list = first("CHECK \\(\(column)(?: IS NULL OR \(column))? IN \\(([^)]*)\\)\\)", block) ?? ""
    let re = try! NSRegularExpression(pattern: "'([^']*)'")
    return re.matches(in: list, range: NSRange(list.startIndex..., in: list)).map { String(list[Range($0.range(at: 1), in: list)!]) }
}

struct PortsTests {
    @Test func enumsMatchTheDatabase() throws {
        #expect(try allowed("archive_units", "state") == UnitState.allCases.map(\.rawValue))
        #expect(try allowed("archive_units", "status") == UnitStatus.allCases.map(\.rawValue))
        #expect(try allowed("replicas", "state") == ReplicaState.allCases.map(\.rawValue))
        #expect(try allowed("jobs", "state") == JobState.allCases.map(\.rawValue))
        #expect(try allowed("jobs", "outcome") == Outcome.allCases.map(\.rawValue))
        #expect(try allowed("jobs", "queue") == Queue.allCases.map(\.rawValue))
        #expect(try allowed("job_steps", "state") == StepState.allCases.map(\.rawValue))
        #expect(try allowed("checks", "result") == CheckResult.allCases.map(\.rawValue))
        #expect(try allowed("works", "kind") == MediaKind.allCases.map(\.rawValue))
    }

    @Test func aPortCanBeImplemented() throws {
        let keystore: any Keystore = MemoryKeystore()
        #expect(try keystore.get("metadata.apiKey") == nil)
        try keystore.set("metadata.apiKey", value: "k")
        #expect(try keystore.get("metadata.apiKey") == "k")
        try keystore.remove("metadata.apiKey")
        #expect(try keystore.get("metadata.apiKey") == nil)
    }
}
