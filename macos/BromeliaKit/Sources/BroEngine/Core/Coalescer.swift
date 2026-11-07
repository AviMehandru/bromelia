import BroFoundation
import BroPorts

/// At most one value per interval per key (plan §8 rule 5): a job's progress before it becomes an event or is stored. A
/// value goes out at once when nothing for its key went out in the last interval; otherwise it waits (replacing the one
/// waiting) until one interval after the last one went out. Robot progress goes through it (a value at most every 0.1
/// s); shared/fixtures/engine/coalescer.cases.json.
@EngineActor
public final class Coalescer {
    private final class Key {
        var lastSentMs: Int64?
        var waiting: JsonValue?
        var timer: EngineTimer?
    }

    private let clock: EngineClock
    private let intervalMs: Int64
    private let emit: @EngineActor (String, JsonValue) -> Void
    private var keys: [String: Key] = [:]

    public init(clock: EngineClock, interval: Duration, emit: @escaping @EngineActor (String, JsonValue) -> Void) {
        self.clock = clock
        intervalMs = Int64((interval.seconds * 1000).rounded())
        self.emit = emit
    }

    private func nowMs() -> Int64 { Int64((clock.monotonic().seconds * 1000).rounded()) }

    public func offer(_ key: String, value: JsonValue) {
        let k = keys[key] ?? Key()
        keys[key] = k
        let now = nowMs()
        if k.waiting == nil, k.lastSentMs.map({ now - $0 >= intervalMs }) ?? true {
            send(key, k, value, now)
            return
        }
        k.waiting = value
        if k.timer != nil { return }
        let due = (k.lastSentMs ?? now) + intervalMs
        let at = Instant(unixMilliseconds: clock.now().unixMilliseconds + max(0, due - now))
        k.timer = clock.timer(.at(instant: at)) { [weak self] in
            k.timer = nil
            guard let self, let w = k.waiting else { return }
            self.send(key, k, w, self.nowMs())
        }
    }

    /// Sends a waiting value at once.
    public func flush(_ key: String) {
        if let k = keys[key], let w = k.waiting { send(key, k, w, nowMs()) }
    }

    /// Throws a waiting value away and forgets when the key last went out.
    public func drop(_ key: String) {
        keys.removeValue(forKey: key)?.timer?.cancel()
    }

    private func send(_ key: String, _ k: Key, _ value: JsonValue, _ now: Int64) {
        k.timer?.cancel()
        k.timer = nil
        k.waiting = nil
        k.lastSentMs = now
        emit(key, value)
    }
}
