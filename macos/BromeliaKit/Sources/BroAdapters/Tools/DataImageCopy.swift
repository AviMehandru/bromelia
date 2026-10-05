import BroDomain

/// What a DataImager copy made: the bytes copied, and a warning when the image is complete but its folder couldn't be flushed
/// (fs.notSynced).
public struct DataImageCopy: Sendable, Equatable {
    public var bytes: Int64
    public var warning: BroMessage?

    public init(bytes: Int64, warning: BroMessage?) {
        self.bytes = bytes
        self.warning = warning
    }
}
