import BroDomain
import BroFoundation
import BroPorts

/// makemkvcon (plan §10.1). Every call prepares the run's settings (SettingsIsolation), builds the arguments
/// (MakemkvArgs), starts it (ProcessLauncher; TERM first, since makemkvcon ignores INT), feeds every line to
/// Robot.parseLine, the RunAccumulator and the sink, gives the settings back on the first line, stops it when the
/// accumulator says so (MakeMKV's space warning, a renumbered drive) or when cancelled, and classifies the run with
/// the names that are new in the destination (RunOutcome.products picks what counts). A destination that can't be
/// listed before or after the run fails the call: every name in it would otherwise look new, or none. Holds no state
/// between calls.
///
/// A backup's destination must not exist yet: makemkvcon refuses one that does, even an empty folder ("already contains
/// a backup", exit 0), so the call fails with fs.alreadyExists before anything runs. A backup that comes out as one file
/// at the destination is classified as `RunProduct.image`, whatever it is called: MakeMKV writes DVD backups as ISO
/// images even when a folder was asked for, and naming the image is the caller's job.
public final class MakemkvTool: Sendable {
    private let launcher: any ProcessLauncher
    private let fs: any FileSystem
    private let isolation: any SettingsIsolation
    private let locator: any ToolLocator

    public init(launcher: any ProcessLauncher, fs: any FileSystem, isolation: any SettingsIsolation, locator: any ToolLocator) {
        self.launcher = launcher
        self.fs = fs
        self.isolation = isolation
        self.locator = locator
    }

    /// `-r --cache=1 info disc:9999`, without settings isolation: every DRV line.
    public func scanDrives(_ cancel: CancellationToken) async throws(BroError) -> [MakemkvDrive] {
        let spec = ProcessSpec(executable: try executable(), arguments: MakemkvArgs.scanDrives(), environment: [:], stopPolicy: .terminateFirst)
        var drives: [MakemkvDrive] = []
        _ = try await feed(spec, cancel: cancel) { e in
            if let d = MakemkvDrive.from(e) { drives.append(d) }
            return nil
        }
        return drives
    }

    public func listing(_ source: MakemkvSource, invocation: MakemkvInvocation, sink: any RunSink,
                        cancel: CancellationToken) async throws(BroError) -> ListingRun {
        var builder = ListingBuilder()
        let run = try await isolated(invocation, accumulator: RunAccumulator(), product: .nothing, destination: nil, sink: sink, cancel: cancel,
                                     arguments: { MakemkvArgs.info(source, options: $0) }, also: { builder.feed($0) })
        return ListingRun(listing: builder.build(), run: run)
    }

    /// title: a title index or "all".
    public func rip(_ source: MakemkvSource, title: String, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
                    cancel: CancellationToken) async throws(BroError) -> MakemkvRun {
        try await isolated(invocation, accumulator: RunAccumulator(readsData: true), product: .titles, destination: destination, sink: sink, cancel: cancel,
                           arguments: { MakemkvArgs.mkv(source, title: title, destination: destination, options: $0) }, also: nil)
    }

    /// disc:N only (backup.needsDrive otherwise), to a destination that doesn't exist yet (fs.alreadyExists otherwise).
    /// Stops at once when a DRV line shows the index now names another device.
    public func backup(_ source: MakemkvSource, decrypt: Bool, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
                       cancel: CancellationToken) async throws(BroError) -> MakemkvRun {
        _ = try MakemkvArgs.backup(source, decrypt: decrypt, destination: destination, options: invocation.options)
        guard case let .drive(index, device) = source else { throw BroMessage(.backupNeedsDrive, severity: .error).toError() }
        if fs.exists(destination) { throw BroMessage(.fsAlreadyExists, [("path", .string(destination))], severity: .error).toError() }
        let args = { (options: MakemkvOptions) in (try? MakemkvArgs.backup(source, decrypt: decrypt, destination: destination, options: options)) ?? [] }
        return try await isolated(invocation, accumulator: RunAccumulator(readsData: true, expectedIndex: index, expectedDevice: device),
                                  product: .backup, destination: destination, sink: sink, cancel: cancel, arguments: args, also: nil)
    }

    private func executable() throws(BroError) -> String {
        let info = locator.locate(.makemkvcon)
        if let path = info.path { return path }
        throw (info.why ?? BroMessage(.toolMissing, [("tool", .string("makemkvcon"))], severity: .error)).toError()
    }

    private func isolated(_ invocation: MakemkvInvocation, accumulator start: RunAccumulator, product: RunProduct, destination: String?, sink: any RunSink,
                          cancel: CancellationToken, arguments: (MakemkvOptions) -> [String],
                          also: ((RobotEvent) -> Void)?) async throws(BroError) -> MakemkvRun {
        if cancel.isCancelled { throw BroError(MessageCode.jobCancelled.rawValue) }
        let exe = try executable()
        let before = try destination.map { (d: String) throws(BroError) in try namesBefore(d) }
        let lease = try isolation.prepare(invocation.settings)
        defer { lease.release() }
        var options = invocation.options
        options.profilePath = lease.profilePath() ?? options.profilePath
        let spec = ProcessSpec(executable: exe, arguments: arguments(options), environment: lease.environment(),
                               workingDirectory: invocation.settings.workDirectory, stopPolicy: .terminateFirst,
                               stallTimeout: invocation.stallTimeout, transcript: invocation.transcript)
        var accumulator = start
        var first = true
        final class Started: @unchecked Sendable { var process: (any RunningProcess)? }
        let started = Started()
        let exit = try await feed(spec, cancel: cancel, started: { started.process = $0 }) { e in
            if first {
                first = false
                lease.firstOutput()
            }
            accumulator.feed(e)
            also?(e)
            sink.event(e)
            return accumulator.stopReason == nil ? nil : .policy
        }
        let newNames = try destination.map { (d: String) throws(BroError) in try self.newNames(d, before: before ?? []) } ?? []
        var product = product
        if product == .backup, let d = destination, fs.exists(d), try !fs.stat(d).isDirectory { product = .image }
        return MakemkvRun(outcome: RunOutcome.classify(accumulator, exit: exit, product: product, newNames: newNames),
                          notice: accumulator.problem, libreDrive: accumulator.libreDrive, version: accumulator.makemkvVersion,
                          transcriptProblem: started.process?.transcriptProblem())
    }

    /// Feeds the process's lines as robot events; `handle` returns a reason to stop it.
    private func feed(_ spec: ProcessSpec, cancel: CancellationToken, started: ((any RunningProcess) -> Void)? = nil,
                      _ handle: (RobotEvent) -> StopReason?) async throws(BroError) -> ProcessExit {
        try await ToolRun.run(launcher, spec, cancel: cancel, started: started) { line in handle(Robot.parseLine(line.text) ?? .raw(text: line.text)) }
    }

    /// The names in the destination before the run: none when it isn't there yet.
    private func namesBefore(_ destination: String) throws(BroError) -> Set<String> {
        do throws(BroError) {
            return Set(try fs.list(destination).map(\.name))
        } catch where error.code == MessageCode.fsNotFound.rawValue {
            return []
        }
    }

    /// The names in the destination that weren't there before; the destination's own name when the run made it a file
    /// (a backup as an image).
    private func newNames(_ destination: String, before: Set<String>) throws(BroError) -> [String] {
        guard fs.exists(destination) else { return [] }
        if try !fs.stat(destination).isDirectory { return [destination.split(separator: "/").last.map(String.init) ?? destination] }
        return try fs.list(destination).map(\.name).filter { !before.contains($0) }
    }
}
