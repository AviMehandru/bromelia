import BroDomain
import BroFoundation
import BroPorts

/// RootOptions.drives false: the operating system reports no drive.
struct NoDeviceMonitor: DeviceMonitor {
    func start(_ sink: @escaping @Sendable (DeviceEvent) -> Void) {}

    func stop() {}

    func currentDrives() -> [OsDrive] { [] }
}

/// RootOptions.drives false: every drive command is refused (drive.notFound).
struct NoDriveControl: DriveControl {
    func eject(_ device: String) async throws(BroError) { throw refused(device) }

    func closeTray(_ device: String) async throws(BroError) { throw refused(device) }

    func waitForMount(_ device: String, timeout: Duration, cancel: CancellationToken) async -> String? { nil }

    func probeContent(_ device: String) -> DiscContent { .unknown }

    func openRaw(_ device: String) throws(BroError) -> any SectorReader { throw refused(device) }

    private func refused(_ device: String) -> BroError {
        BroMessage(.driveNotFound, [("driveId", .string(device))], severity: .error).toError()
    }
}

/// RootOptions.drives false: MakeMKV as it is, but its drive scan finds none and never starts makemkvcon.
struct NoDriveScan: Makemkv {
    private let makemkv: any Makemkv

    init(_ makemkv: any Makemkv) { self.makemkv = makemkv }

    func scanDrives(_ cancel: CancellationToken) async throws(BroError) -> [MakemkvDrive] { [] }

    func listing(_ source: MakemkvSource, invocation: MakemkvInvocation, sink: any RunSink,
                 cancel: CancellationToken) async throws(BroError) -> ListingRun {
        try await makemkv.listing(source, invocation: invocation, sink: sink, cancel: cancel)
    }

    func rip(_ source: MakemkvSource, title: String, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
             cancel: CancellationToken) async throws(BroError) -> MakemkvRun {
        try await makemkv.rip(source, title: title, destination: destination, invocation: invocation, sink: sink, cancel: cancel)
    }

    func backup(_ source: MakemkvSource, decrypt: Bool, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
                cancel: CancellationToken) async throws(BroError) -> MakemkvRun {
        try await makemkv.backup(source, decrypt: decrypt, destination: destination, invocation: invocation, sink: sink, cancel: cancel)
    }
}
