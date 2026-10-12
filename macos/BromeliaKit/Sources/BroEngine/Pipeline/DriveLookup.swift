/// Where a drive is now (implemented by DriveService): MakeMKV numbers drives by scan order and the device can change,
/// so a step asks when it runs.
public protocol DriveLookup: Sendable {
    /// nil when no such drive is connected.
    func find(_ driveId: String) -> DriveLocation?
}
