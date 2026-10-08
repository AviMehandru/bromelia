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
        let parent = CancellationSource()
        let child = CancellationSource.linked(to: parent.token)
        final class Counter: @unchecked Sendable { var calls = 0 }
        let counter = Counter()
        child.token.onCancel { counter.calls += 1 }
        #expect(!child.token.isCancelled)
        parent.cancel()
        parent.cancel()
        #expect(child.token.isCancelled)
        #expect(counter.calls == 1)
    }

    /// A closed (or freed) linked source no longer follows its parent: a parent that lives long doesn't keep a handler
    /// per child.
    @Test func aClosedLinkedSourceLetsGoOfItsParent() {
        let parent = CancellationSource()
        let child = CancellationSource.linked(to: parent.token)
        child.close()
        var freed: CancellationSource? = CancellationSource.linked(to: parent.token)
        let gone = Weak(value: freed) // not `weak let`: CI's Xcode 16.4 (Swift 6.1) doesn't have it
        freed = nil
        #expect(gone.value == nil)
        parent.cancel()
        #expect(!child.token.isCancelled)
        #expect(CancellationSource.linked(to: parent.token).token.isCancelled)
    }
}

/// Holds an object without keeping it alive.
private struct Weak<T: AnyObject> {
    weak var value: T?
}
