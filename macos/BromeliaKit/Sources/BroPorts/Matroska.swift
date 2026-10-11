import BroDomain
import BroFoundation

/// MKVToolNix (plan §10.1): what an MKV file holds, remuxing, splitting at chapters, chapter times.
public protocol Matroska: Sendable {
    /// Nil when mkvmerge can't read the file.
    func probe(_ file: String, cancel: CancellationToken) async throws(BroError) -> MkvProbe?

    func remux(_ arguments: [String], cancel: CancellationToken) async throws(BroError) -> Bool

    /// The parts in order; nil when it failed or made another number of parts.
    func split(_ chapters: [Int], input: String, cancel: CancellationToken) async throws(BroError) -> [String]?

    func chapterTimes(_ file: String, cancel: CancellationToken) async throws(BroError) -> [Duration]?
}
