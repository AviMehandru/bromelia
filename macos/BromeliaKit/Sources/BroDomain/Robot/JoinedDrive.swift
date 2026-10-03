/// A drive as the engine knows it: its id, what MakeMKV reported and what the OS reported (either may be
/// missing).
public struct JoinedDrive: Sendable, Equatable {
    public var driveId: String
    public var makemkv: MakemkvDrive?
    public var os: OsDrive?

    public init(driveId: String, makemkv: MakemkvDrive?, os: OsDrive?) {
        self.driveId = driveId
        self.makemkv = makemkv
        self.os = os
    }
}
