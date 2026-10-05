import BroDomain
import BroFoundation
import BroTestSupport
import Testing

func errorJson(_ e: BroError) -> JsonValue {
    var members: [(key: String, value: JsonValue)] = [("code", .string(e.code))]
    if !e.params.isEmpty { members.append(("params", .object(e.params))) }
    if let cause = e.cause { members.append(("cause", errorJson(cause))) }
    return .object(members)
}

private func stepOf(_ s: JsonValue) -> StepResult {
    var notice: MakemkvNotice?
    switch s["notice"]?.string {
    case "keyExpired": notice = .keyExpired
    case "evaluationNotStarted": notice = .evaluationNotStarted
    case "versionTooOld": notice = .versionTooOld
    case "libreDriveRequired": notice = .libreDriveRequired
    default: notice = nil
    }
    return StepResult(kind: StepKind(rawValue: s["kind"]?.string ?? "")!, state: StepState(rawValue: s["state"]?.string ?? "")!,
                      error: s["error"].map { BroError($0["code"]?.string ?? "", $0["params"]?.members ?? []) },
                      readErrors: Int(s["readErrors"]?.int ?? 0), quarantined: s["quarantined"]?.bool ?? false, notice: notice,
                      skip: s["skip"].map { BroMessage(MessageCode.parse($0["code"]?.string ?? "")!, $0["params"]?.members ?? []) },
                      affectsOutcome: s["affectsOutcome"]?.bool ?? true)
}

struct OutcomeTests {
    @Test func outcomeCases() throws {
        let failures = try Fixtures.runCases("domain/outcome.cases.json") { _, given, expect in
            if let fixture = given["fixture"]?.string {
                let acc = try accumulate(fixture, readsData: true)
                let run = RunOutcome.classify(acc, exit: ProcessExit(status: Int(given["exitCode"]?.int ?? 0)),
                                              product: RunProduct(rawValue: given["product"]?.string ?? "")!,
                                              newNames: (given["newNames"]?.array ?? []).map { $0.string! })
                try Fixtures.same(expect["statusWord"]?.string, run.status.rawValue, "status word")
                try Fixtures.same(expect["failed"]?.bool, run.status == .failed, "failed")
                try Fixtures.same(expect["readErrors"]?.bool, run.status == .errors, "read errors")
                let text = try run.error.map { try English.render($0.toJson()) }
                if let part = expect["errorContains"]?.string {
                    try Fixtures.check(text?.contains(part) == true, "error \(text ?? "none") doesn't contain \(part)")
                } else {
                    try Fixtures.same(nil, text, "error")
                }
                if let code = expect["errorCode"]?.string { try Fixtures.same(code, run.error?.code.rawValue, "error code") }
                if let produced = expect["produced"]?.array { try Fixtures.same(produced.map { $0.string! }, run.produced, "produced") }
                return true
            }
            if let steps = given["steps"]?.array {
                let d = JobOutcome.decide(steps.map(stepOf), cancelled: given["cancelled"]?.bool ?? false, policy: .default)
                try Fixtures.same(expect["outcome"]?.string, d.outcome.rawValue, "outcome")
                if let error = expect["error"] {
                    try Fixtures.same(error, d.error.map(errorJson) ?? .null, "error")
                } else if d.outcome == .succeeded {
                    try Fixtures.same(nil, d.error, "error")
                }
                return true
            }
            return false
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func stalledAndCancelledRuns() throws {
        let acc = RunAccumulator(readsData: true)
        let stalled = RunOutcome.classify(acc, exit: ProcessExit(status: -1, signal: 9, stalled: Duration(seconds: 600), abandoned: true), product: .titles, newNames: [])
        #expect(stalled.status == .failed)
        #expect(try English.render(stalled.error!.toJson()) == "makemkvcon printed nothing for 10 minutes and was stopped (it did not exit; the drive may need to be reset). The drive or disc may be stuck: eject the disc and retry.")
        #expect(RunOutcome.classify(acc, exit: ProcessExit(status: 143, signal: 15, cancelled: true), product: .titles, newNames: ["title_t00.mkv"]).status == .cancelled)
    }
}
