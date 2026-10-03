/// A name and the year typed after it: "Inception (2010)".
public struct NameAndYear: Sendable, Equatable {
    public var name: String
    public var year: Int?

    public init(name: String, year: Int?) {
        self.name = name
        self.year = year
    }
}
