/// The format code of file names: DVD, BR, 4K, HDDVD or DISC, with an `e` suffix for backups that weren't
/// decrypted (common.json's FormatCode).
public struct FormatCode: Sendable, Hashable, CustomStringConvertible {
    public var text: String

    public init(_ text: String) { self.text = text }

    public var description: String { text }
}
