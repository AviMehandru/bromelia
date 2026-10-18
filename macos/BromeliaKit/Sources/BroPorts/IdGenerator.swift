import BroFoundation

/// New ids for what the engine creates (jobs first): randomness is I/O, so it comes through a port (owner, 2026-10-17).
/// The adapter gives random UUIDs; the test kit counts.
public protocol IdGenerator: Sendable {
    /// An id no other call has given: a lower-case UUID.
    func next() -> Id
}
