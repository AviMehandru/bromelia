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

struct ListingTests {
    @Test func listingOfARealDvd() throws {
        let golden = try Fixtures.json("domain/info-dvd.listing.expected.json")
        let actual = Listing.encode(listingOf(try Fixtures.text(golden["input"]?.string ?? "")))
        #expect(golden["expect"] == actual)
        #expect(golden["expect"] == Listing.encode(Listing.decode(golden["expect"] ?? .null)), "read back")
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
