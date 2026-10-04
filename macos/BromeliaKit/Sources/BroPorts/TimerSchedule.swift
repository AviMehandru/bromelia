import BroDomain
import BroFoundation

/// When a timer fires: once at an instant, or every interval.
public enum TimerSchedule: Sendable, Equatable {
    case at(instant: Instant)
    case every(interval: Duration)
}
