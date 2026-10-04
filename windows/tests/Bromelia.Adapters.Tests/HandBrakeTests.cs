using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Tests;

/// <summary>A clock whose timers fire as soon as they are set (when asked to), recording when they were for.</summary>
internal sealed class FiringClock : IClock
{
    public readonly Instant At = Instant.Parse("2026-10-04T12:00:00.000Z")!.Value;
    public readonly List<TimerSchedule> Timers = new();
    public bool Fire;
    public Instant Now() => At;
    public Duration Monotonic() => new(0);
    public Task Sleep(Duration duration, CancellationToken cancel) => Task.CompletedTask;

    public ITimerHandle Timer(TimerSchedule schedule, Action handler)
    {
        Timers.Add(schedule);
        if (Fire) handler();
        return new Handle();
    }

    sealed class Handle : ITimerHandle
    {
        public void Cancel() { }
    }
}

/// <summary>Keeps every event.</summary>
internal sealed class RecordingSink : IRunSink
{
    public readonly List<RobotEvent> Events = new();
    public void Event(RobotEvent @event) => Events.Add(@event);
}

/// <summary>shared/fixtures/adapters/handbrake.cases.json.</summary>
public sealed class HandBrakeTests : IDisposable
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-hb-" + Guid.NewGuid().ToString("N"));
    readonly string _home = Path.Combine(Path.GetTempPath(), "bromelia-home");

    public void Dispose()
    {
        try { Directory.Delete(_root, true); } catch (IOException) { }
    }

    string R(string s) => s.Replace("<root>", _root).Replace("<home>", _home).Replace("<handbrake>", "/opt/HandBrakeCLI");

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/handbrake.cases.json", (id, given, expect) =>
        {
            if (Directory.Exists(_root)) Directory.Delete(_root, true);
            Directory.CreateDirectory(_root);
            foreach (var f in given["files"]?.AsArray ?? Array.Empty<JsonValue>())
            {
                Directory.CreateDirectory(Path.GetDirectoryName(R(f.AsString!))!);
                File.WriteAllText(R(f.AsString!), "");
            }
            var lines = new List<string>();
            var stderr = new HashSet<int>();
            if (given["linesFile"]?.AsString is { } file)
                foreach (var l in Text(file).Split('\n').Select(l => l.TrimEnd('\r')).Where(l => l.Length > 0))
                {
                    stderr.Add(lines.Count);
                    lines.Add(l);
                }
            foreach (var l in given["lines"]?.AsArray ?? Array.Empty<JsonValue>())
            {
                if (l["stderr"]?.AsString is { } e) stderr.Add(lines.Count);
                lines.Add(l.AsString ?? l["stderr"]!.AsString!);
            }
            var launcher = new ScriptedProcessLauncher { Lines = lines, Stderr = stderr, ExitCode = (int)(given["exitCode"]?.AsInteger ?? 0) };
            var tools = new Dictionary<ToolKind, string>();
            if (given["located"]?.AsBool != false) tools[ToolKind.Handbrake] = "/opt/HandBrakeCLI";
            var clock = new FiringClock { Fire = given["timerFires"]?.AsBool == true };
            var handbrake = new HandBrake(launcher, new MapLocator(tools), clock, _home);
            var cancel = new CancellationToken();
            if (given["cancelled"]?.AsBool == true) cancel.Cancel();
            var sink = new RecordingSink();
            try
            {
                if (given["op"]!.AsString == "presets")
                {
                    var presets = handbrake.Presets(cancel).GetAwaiter().GetResult();
                    if (expect["presets"] is { } all) Assert.Equal(all.AsArray!.Select(p => p.AsString!), presets);
                    if (expect["count"]?.AsInteger is { } n) Assert.Equal(n, presets.Count);
                    if (expect["first"]?.AsString is { } first) Assert.Equal(first, presets[0]);
                    if (expect["last"]?.AsString is { } last) Assert.Equal(last, presets[^1]);
                    foreach (var p in expect["includes"]?.AsArray ?? Array.Empty<JsonValue>()) Assert.Contains(p.AsString!, presets);
                }
                else
                {
                    var stepText = JsonText(given["step"]!).Replace("<root>", _root.Replace("\\", "\\\\")).Replace("<home>", _home.Replace("\\", "\\\\"));
                    var step = StepDefinition.Decode(JsonValue.Parse(System.Text.Encoding.UTF8.GetBytes(stepText))!);
                    var run = handbrake.Encode(step, R(given["input"]!.AsString!), R(given["output"]!.AsString!), sink, cancel).GetAwaiter().GetResult();
                    Assert.True(expect["error"] == null, "expected " + expect["error"]);
                    Assert.Equal(expect["status"]!.AsInteger, run.Exit.Status);
                    Assert.Equal(expect["timedOut"]!.AsBool, run.TimedOut);
                }
            }
            catch (BroFailure f)
            {
                Assert.Equal(expect["error"]?["code"]?.AsString, f.Error.Code);
                foreach (var m in expect["error"]!["params"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                    Assert.Equal(R(m.Value.AsString!), f.Error.Params[m.Key]!.AsString);
            }
            if (expect["started"]?.AsBool == false) Assert.Empty(launcher.Started);
            if (expect["argv"] is { } argv)
                Assert.Equal(argv.AsArray!.Select(a => R(a.AsString!)), new[] { launcher.Started[0].Executable }.Concat(launcher.Started[0].Arguments));
            if (expect["workingDirectory"]?.AsString is { } wd)
                Assert.Equal(R(wd).Replace('\\', '/'), launcher.Started[0].WorkingDirectory?.Replace('\\', '/'));
            if (expect["progress"] is { } progress)
                Assert.Equal(progress.AsArray!.Select(p => $"{p.AsArray![0].AsInteger}/{p.AsArray![1].AsInteger}"),
                    sink.Events.OfType<RobotEvent.ProgressValue>().Select(p => $"{p.Current}/{p.Total}"));
            Assert.All(sink.Events.OfType<RobotEvent.ProgressValue>(), p => Assert.Equal(10000, p.Max));
            if (expect["raw"] is { } raw)
                Assert.Equal(raw.AsArray!.Select(r => r.AsString!), sink.Events.OfType<RobotEvent.Raw>().Select(r => r.Text));
            if (expect["timer"]?.AsInteger is { } seconds)
                Assert.Equal(new TimerSchedule.At(new Instant(clock.At.UnixMilliseconds + seconds * 1000)), Assert.Single(clock.Timers));
            else
                Assert.Empty(clock.Timers);
            return true;
        });
    }

    static string JsonText(JsonValue v) => System.Text.Encoding.UTF8.GetString(JsonValue.EncodeCanonical(v));
}
