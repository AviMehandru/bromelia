/// How a caller suspended by `EngineExecutor.suspend` is woken: once, from the executor. Resuming it counts in the
/// executor's activity until the caller runs again.
public struct Resumption<T: Sendable>: Sendable {
    let continuation: CheckedContinuation<T, Never>
    let counted: @Sendable () -> Void

    public func resume(returning value: T) {
        counted()
        continuation.resume(returning: value)
    }
}
