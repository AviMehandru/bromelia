import BroAdapters
import BroDomain
import BroFoundation
import BroPorts
import BroTestSupport
import Testing

/// shared/fixtures/adapters/message-renderer.cases.json.
struct MessageTextRendererTests {
    static let cases = "adapters/message-renderer.cases.json"

    // The cases' own catalog: texts by code, and the params that are message lists.
    func testCatalog() throws -> MessageTexts {
        let catalog = try Fixtures.json(Self.cases)["catalog"]
        var texts: [String: String] = [:]
        for (code, text) in catalog?["texts"]?.members ?? [] { texts[code] = text.string }
        let lists = (catalog?["messageLists"]?.members ?? []).flatMap { m in (m.value.array ?? []).map { (code: m.key, param: $0.string ?? "") } }
        return MessageTexts(texts: texts, messageLists: lists)
    }

    @Test func theSharedCasesPass() throws {
        let test = MessageTextRenderer(try testCatalog())
        let english = MessageTextRenderer(EnglishMessages.catalog)
        let failures = try Fixtures.runCases(Self.cases) { _, given, expect in
            let renderer = given["catalog"]?.string == "english" ? english : test
            let message = given["message"] ?? .null
            let text = given["via"]?.string == "port" ? (renderer as any MessageRenderer).render(BroMessage.decode(message)) : renderer.renderJson(message)
            try Fixtures.same(expect["text"]?.string, text, "text")
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
