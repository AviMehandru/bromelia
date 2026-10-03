import os

/// Cancellation that crosses layers (plan §6). Thread-safe; handlers run once, on the thread that cancels (or
/// straight away when registered after cancellation).
public final class CancellationToken: Sendable {
    private struct State {
        var cancelled = false
        var handlers: [Int: @Sendable () -> Void] = [:]
        var next = 0
    }
    private let state = OSAllocatedUnfairLock(initialState: State())

    public init() {}

    /// A token that is cancelled when `parent` is, or when it is cancelled itself.
    public static func child(of parent: CancellationToken) -> CancellationToken {
        let child = CancellationToken()
        _ = parent.onCancel { [weak child] in child?.cancel() }
        return child
    }

    public var isCancelled: Bool { state.withLock { $0.cancelled } }

    public func cancel() {
        let handlers = state.withLock { s -> [@Sendable () -> Void] in
            if s.cancelled { return [] }
            s.cancelled = true
            defer { s.handlers = [:] }
            return s.handlers.sorted { $0.key < $1.key }.map(\.value)
        }
        handlers.forEach { $0() }
    }

    /// Runs `handler` once on cancellation. Calling the returned function removes the handler.
    @discardableResult
    public func onCancel(_ handler: @escaping @Sendable () -> Void) -> @Sendable () -> Void {
        let id: Int? = state.withLock { s in
            if s.cancelled { return nil }
            s.next += 1
            s.handlers[s.next] = handler
            return s.next
        }
        guard let id else { handler(); return {} }
        return { [weak self] in _ = self?.state.withLock { $0.handlers.removeValue(forKey: id) } }
    }
}
