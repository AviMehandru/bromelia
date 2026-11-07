import BroFoundation

/// How a step keeps room on a volume for its job (plan §21), implemented by the runner over the Scheduler's
/// reservations : two rips on one volume count each other's space. What a job keeps goes back when it finishes.
public protocol SpaceReservations: Sendable {
    /// Keeps `need` bytes on `volume` for the job, replacing what it kept there. space.insufficient {path, need, free} when
    /// `free` minus what other jobs keep there is less.
    func reserve(_ volume: String, path: String, need: Int64, free: Int64) async throws(BroError)
}
