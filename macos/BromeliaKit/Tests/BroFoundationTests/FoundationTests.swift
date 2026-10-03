import BroFoundation
import BroTestSupport
import Testing

struct FoundationTests {
    @Test func foundationCases() throws {
        let failures = try Fixtures.runCases("foundation/foundation.cases.json") { _, given, expect in
            if let id = given["id"]?.string {
                try Fixtures.same(expect["short"]?.string, Id.short(Id(id)), "short id")
                return true
            }
            if let text = given["instant"]?.string {
                try Fixtures.same(expect["formatted"]?.string, Instant.parse(text).map(Instant.format), "instant")
                return true
            }
            if let clock = given["clock"]?.string {
                let d = Duration.parseClock(clock)
                try Fixtures.same(expect["seconds"]?.int, d.map { Int64($0.seconds) }, "seconds")
                try Fixtures.same(expect["formatted"]?.string, d.map(Duration.formatClock), "formatted")
                return true
            }
            if let json = given["json"]?.string {
                let v = JsonValue.parse(json)
                try Fixtures.same(expect["canonical"]?.string, v.map { String(decoding: JsonValue.encodeCanonical($0), as: UTF8.self) }, "canonical")
                return true
            }
            return false
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func jsonValuesCompareByContent() {
        #expect(JsonValue.parse("{\"a\": [1, {\"b\": null}]}") == JsonValue.parse("{ \"a\" : [ 1 , { \"b\" : null } ] }"))
        #expect(JsonValue.parse("{\"a\": 1, \"b\": 2}") != JsonValue.parse("{\"b\": 2, \"a\": 1}"))
        #expect(JsonValue.parse("1") != JsonValue.parse("1.0"))
    }

    @Test func cancellationRunsHandlersOnce() {
        let token = CancellationToken()
        let child = CancellationToken.child(of: token)
        final class Counter: @unchecked Sendable { var calls = 0 }
        let counter = Counter()
        child.onCancel { counter.calls += 1 }
        #expect(!child.isCancelled)
        token.cancel()
        token.cancel()
        #expect(child.isCancelled)
        #expect(counter.calls == 1)
    }
}
