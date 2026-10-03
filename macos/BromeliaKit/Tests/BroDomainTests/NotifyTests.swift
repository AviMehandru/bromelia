import BroDomain
import BroFoundation
import BroTestSupport
import Testing

/// A delivery against a fixture's expectation: the URL, the headers it names, the body as JSON or text, or the Apprise
/// URL; `delivery: null` for none.
private func sameDelivery(_ expect: JsonValue, _ got: Delivery?) throws {
    if (expect.members ?? []).contains(where: { $0.key == "delivery" }) {
        try Fixtures.check(got == nil, "expected no delivery")
        return
    }
    if let apprise = expect["apprise"]?.string {
        guard case .apprise(let url)? = got else { return try Fixtures.check(false, "expected apprise") }
        try Fixtures.same(apprise, url, "apprise")
        return
    }
    guard case .http(let request)? = got else { return try Fixtures.check(false, "expected an HTTP request") }
    try Fixtures.same("POST", request.method, "method")
    if let url = expect["url"]?.string { try Fixtures.same(url, request.url, "url") }
    for h in expect["headers"]?.members ?? [] {
        try Fixtures.same(h.value.string, request.headers.first { $0.name == h.key }?.value, "header \(h.key)")
    }
    if let json = expect["json"] { try Fixtures.same(Fixtures.sorted(json), Fixtures.sorted(JsonValue.parse(request.body ?? []) ?? .null), "json") }
    if let text = expect["text"]?.string { try Fixtures.same(text, String(decoding: request.body ?? [], as: UTF8.self), "text") }
}

private func status(_ s: String?) -> StatusWord { StatusWord(rawValue: s ?? "") ?? .failed }

private func folder(_ name: String, _ ok: Bool) -> FolderCheck {
    FolderCheck(folder: name, result: VerifyResult.compare(["a.mkv"], hashes: ["a.mkv": ok ? .same : .changed], folderFiles: ["a.mkv"]))
}

struct NotifyTests {
    @Test func notifyCases() throws {
        let failures = try Fixtures.runCases("domain/notify.cases.json") { _, given, expect in
            if let fixture = given["fixture"]?.string {
                for c in try Fixtures.json(fixture)["cases"]?.array ?? [] {
                    try sameDelivery(c["expect"]!, NotifyRequests.build(c["url"]?.string ?? "", title: "T", body: "B", status: status(c["status"]?.string)))
                }
                return true
            }
            if let url = given["url"]?.string {
                try sameDelivery(expect, NotifyRequests.build(url, title: given["title"]?.string ?? "", body: given["body"]?.string ?? "",
                                                              status: status(given["status"]?.string)))
                return true
            }
            if let targets = given["targets"]?.array {
                let list = targets.map { NotifyTarget(id: $0["id"]?.string ?? "", secret: "url", onlyProblems: $0["onlyProblems"]?.bool ?? false) }
                for m in expect["sentTo"]?.members ?? [] {
                    try Fixtures.same(m.value, .array(NotifyRequests.targetsFor(list, status: status(m.key)).map { .string($0.id) }), "sentTo \(m.key)")
                }
                return true
            }
            let outcome = Outcome(rawValue: given["outcome"]?.string ?? "")!
            let job = JobSummary(mode: RipMode(rawValue: given["mode"]?.string ?? "") ?? .mkv, what: given["what"]?.string ?? "",
                                 files: Int(given["files"]?.int ?? 0), path: given["path"]?.string ?? "")
            let message = NotifyMessages.forJob(job, outcome: outcome)
            if let title = expect["title"] { try Fixtures.same(title, message.title.toJson(), "title") }
            if let body = expect["body"] {
                try Fixtures.check(message.lines.count == 1, "one line")
                try Fixtures.same(body, message.lines[0].toJson(), "body")
            }
            if let text = expect["text"]?.string {
                try Fixtures.same(text, try English.render((given["mode"] != nil ? message.title : message.lines[0]).toJson()), "text")
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }

    @Test func checkNotifications() throws {
        let allOk = NotifyMessages.forCheck([folder("A", true), folder("B", true)])
        #expect(try English.render(allOk.title.toJson()) == "Archive check: all 2 folder(s) OK")
        #expect(allOk.lines.count == 1)
        #expect(try English.render(allOk.lines[0].toJson()) == "Every file matches its checksum.")
        let damaged = NotifyMessages.forCheck((0..<12).map { folder("F\($0)", false) } + [folder("Good", true)])
        #expect(try English.render(damaged.title.toJson()) == "Archive check: 12 of 13 folder(s) damaged")
        #expect(damaged.lines.count == 11)
        #expect(try English.render(damaged.lines[0].toJson()) == "F0: 1 changed of 1 file")
        #expect(try English.render(damaged.lines[10].toJson()) == "… and 2 more")
        let cancelled = NotifyMessages.forJob(JobSummary(mode: .backup, what: "X", files: 0, path: "/p"), outcome: .cancelled)
        #expect(try English.render(cancelled.lines[0].toJson()) == "Cancelled")
    }
}
