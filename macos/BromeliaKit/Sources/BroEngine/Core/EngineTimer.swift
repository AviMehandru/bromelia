import BroPorts

/// A timer of `EngineClock`.
@EngineActor
public final class EngineTimer {
    private let handler: @EngineActor () -> Void
    private let once: Bool
    private var ended = false
    var handle: (any TimerHandle)?

    init(handler: @escaping @EngineActor () -> Void, once: Bool) {
        self.handler = handler
        self.once = once
    }

    /// Stops the timer: a handler the clock already posted doesn't run either. Does nothing the second time, or after
    /// a one-off timer fired.
    public func cancel() {
        if ended { return }
        ended = true
        handle?.cancel()
    }

    /// Posted by the clock's handler.
    func fire() {
        if ended { return }
        if once { ended = true }
        handler()
    }
}
