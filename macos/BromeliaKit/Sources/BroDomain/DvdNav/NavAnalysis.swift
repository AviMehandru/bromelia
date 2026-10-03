/// What analyse found: every title's chapters, how the menus and pre-commands reach chapters, and the menu stills.
public struct NavAnalysis: Sendable, Equatable {
    public var titles: [NavTitle]
    public var jumps: [NavJump]
    public var stills: [CellRef]

    public init(titles: [NavTitle], jumps: [NavJump], stills: [CellRef]) {
        self.titles = titles
        self.jumps = jumps
        self.stills = stills
    }
}
