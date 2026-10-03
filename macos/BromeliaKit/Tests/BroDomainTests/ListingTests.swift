import BroDomain
import BroFoundation
import BroTestSupport
import Testing

func listingOf(_ robotText: String) -> Listing {
    var b = ListingBuilder()
    for line in robotText.split(separator: "\n", omittingEmptySubsequences: false) {
        if let e = Robot.parseLine(String(line)) { b.feed(e) }
    }
    return b.build()
}

private func attributesJson(_ a: [Int: String]) -> JsonValue {
    .object(a.keys.sorted().map { (String($0), .string(a[$0] ?? "")) })
}

private func orNull(_ v: Int?) -> JsonValue { v.map { .integer(Int64($0)) } ?? .null }

/// A listing as the golden writes it.
func listingJson(_ l: Listing) -> JsonValue {
    .object([
        ("name", .string(l.name)), ("volumeName", .string(l.volumeName)), ("type", .string(l.type.rawValue)),
        ("typeText", .string(l.typeText)), ("reportedTitleCount", .integer(Int64(l.reportedTitleCount))),
        ("titles", .array(l.titles.map { t in
            .object([
                ("index", .integer(Int64(t.index))), ("sourceTitleId", orNull(t.sourceTitleId)), ("sourceFile", .string(t.sourceFile)),
                ("name", .string(t.name)), ("comment", .string(t.comment)), ("duration", .string(t.duration)),
                ("durationSeconds", .integer(Int64(t.durationSeconds))), ("chapters", .integer(Int64(t.chapters))),
                ("sizeBytes", .integer(t.sizeBytes)), ("segmentMap", .string(t.segmentMap)), ("outputFileName", .string(t.outputFileName)),
                ("angle", orNull(t.angle)),
                ("tracks", .array(t.tracks.map { k in
                    .object([("index", .integer(Int64(k.index))), ("kind", .string(k.kind.rawValue)), ("codec", .string(k.codec)),
                             ("language", .string(k.language)), ("languageName", .string(k.languageName)), ("name", .string(k.name)),
                             ("isDefault", .bool(k.isDefault)), ("attributes", attributesJson(k.attributes))])
                })),
                ("attributes", attributesJson(t.attributes)),
            ])
        })),
        ("attributes", attributesJson(l.attributes)),
    ])
}

struct ListingTests {
    @Test func listingOfARealDvd() throws {
        let golden = try Fixtures.json("domain/info-dvd.listing.expected.json")
        let actual = listingJson(listingOf(try Fixtures.text(golden["input"]?.string ?? "")))
        #expect(golden["expect"] == actual)
    }

    @Test func fingerprintCases() throws {
        let failures = try Fixtures.runCases("domain/fingerprint.cases.json") { _, given, expect in
            if let file = given["listingFile"]?.string {
                try Fixtures.same(expect["fingerprint"]?.string, Fingerprint.of(listingOf(try Fixtures.text(file))), "fingerprint")
            } else if let listings = given["listings"]?.array {
                let prints = listings.map { Fingerprint.of(listingOf(Fixtures.generatedListing($0))) }
                try Fixtures.same(expect["fingerprints"], .array(prints.map { $0.map(JsonValue.string) ?? .null }), "fingerprints")
                try Fixtures.same(expect["equal"]?.bool, Set(prints.map { $0 ?? "" }).count == 1, "equal")
            } else if let listing = given["listing"] {
                try Fixtures.same(expect["fingerprint"]?.string, Fingerprint.of(listingOf(Fixtures.generatedListing(listing))), "fingerprint")
            } else {
                return false
            }
            return true
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
