import os

/// What cancels a CancellationToken: the job (or whoever started the work) keeps the source and hands out `token`, so
/// what receives the token can't cancel the caller's work. A linked source is cancelled with its parent too, until it
/// is closed (or freed): closing removes its handler from the parent, so a parent that lives as long as the daemon
/// doesn't keep one per job.
public final class CancellationSource: Sendable {
    public let token = CancellationToken()
    private let unlink = OSAllocatedUnfairLock<(@Sendable () -> Void)?>(initialState: nil)

    public init() {}

    /// A source that is cancelled when `parent` is, or when it is cancelled itself.
    public static func linked(to parent: CancellationToken) -> CancellationSource {
        let source = CancellationSource()
        let token = source.token
        let remove = parent.onCancel { token.fire() }
        source.unlink.withLock { $0 = remove }
        return source
    }

    deinit { close() }

    /// Cancels the token and runs its handlers once.
    public func cancel() { token.fire() }

    /// No longer follows the parent (a linked source); the token keeps its state.
    public func close() {
        let remove = unlink.withLock { u -> (@Sendable () -> Void)? in
            defer { u = nil }
            return u
        }
        remove?()
    }
}
