using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>makemkvcon (plan §10.1). Every call prepares the run's settings (SettingsIsolation), builds the arguments
/// (MakemkvArgs), starts it (ProcessLauncher; TERM first, since makemkvcon ignores INT), feeds every line to
/// Robot.parseLine, the RunAccumulator and the sink, gives the settings back on the first line, stops it when the
/// accumulator says so (MakeMKV's space warning, a renumbered drive) or when cancelled, and classifies the run with
/// the files it produced. Holds no state between calls.</summary>
public sealed class MakemkvTool
{
    readonly IProcessLauncher _launcher;
    readonly IFileSystem _fs;
    readonly ISettingsIsolation _isolation;
    readonly IToolLocator _locator;

    public MakemkvTool(IProcessLauncher launcher, IFileSystem fs, ISettingsIsolation isolation, IToolLocator locator)
    {
        _launcher = launcher;
        _fs = fs;
        _isolation = isolation;
        _locator = locator;
    }

    /// <summary><c>-r --cache=1 info disc:9999</c>, without settings isolation: every DRV line.</summary>
    public async Task<IReadOnlyList<MakemkvDrive>> ScanDrives(CancellationToken cancel)
    {
        var spec = new ProcessSpec(Executable(), MakemkvArgs.ScanDrives(), new Dictionary<string, string>(), null, StopPolicy.TerminateFirst);
        var drives = new List<MakemkvDrive>();
        await Run(spec, cancel, e =>
        {
            if (MakemkvDrive.From(e) is { } d) drives.Add(d);
            return null;
        }).ConfigureAwait(false);
        return drives;
    }

    public async Task<ListingRun> Listing(MakemkvSource source, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel)
    {
        var builder = new ListingBuilder();
        var run = await Isolated(invocation, options => MakemkvArgs.Info(source, options), new RunAccumulator(), null, sink, cancel, builder.Feed).ConfigureAwait(false);
        return new ListingRun(builder.Build(), run);
    }

    /// <param name="title">A title index or "all".</param>
    public Task<MakemkvRun> Rip(MakemkvSource source, string title, string destination, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel) =>
        Isolated(invocation, options => MakemkvArgs.Mkv(source, title, destination, options), new RunAccumulator(readsData: true), destination, sink, cancel, null);

    /// <summary>disc:N only (backup.needsDrive otherwise). Stops at once when a DRV line shows the index now names
    /// another device.</summary>
    public Task<MakemkvRun> Backup(MakemkvSource source, bool decrypt, string destination, MakemkvInvocation invocation, IRunSink sink, CancellationToken cancel)
    {
        MakemkvArgs.Backup(source, decrypt, destination, invocation.Options); // backup.needsDrive before anything runs
        var drive = (MakemkvSource.Drive)source;
        return Isolated(invocation, options => MakemkvArgs.Backup(source, decrypt, destination, options),
            new RunAccumulator(readsData: true, expectedIndex: drive.Index, expectedDevice: drive.Device), destination, sink, cancel, null);
    }

    string Executable()
    {
        var info = _locator.Locate(ToolKind.Makemkvcon);
        if (info.Path is { } path) return path;
        throw new BroFailure((info.Why ?? new BroMessage(MessageCode.ToolMissing, Severity.Error, ("tool", JsonValue.Of("makemkvcon")))).ToError());
    }

    async Task<MakemkvRun> Isolated(MakemkvInvocation invocation, Func<MakemkvOptions, List<string>> arguments, RunAccumulator accumulator,
        string? destination, IRunSink sink, CancellationToken cancel, Action<RobotEvent>? also)
    {
        if (cancel.IsCancelled) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
        var exe = Executable();
        var before = destination is null ? null : Names(destination);
        var lease = _isolation.Prepare(invocation.Settings);
        try
        {
            var options = invocation.Options with { ProfilePath = lease.ProfilePath() ?? invocation.Options.ProfilePath };
            var spec = new ProcessSpec(exe, arguments(options), lease.Environment(), invocation.Settings.WorkDirectory, StopPolicy.TerminateFirst,
                invocation.StallTimeout, invocation.Transcript);
            var first = true;
            var exit = await Run(spec, cancel, e =>
            {
                if (first)
                {
                    first = false;
                    lease.FirstOutput();
                }
                accumulator.Feed(e);
                also?.Invoke(e);
                sink.Event(e);
                return accumulator.StopReason is null ? null : StopReason.Cancelled;
            }).ConfigureAwait(false);
            int produced = destination is null ? 1 : Names(destination).Count(n => before is null || !before.Contains(n));
            var outcome = RunOutcome.Classify(accumulator, exit, produced);
            return new MakemkvRun(outcome, accumulator.Problem, accumulator.LibreDrive, accumulator.MakemkvVersion);
        }
        finally
        {
            lease.Release();
        }
    }

    /// <summary>Starts the process and feeds it line by line; <paramref name="onEvent"/> returns a reason to stop it
    /// (once). Cancelling the token stops it too.</summary>
    async Task<ProcessExit> Run(ProcessSpec spec, CancellationToken cancel, Func<RobotEvent, StopReason?> onEvent)
    {
        var process = _launcher.Start(spec);
        using var onCancel = cancel.OnCancel(() => process.Stop(StopReason.Cancelled));
        var stopped = false;
        await foreach (var line in process.Lines().ConfigureAwait(false))
        {
            var e = Robot.ParseLine(line.Text) ?? new RobotEvent.Raw(line.Text);
            if (onEvent(e) is { } reason && !stopped)
            {
                stopped = true;
                process.Stop(reason);
            }
        }
        return await process.Wait().ConfigureAwait(false);
    }

    HashSet<string> Names(string folder)
    {
        try { return _fs.List(folder).Select(e => e.Name).ToHashSet(StringComparer.Ordinal); }
        catch (BroFailure) { return new HashSet<string>(StringComparer.Ordinal); }
    }
}
