import BroDomain
import BroFoundation
import BroTestSupport
import Testing

func jobRequestOf(_ r: JsonValue) -> JobRequest {
    JobRequest(kind: r["kind"]?.string.flatMap(JobKind.init(rawValue:)) ?? .videoDisc, title: r["title"]?.string ?? "",
               automatic: r["automatic"]?.bool ?? false, driveId: r["driveId"]?.string, mediaGeneration: r["mediaGeneration"]?.int,
               sessionId: r["sessionId"]?.string.map(Id.init), options: r["options"], details: r["details"])
}

private func encoded(_ r: DiscRequest?) -> JsonValue { r.map(DiscRequest.encode) ?? .null }

/// shared/fixtures/domain/job-details.cases.json.
struct JobDetailsTests {
    @Test func theSharedCasesPass() throws {
        let failures = try Fixtures.runCases("domain/job-details.cases.json") { _, given, expect in
            if let request = given["request"] {
                try Fixtures.same(expect["discRequest"], encoded(DiscRequest.decode(jobRequestOf(request))), "discRequest")
            } else if let requests = given["requests"]?.array {
                try Fixtures.same(expect["discRequests"], .array(requests.map { encoded(DiscRequest.decode(jobRequestOf($0))) }), "discRequests")
            } else if let choices = given["choices"] {
                try Fixtures.same(expect["choices"], SessionChoices.encode(SessionChoices.decode(choices)), "choices")
            } else if let identity = given["identity"] {
                try Fixtures.same(expect["identity"], Identity.encode(Identity.decode(identity)), "identity")
            } else if let resolve = given["resolve"] {
                let listing = listingOf(Fixtures.generatedListing(resolve["listing"]!))
                let resolved = Identity.resolve(IdentityInputs(listing: listing, discLabel: listing.volumeName, encrypted: false))
                try Fixtures.same(expect["identity"], Identity.encode(resolved), "identity")
                try Fixtures.same(expect["identity"], Identity.encode(Identity.decode(Identity.encode(resolved))), "identity read back")
            } else if let message = given["message"] {
                let m = BroMessage.decode(message)
                try Fixtures.same(expect["message"], m.toJson(), "message")
                try Fixtures.same(expect["severity"]?.string, m.severity.rawValue, "severity")
            } else if let of = given["automationOf"]?.array {
                let config = try decodeConfig(given["config"]!)
                try Fixtures.same(expect["automation"], .array(of.map { Config.automation(config, $0.string) }), "automation")
            } else if let kinds = given["outputOf"]?.array {
                let outputs = (given["outputs"]?.array ?? []).map { StepOutput(kind: StepKind(rawValue: $0["kind"]?.string ?? "")!, data: $0["data"]!) }
                let job = JobSnapshot(id: Id("00000000-0000-4000-8000-000000000001"), request: JobRequest(kind: .videoDisc, title: ""),
                                      plan: nil, outputs: outputs)
                try Fixtures.same(expect["outputs"], .array(kinds.map { JobSnapshot.output(job, StepKind(rawValue: $0.string ?? "")!) ?? .null }),
                                  "outputs")
            } else {
                return false
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
