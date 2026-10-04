using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>Audio CDs (plan §10.1; shared/fixtures/adapters/cd-ripper.cases.json): the profile's audio CD command, else
/// cyanrip, else abcde (not on Windows: it is a shell script), run in the destination; both look the album up in
/// MusicBrainz and name the files themselves.</summary>
public sealed class CdRipper
{
    readonly IProcessLauncher _launcher;
    readonly IToolLocator _locator;
    readonly IFileSystem _fs;

    public CdRipper(IProcessLauncher launcher, IToolLocator locator, IFileSystem fs)
    {
        _launcher = launcher;
        _locator = locator;
        _fs = fs;
    }

    /// <summary>Rips the CD in <paramref name="device"/> into <paramref name="dest"/> (which must exist) and returns the
    /// visible items saved there, sorted. <paramref name="command"/>: the profile's audio CD command ({device} is the
    /// drive), empty for none; <paramref name="stallMinutes"/>: 0 for no stall timeout.</summary>
    public async Task<IReadOnlyList<string>> Rip(string device, string dest, string? command, int stallMinutes, IRunSink sink, CancellationToken cancel)
    {
        var located = new Dictionary<string, string>();
        foreach (var tool in new[] { ToolKind.Cyanrip, ToolKind.Abcde })
        {
            if (tool == ToolKind.Abcde && OperatingSystem.IsWindows()) continue;
            if (_locator.Locate(tool).Path is { } path) located[EnumWire.Name(tool)] = path;
        }
        var otherDiscs = new JsonValue.Object(new List<KeyValuePair<string, JsonValue>> { new("audioCommand", JsonValue.Of(command ?? "")) });
        if (CdRipperArgs.Build(otherDiscs, device, located.Keys.ToList()) is not { } line)
            throw new BroFailure(new BroMessage(MessageCode.OtherAudioNeedsRipper, Severity.Error, ("platform", JsonValue.Of(Platform()))).ToError());
        var name = line.Executable;
        var exe = located.TryGetValue(name, out var found) ? found
            : name is "cyanrip" or "abcde" && _locator.Locate(EnumWire.Parse<ToolKind>(name)!.Value).Path is { } p ? p
            : name;
        if (cancel.IsCancelled) throw Cancelled();
        var process = _launcher.Start(new ProcessSpec(exe, line.Arguments, new Dictionary<string, string>(), dest, StopPolicy.InterruptFirst,
            stallMinutes > 0 ? new Duration(stallMinutes * 60) : null));
        using var onCancel = cancel.OnCancel(() => process.Stop(StopReason.Cancelled));
        await foreach (var output in process.Lines().ConfigureAwait(false)) sink.Event(new RobotEvent.Raw(output.Text));
        var exit = await process.Wait().ConfigureAwait(false);
        if (exit.Cancelled) throw Cancelled();
        if (exit.Stalled is not null) throw Failed(MessageCode.ProcessStalled, name, ("minutes", JsonValue.Of(stallMinutes)));
        if (exit.Status != 0) throw Failed(MessageCode.ProcessFailed, name, ("status", JsonValue.Of(exit.Status)));
        var saved = _fs.List(dest).Select(e => e.Name).Where(n => !n.StartsWith('.')).OrderBy(n => n, StringComparer.Ordinal)
            .Select(n => System.IO.Path.Combine(dest, n)).ToList();
        if (saved.Count == 0) throw Failed(MessageCode.ProcessSavedNothing, name);
        return saved;
    }

    static string Platform() => OperatingSystem.IsWindows() ? "windows" : OperatingSystem.IsMacOS() ? "macos" : "linux";

    static BroFailure Failed(MessageCode code, string tool, params (string Key, JsonValue Value)[] more) =>
        new(new BroMessage(code, Severity.Error, new[] { ("tool", JsonValue.Of(tool)) }.Concat(more).ToArray()).ToError());

    static BroFailure Cancelled() => new(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
}
