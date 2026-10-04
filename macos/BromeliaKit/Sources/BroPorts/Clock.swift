import BroDomain
import BroFoundation

/// Time.
public protocol Clock: Sendable {
    func now() -> Instant

    /// Time since an arbitrary start; never goes back.
    func monotonic() -> Duration

    /// Fails with job.cancelled when cancelled first.
    func sleep(_ duration: Duration, cancel: CancellationToken) async throws(BroError)

    func timer(_ schedule: TimerSchedule, handler: @escaping @Sendable () -> Void) -> any TimerHandle
}
