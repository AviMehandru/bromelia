import BroDomain
import BroFoundation

/// A file read as a stream (hashing).
public protocol ByteStream: Sendable {
    /// Up to maxBytes; empty at the end.
    func read(_ maxBytes: Int) throws(BroError) -> [UInt8]

    /// Closes it.
    func close()
}
