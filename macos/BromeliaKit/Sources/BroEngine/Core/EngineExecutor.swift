import BroDomain
import BroFoundation

/// The owner of all service state (plan §8): actions posted from any thread run one at a time on `EngineActor`, in
/// the order they were posted, never inside `post`. Blocking work goes to `worker`, whose result comes back on the
/// actor. Today's AppModel ran this on the main actor with `DispatchQueue.main.async`.
public final class EngineExecutor: Sendable {
    public typealias Action = @EngineActor @Sendable () -> Void

    private let mailbox: AsyncStream<Action>.Continuation
    private let pump: Task<Void, Never>

    public init() {
        let (stream, mailbox) = AsyncStream<Action>.makeStream()
        self.mailbox = mailbox
        pump = Task { @EngineActor in
            for await action in stream { action() }
        }
    }

    /// Runs `action` on the executor after everything posted before it. False, and the action never runs, once the
    /// executor is stopping.
    @discardableResult
    public func post(_ action: @escaping Action) -> Bool {
        if case .terminated = mailbox.yield(action) { return false }
        return true
    }

    /// Runs `work` on a worker (off the executor); its result or its error comes back on the executor.
    @EngineActor
    public func worker<T: Sendable>(_ work: @escaping @Sendable () throws(BroError) -> T) async throws(BroError) -> T {
        let result = await Task.detached { () -> Result<T, BroError> in
            do throws(BroError) {
                return .success(try work())
            } catch {
                return .failure(error)
            }
        }.value
        return try result.get()
    }

    /// Returns once every action posted before it has run (at once when stopped).
    public func barrier() async {
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
            if !post({ done.resume() }) { done.resume() }
        }
    }

    /// Lets the queued actions run, then ends; later posts don't run. Returns once the last one has run.
    public func stop() async {
        mailbox.finish()
        await pump.value
    }
}
