import BroPorts

/// The tools a step may run, as ports (owner, 2026-10-11). More join with their steps in phase 5.
public struct ToolSet: Sendable {
    public var makemkv: any Makemkv
    public var matroska: any Matroska

    public init(makemkv: any Makemkv, matroska: any Matroska) {
        self.makemkv = makemkv
        self.matroska = matroska
    }
}
