using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>HandBrakeCLI (plan §10.1; shared/fixtures/adapters/handbrake.cases.json): one encode of a ripped MKV with
/// the step's preset, next to the archive (never over it: the step picks the output), and the presets to offer. The
/// step's own executable is used when it sets one; otherwise the locator's.</summary>
public sealed class HandBrake
{
    /// <summary>Presets built into HandBrake 1.6 and later (any preset name works): offered when HandBrakeCLI can't
    /// list its own.</summary>
    public static readonly IReadOnlyList<string> BuiltInPresets = new[] { "H.265 MKV 1080p30", "H.265 MKV 2160p60 4K", "H.264 MKV 1080p30",
        "H.264 MKV 480p30", "Fast 1080p30", "HQ 1080p30 Surround", "Super HQ 1080p30 Surround", "Fast 2160p60 4K HEVC" };

    const int Max = 10000;
    static readonly Regex Progress = new(@"Encoding: task (\d+) of (\d+), (\d+)\.(\d\d)", RegexOptions.CultureInvariant);

    readonly IProcessLauncher _launcher;
    readonly IToolLocator _locator;
    readonly IClock _clock;
    readonly string _home;

    /// <param name="home">The home folder a leading ~ stands for (the step's executable and preset file).</param>
    public HandBrake(IProcessLauncher launcher, IToolLocator locator, IClock clock, string home)
    {
        _launcher = launcher;
        _locator = locator;
        _clock = clock;
        _home = home;
    }

    /// <summary>HandBrakeCLI --preset-list's names, in its order; <see cref="BuiltInPresets"/> when HandBrakeCLI is
    /// missing or fails.</summary>
    public async Task<IReadOnlyList<string>> Presets(CancellationToken cancel)
    {
        if (_locator.Locate(ToolKind.Handbrake).Path is not { } exe) return BuiltInPresets;
        if (cancel.IsCancelled) throw Cancelled();
        var names = new List<string>();
        var inCategory = false;
        var exit = await ToolRun.Run(_launcher, new ProcessSpec(exe, new[] { "--preset-list" }, new Dictionary<string, string>(), null,
            StopPolicy.InterruptFirst, new Duration(60)), cancel, line =>
        {
            var text = line.Text;
            if (text[0] != ' ') inCategory = text.EndsWith('/'); // a category, or anything else at column 0
            else if (inCategory && text.Length > 4 && text.StartsWith("    ", StringComparison.Ordinal) && text[4] != ' ')
                names.Add(text.Substring(4).TrimEnd());
            return null;
        }).ConfigureAwait(false);
        if (exit.Cancelled) throw Cancelled();
        return exit.Status == 0 && names.Count > 0 ? names : BuiltInPresets;
    }

    /// <summary>Encodes <paramref name="input"/> to <paramref name="output"/> (whose folder must exist) in that folder.
    /// Progress goes to the sink as ProgressValue (of 10000), the lines HandBrakeArgs.KeepLine keeps as Raw.</summary>
    public async Task<HandBrakeRun> Encode(StepDefinition step, string input, string output, IRunSink sink, CancellationToken cancel)
    {
        var exe = Executable(step);
        if (cancel.IsCancelled) throw Cancelled();
        var timedOut = 0;
        ITimerHandle? timer = null;
        void Started(IRunningProcess process)
        {
            if (step.TimeoutSeconds <= 0) return;
            var at = new Instant(_clock.Now().UnixMilliseconds + step.TimeoutSeconds * 1000L);
            timer = _clock.Timer(new TimerSchedule.At(at), () =>
            {
                Interlocked.Exchange(ref timedOut, 1);
                process.Stop(StopReason.TimedOut);
            });
        }
        try
        {
            var filter = new ProgressFilter(0, 0);
            var exit = await ToolRun.Run(_launcher, new ProcessSpec(exe, HandBrakeArgs.Build(step, input, output, _home), new Dictionary<string, string>(),
                Path.GetDirectoryName(output), StopPolicy.InterruptFirst), cancel, line =>
            {
                if (ProgressOf(line.Text) is { } p) sink.Event(new RobotEvent.ProgressValue(p.Current, p.Total, Max));
                if (HandBrakeArgs.KeepLine(ref filter, line.Text)) sink.Event(new RobotEvent.Raw(line.Text));
                return null;
            }, Started).ConfigureAwait(false);
            if (exit.Cancelled) throw Cancelled();
            return new HandBrakeRun(exit, Volatile.Read(ref timedOut) == 1);
        }
        finally
        {
            timer?.Cancel();
        }
    }

    /// <summary>The step's executable (it must exist), or the one the locator finds.</summary>
    string Executable(StepDefinition step)
    {
        var own = (step.Handbrake ?? new HandBrakeSettings()).Executable.Trim();
        if (own.Length > 0)
        {
            if (own[0] == '~' && (own.Length == 1 || own[1] is '/' or '\\')) own = _home.TrimEnd('/', '\\') + own.Substring(1);
            if (File.Exists(own)) return own;
            throw new BroFailure(new BroMessage(MessageCode.ToolNotFoundAt, Severity.Error, ("tool", JsonValue.Of(EnumWire.Name(ToolKind.Handbrake))),
                ("path", JsonValue.Of(own))).ToError());
        }
        var info = _locator.Locate(ToolKind.Handbrake);
        return info.Path ?? throw new BroFailure((info.Why ?? new BroMessage(MessageCode.ToolMissing, Severity.Warning,
            ("tool", JsonValue.Of(EnumWire.Name(ToolKind.Handbrake))))).ToError());
    }

    /// <summary>'Encoding: task t of n, p %' as (this task's share, all tasks' share) of 10000.</summary>
    static (int Current, int Total)? ProgressOf(string line)
    {
        var m = Progress.Match(line);
        if (!m.Success) return null;
        int task = int.Parse(m.Groups[1].Value), tasks = Math.Max(1, int.Parse(m.Groups[2].Value));
        int current = Math.Min(Max, int.Parse(m.Groups[3].Value) * 100 + int.Parse(m.Groups[4].Value));
        int total = (int)(((long)Math.Clamp(task - 1, 0, tasks - 1) * Max + current) / tasks);
        return (current, total);
    }

    static BroFailure Cancelled() => new(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
}
