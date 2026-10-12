import BroFoundation

/// What ReconcileStep found: each chosen title of the opened listing → its number in the listing read now.
public struct ReconcileOutput: Sendable, Equatable {
    public var titleMap: [Int: Int]

    public init(titleMap: [Int: Int]) { self.titleMap = titleMap }

    /// `{"titleMap": [[opened, now], …]}`, by the opened number.
    public static func encode(_ output: ReconcileOutput) -> JsonValue {
        .object([("titleMap", .array(output.titleMap.keys.sorted().map { .array([.integer(Int64($0)), .integer(Int64(output.titleMap[$0]!))]) }))])
    }

    public static func decode(_ json: JsonValue) -> ReconcileOutput {
        var map: [Int: Int] = [:]
        for pair in json["titleMap"]?.array ?? [] {
            if let p = pair.array, p.count == 2, let from = p[0].int, let to = p[1].int { map[Int(from)] = Int(to) }
        }
        return ReconcileOutput(titleMap: map)
    }
}
