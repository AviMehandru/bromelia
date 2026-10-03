/// A checked archive folder and its result.
public struct FolderCheck: Sendable, Equatable {
    public var folder: String
    public var result: VerifyResult

    public init(folder: String, result: VerifyResult) {
        self.folder = folder
        self.result = result
    }
}
