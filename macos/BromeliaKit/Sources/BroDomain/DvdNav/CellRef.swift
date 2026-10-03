/// A menu still: the sectors [first, end) of one VOB/cell id in a menu VOB (input for MenuOcr).
public struct CellRef: Sendable, Equatable {
    public var file: String
    public var firstSector: Int64
    public var endSector: Int64

    public init(file: String, firstSector: Int64, endSector: Int64) {
        self.file = file
        self.firstSector = firstSector
        self.endSector = endSector
    }
}
