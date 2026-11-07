import BroDomain
import BroFoundation

/// Room for a plan on its library's volume (plan §21).
public enum SpaceCheck {
    /// Logs space.report {free, need}, then keeps `need` plus the margin (SpaceEstimate.required) on the volume of
    /// `libraryRoot`: space.insufficient fails the job before anything is written.
    public static func reserve(_ context: StepContext, _ libraryRoot: String, _ need: Int64) async throws(BroError) {
        let volume = try context.fs.volume(libraryRoot)
        context.log.log(BroMessage(.spaceReport, [("free", .integer(volume.freeBytes)), ("need", .integer(need))]))
        try await context.space.reserve(volume.id, path: libraryRoot, need: SpaceEstimate.required(Bytes(need)).count, free: volume.freeBytes)
    }
}
