import CryptoKit

/// SHA-256 fed a piece at a time (a file read back from disk as a stream), lower-case hex at the end.
public final class Sha256Hasher {
    private var hash = SHA256()

    public init() {}

    public func update(_ bytes: [UInt8]) { hash.update(data: bytes) }

    /// The digest, lower-case hex; the hasher is spent.
    public func finish() -> String {
        hash.finalize().map { b in
            let h = String(b, radix: 16)
            return h.count == 1 ? "0" + h : h
        }.joined()
    }
}
