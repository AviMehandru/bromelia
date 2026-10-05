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
        _ = await feed(try launcher.start(spec), cancel: cancel) { e in
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

    /// disc:N only (backup.needsDrive otherwise). Stops at once when a DRV line shows the index now names another
    /// device.
    public func backup(_ source: MakemkvSource, decrypt: Bool, destination: String, invocation: MakemkvInvocation, sink: any RunSink,
                       cancel: CancellationToken) async throws(BroError) -> MakemkvRun {
        _ = try MakemkvArgs.backup(source, decrypt: decrypt, destination: destination, options: invocation.options)
        guard case let .drive(index, device) = source else { throw BroMessage(.backupNeedsDrive, severity: .error).toError() }
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
        let exit = await feed(try launcher.start(spec), cancel: cancel) { e in
            if first {
                first = false
                lease.firstOutput()
            }
            accumulator.feed(e)
            also?(e)
            sink.event(e)
            return accumulator.stopReason == nil ? nil : .cancelled
        }
        let newNames = try destination.map { (d: String) throws(BroError) in try self.newNames(d, before: before ?? []) } ?? []
        return MakemkvRun(outcome: RunOutcome.classify(accumulator, exit: exit, product: product, newNames: newNames),
                          notice: accumulator.problem, libreDrive: accumulator.libreDrive, version: accumulator.makemkvVersion)
    }

    /// Reads the process line by line; `handle` returns a reason to stop it (acted on once). Cancelling stops it too.
    private func feed(_ process: any RunningProcess, cancel: CancellationToken,
                      _ handle: (RobotEvent) -> StopReason?) async -> ProcessExit {
        let removeHandler = cancel.onCancel { process.stop(.cancelled) }
        defer { removeHandler() }
        var stopped = false
        for await line in process.lines() {
            let e = Robot.parseLine(line.text) ?? .raw(text: line.text)
            if let reason = handle(e), !stopped {
                stopped = true
                process.stop(reason)
            }
        }
        return await process.wait()
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
    /// (a backup to an .iso image).
    private func newNames(_ destination: String, before: Set<String>) throws(BroError) -> [String] {
        guard fs.exists(destination) else { return [] }
        if try !fs.stat(destination).isDirectory { return [destination.split(separator: "/").last.map(String.init) ?? destination] }
        return try fs.list(destination).map(\.name).filter { !before.contains($0) }
    }
}
