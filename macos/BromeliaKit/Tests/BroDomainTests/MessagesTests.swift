import BroDomain
import BroFoundation
import BroTestSupport
import Testing

struct MessagesTests {
    @Test func everyCodeOfTheCatalogueIsACase() throws {
        let codes = try Fixtures.sharedJson("messages/codes.json")["codes"]?.members?.map(\.key) ?? []
        #expect(codes == MessageCode.allCases.map(MessageCode.wire))
        for code in codes { #expect(MessageCode.parse(code).map(MessageCode.wire) == code) }
        #expect(MessageCode.parse("no.suchCode") == nil)
        #expect(MessageCode.wire(.ripReadErrors) == "rip.readErrors")
    }

    @Test func messagesBecomeErrorsAndJson() {
        let m = BroMessage(.libraryOffline, [("library", .string("Films"))], severity: .error)
        #expect(m.toError() == BroError("library.offline", [("library", .string("Films"))]))
        #expect(m.toJson() == JsonValue.parse("{\"code\": \"library.offline\", \"params\": {\"library\": \"Films\"}}"))
    }

    @Test func jobEnumsMatchTheSharedSchema() throws {
        let defs = try Fixtures.sharedJson("schema/common.json")["$defs"]
        func check<T: CaseIterable & RawRepresentable>(_: T.Type, _ name: String) where T.RawValue == String {
            let expected = defs?[name]?["enum"]?.array?.compactMap(\.string) ?? []
            #expect(expected == T.allCases.map(\.rawValue), "\(name)")
        }
        check(JobKind.self, "JobKind")
        check(JobState.self, "JobState")
        check(Outcome.self, "Outcome")
        check(StepKind.self, "StepKind")
        check(StepState.self, "StepState")
        check(Queue.self, "Queue")
        check(Severity.self, "Severity")
    }
}
