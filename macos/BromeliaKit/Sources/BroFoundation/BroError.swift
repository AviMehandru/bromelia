/// An expected failure (plan §16): a message code in its wire form (`"library.offline"`), its parameters and
/// the failure that caused it. Domain's `MessageCode` gives the codes their names.
public struct BroError: Error, Sendable, Equatable {
    public var code: String
    public var params: [(key: String, value: JsonValue)]
    // An array, because a struct can't hold itself directly; at most one element.
    private var causes: [BroError]

    public var cause: BroError? {
        get { causes.first }
        set { causes = newValue.map { [$0] } ?? [] }
    }

    public init(_ code: String, _ params: [(key: String, value: JsonValue)] = [], cause: BroError? = nil) {
        self.code = code
        self.params = params
        self.causes = cause.map { [$0] } ?? []
    }

    public static func == (a: BroError, b: BroError) -> Bool {
        a.code == b.code && JsonValue.object(a.params) == JsonValue.object(b.params) && a.causes == b.causes
    }
}
