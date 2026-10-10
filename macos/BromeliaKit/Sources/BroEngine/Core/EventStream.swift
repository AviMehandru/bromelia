import Foundation

/// One reader's bounded queue of client-visible events (the server's SSE connection). The bus fills it on the
/// executor; the reader takes from it on any thread. When an event doesn't fit, everything queued becomes one resync
/// item, and while a resync waits, new events are dropped: the snapshot the reader sends next covers them.
public final class EventStream: @unchecked Sendable {
    private let lock = NSLock()
    private var items: [StreamItem] = []
    private var head = 0
    private let capacity: Int
    private let types: Set<EngineEventType>
    private var resyncWaiting = false
    private var closed = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    init(capacity: Int, types: [EngineEventType]) {
        self.capacity = capacity
        self.types = Set(types)
    }

    func wants(_ type: EngineEventType) -> Bool { !type.isInternal && (types.isEmpty || types.contains(type)) }

    private var queued: Int { items.count - head }

    func offer(_ event: EngineEvent) {
        let wake: [CheckedContinuation<Void, Never>] = lock.withLock {
            if closed || resyncWaiting || !wants(event.type) { return [] }
            if queued >= capacity { return resyncLocked() }
            items.append(.event(event))
            return takeWaitersLocked()
        }
        wake.forEach { $0.resume() }
    }

    func resync() {
        let wake = lock.withLock { closed ? [] : resyncLocked() }
        wake.forEach { $0.resume() }
    }

    private func resyncLocked() -> [CheckedContinuation<Void, Never>] {
        items = [.resync]
        head = 0
        resyncWaiting = true
        return takeWaitersLocked()
    }

    private func takeWaitersLocked() -> [CheckedContinuation<Void, Never>] {
        defer { waiters = [] }
        return waiters
    }

    private func takeLocked() -> StreamItem? {
        guard head < items.count else { return nil }
        let item = items[head]
        head += 1
        if head == items.count {
            items = []
            head = 0
        }
        if case .resync = item { resyncWaiting = false }
        return item
    }

    /// The next item if one is queued; never waits.
    public func poll() -> StreamItem? { lock.withLock { takeLocked() } }

    /// Waits for the next item; nil once the stream is closed.
    public func next() async -> StreamItem? {
        while true {
            let ready: StreamItem?? = lock.withLock {
                if let item = takeLocked() { return .some(item) }
                return closed ? .some(nil) : nil
            }
            if let ready { return ready }
            await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
                let now = lock.withLock { () -> Bool in
                    if queued > 0 || closed { return true }
                    waiters.append(c)
                    return false
                }
                if now { c.resume() }
            }
        }
    }

    /// Ends the stream, from any thread: what was queued is dropped, the bus forgets it, next returns nil.
    public func close() {
        let wake = lock.withLock { () -> [CheckedContinuation<Void, Never>] in
            closed = true
            items = []
            head = 0
            return takeWaitersLocked()
        }
        wake.forEach { $0.resume() }
    }

    public var isClosed: Bool { lock.withLock { closed } }
}
