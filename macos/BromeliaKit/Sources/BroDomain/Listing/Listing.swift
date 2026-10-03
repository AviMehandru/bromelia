/// What makemkvcon reported about a disc, ISO or folder: its name (2, else 32), volume name (32), type (from 1),
/// the number of titles it announced, the titles in index order, and every disc attribute.
public struct Listing: Sendable, Equatable {
    public var name: String
    public var volumeName: String
    public var type: DiscType
    public var typeText: String
    public var reportedTitleCount: Int
    public var titles: [Title]
    public var attributes: [Int: String]

    public init(name: String, volumeName: String, type: DiscType, typeText: String, reportedTitleCount: Int, titles: [Title],
                attributes: [Int: String]) {
        self.name = name
        self.volumeName = volumeName
        self.type = type
        self.typeText = typeText
        self.reportedTitleCount = reportedTitleCount
        self.titles = titles
        self.attributes = attributes
    }

    public static let empty = Listing(name: "", volumeName: "", type: .disc, typeText: "", reportedTitleCount: 0, titles: [], attributes: [:])
}
