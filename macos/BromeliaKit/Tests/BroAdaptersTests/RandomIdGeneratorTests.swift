import BroAdapters
import BroFoundation
import Foundation
import Testing

struct RandomIdGeneratorTests {
    @Test func idsAreLowerCaseVersion4UuidsAndNeverRepeat() throws {
        let ids = RandomIdGenerator()
        let shape = try NSRegularExpression(pattern: "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")
        var seen = Set<String>()
        for _ in 0..<10000 {
            let id = ids.next().value
            #expect(shape.firstMatch(in: id, range: NSRange(id.startIndex..., in: id)) != nil, "\(id)")
            #expect(seen.insert(id).inserted, "\(id)")
        }
    }
}
