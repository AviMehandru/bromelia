import CryptoKit

/// Disc fingerprints, to recognise a disc when it is inserted again.
public enum Fingerprint {
    /// v1, byte for byte as today: `v1:` and 32 hex digits of SHA-256 over the volume name (else the name), the
    /// title count and, sorted, each title's source title id, length in seconds, segment map and size in bytes.
    /// Nil for a listing without titles.
    public static func of(_ listing: Listing) -> String? {
        if listing.titles.isEmpty { return nil }
        let volume = listing.volumeName.isEmpty ? listing.name : listing.volumeName
        let lines = listing.titles.map { "\($0.sourceTitleId ?? -1)|\($0.durationSeconds)|\($0.segmentMap)|\($0.sizeBytes)" }
            .sorted { Array($0.utf8).lexicographicallyPrecedes(Array($1.utf8)) }
        var text = "bromelia-disc-fingerprint 1\nvolume:\(volume)\ntitles:\(listing.titles.count)\n"
        for l in lines { text += l + "\n" }
        let digest = SHA256.hash(data: Array(text.utf8))
        return "v1:" + digest.prefix(16).map { b in
            let h = String(b, radix: 16)
            return h.count == 1 ? "0" + h : h
        }.joined()
    }
}
