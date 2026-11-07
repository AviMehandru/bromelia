import BroDomain
import BroFoundation
import Foundation

/// The owner of all service state (plan §8): actions posted from any thread run one at a time on `EngineActor`, in the
/// order they were posted, never inside `post`. Blocking work goes to `worker`, whose result comes back on the actor.
public final class EngineExecutor: Sendable {
    public typealias Action = @EngineActor @Sendable () -> Void

    /// What `activity` counts.
    private final class Counts: @unchecked Sendable {
        let lock = NSLock()
        var queued = 0
        var workers = 0
        var resuming = 0
    }

    private let mailbox: AsyncStream<Action>.Continuation
    private let pump: Task<Void, Never>
    private let counts = Counts()

    public init() {
        let (stream, mailbox) = AsyncStream<Action>.makeStream()
        self.mailbox = mailbox
        let counts = counts
        pump = Task { @EngineActor in
            for await action in stream {
                counts.lock.withLock { counts.queued -= 1 }
                action()
            }
        }
    }

    /// Runs `action` on the executor after everything posted before it. False, and the action never runs, once the
    /// executor is stopping.
    @discardableResult
    public func post(_ action: @escaping Action) -> Bool {
        counts.lock.withLock { counts.queued += 1 }
        if case .terminated = mailbox.yield(action) {
            counts.lock.withLock { counts.queued -= 1 }
            return false
        }
        return true
    }

    /// Work not done yet: actions waiting to run (not the one running), workers whose caller hasn't resumed, and
    /// `suspend`s resumed whose caller hasn't run yet. A test (or a scenario runner) knows the engine has settled when
    /// this is down to the workers it knows are parked (a step waiting for an answer).
    public var activity: Int { counts.lock.withLock { counts.queued + counts.workers + counts.resuming } }

    /// Suspends the caller until `start`'s resumption is resumed. Swift runs the caller again on the actor, not
    /// through the mailbox, so `activity` counts it from the resume until the caller runs (C# and C resume through
    /// the executor's queue, which counts it already). The Scheduler waits for leases this way.
    @EngineActor
    public func suspend<T: Sendable>(_ start: (Resumption<T>) -> Void) async -> T {
        let counts = counts
        let value = await withCheckedContinuation { (c: CheckedContinuation<T, Never>) in
            start(Resumption(continuation: c, counted: { counts.lock.withLock { counts.resuming += 1 } }))
        }
        counts.lock.withLock { counts.resuming -= 1 }
        return value
    }

    /// Starts `work` as a task of its own on the actor and returns at once (a service checking several things side by
    /// side). Swift starts the task on the actor, not through the mailbox, so `activity` counts it until it starts;
    /// from there its `worker` calls count. C# and C start their worker at once, which counts it already.
    @EngineActor
    public func spawn(_ work: @escaping @EngineActor @Sendable () async -> Void) {
        let counts = counts
        counts.lock.withLock { counts.resuming += 1 }
        Task { @EngineActor in
            counts.lock.withLock { counts.resuming -= 1 }
            await work()
        }
    }

    /// Runs `work` on a worker (off the executor); its result or its error comes back on the executor, through the
    /// mailbox: after everything the work posted before it returned (a step's last progress, its log lines).
    @EngineActor
    public func worker<T: Sendable>(_ work: @escaping @Sendable () async throws(BroError) -> T) async throws(BroError) -> T {
        counts.lock.withLock { counts.workers += 1 }
        let result = await withCheckedContinuation { (done: CheckedContinuation<Result<T, BroError>, Never>) in
            Task.detached { [self] in
                let result: Result<T, BroError>
                do throws(BroError) {
                    result = .success(try await work())
                } catch {
                    result = .failure(error)
                }
                if !post({ done.resume(returning: result) }) { done.resume(returning: result) }
            }
        }
        counts.lock.withLock { counts.workers -= 1 }
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
