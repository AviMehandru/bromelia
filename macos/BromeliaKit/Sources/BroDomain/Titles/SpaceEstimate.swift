import BroFoundation

/// How much space a rip needs (listing sizes are estimates).
public enum SpaceEstimate {
    /// `bytes` plus a margin: 2 % or 256 MiB, whichever is larger.
    public static func required(_ bytes: Bytes) -> Bytes { Bytes(bytes.count + max(256 << 20, bytes.count / 50)) }

    /// The chosen titles, plus a second copy of the titles with hand-picked tracks (remuxed) and of the "play all"
    /// title (split into episodes).
    public static func forPlan(_ titles: [Title], handPicked: Set<Int>, splitTitle: Int?) -> Bytes {
        var need = titles.reduce(Int64(0)) { $0 + $1.sizeBytes } + titles.filter { handPicked.contains($0.index) }.reduce(Int64(0)) { $0 + $1.sizeBytes }
        if let s = splitTitle, let playAll = titles.first(where: { $0.index == s }) { need += playAll.sizeBytes }
        return Bytes(need)
    }
}
