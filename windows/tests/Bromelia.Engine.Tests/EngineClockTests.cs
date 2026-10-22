using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Engine.Tests;

/// <summary>shared/fixtures/engine/engine-clock.cases.json and coalescer.cases.json, on a real executor.</summary>
public sealed class EngineClockTests
{
    /// <summary>A real executor and an EngineClock over a manual clock that waits for the executor after each timer.</summary>
    internal sealed class World : IDisposable
    {
        public readonly ManualClock Clock = new();
        public readonly EngineExecutor Executor = new("engine-test");
        public readonly EngineClock EngineClock;

        public World()
        {
            EngineClock = new EngineClock(Clock, Executor);
            Clock.Settle = () =>
            {
                if (!Executor.IsCurrent) Settle();
            };
        }

        /// <summary>Runs <paramref name="action"/> on the executor and waits for it (at once when already there).</summary>
        public void OnExecutor(Action action)
        {
            if (Executor.IsCurrent)
            {
                action();
                return;
            }
            Exception? failure = null;
            Executor.Post(() =>
            {
                try { action(); }
                catch (Exception e) { failure = e; }
            });
            Settle();
            if (failure != null) throw failure;
        }

        /// <summary>Waits until the executor has run everything posted so far.</summary>
        public void Settle()
        {
            if (!Executor.Barrier().Wait(TimeSpan.FromSeconds(10))) throw new TimeoutException("the executor didn't settle");
        }

        public void Dispose() => Executor.Stop();
    }

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("engine/engine-clock.cases.json", (id, given, expect) =>
        {
            using var w = new World();
            var timers = new Dictionary<string, EngineTimer>();
            var fired = new List<string>();
            var allOnExecutor = true;
            var localTimes = new List<string>();

            void Run(IEnumerable<JsonValue> ops)
            {
                foreach (var op in ops)
                {
                    var (name, arg) = op.AsObject!.Single();
                    switch (name)
                    {
                        case "timer":
                        {
                            var @as = arg["as"]!.AsString!;
                            TimerSchedule schedule = arg["at"] is { } at
                                ? new TimerSchedule.At(new Instant(ManualClock.StartMs + at.AsInteger!.Value))
                                : new TimerSchedule.Every(new Duration(arg["every"]!.AsNumber!.Value));
                            var cancelAfter = arg["cancelSelfAfter"]?.AsInteger;
                            var count = 0;
                            w.OnExecutor(() => timers[@as] = w.EngineClock.Timer(schedule, () =>
                            {
                                allOnExecutor &= w.Executor.IsCurrent;
                                fired.Add(@as);
                                if (++count == cancelAfter) timers[@as].Cancel();
                            }));
                            break;
                        }
                        case "advance":
                            w.Clock.Advance(arg.AsInteger!.Value);
                            break;
                        case "cancel":
                            w.OnExecutor(() => timers[arg.AsString!].Cancel());
                            break;
                        case "onExecutor":
                            w.OnExecutor(() => Run(arg.AsArray!));
                            break;
                        case "utcOffsetMinutes":
                            w.Clock.UtcOffsetMinutes = (int)arg.AsInteger!.Value;
                            break;
                        case "localTime":
                        {
                            var t = w.EngineClock.LocalTime(Instant.Parse(arg.AsString!)!.Value);
                            localTimes.Add($"{t.Year:0000}-{t.Month:00}-{t.Day:00} {t.Hour:00}:{t.Minute:00}:{t.Second:00}");
                            break;
                        }
                        default:
                            throw new InvalidOperationException("unknown op " + name);
                    }
                }
            }

            Run(given["ops"]!.AsArray!);
            w.Settle();
            Same(string.Join(" ", expect["fired"]!.AsArray!.Select(f => f.AsString)), string.Join(" ", fired), "fired");
            if (expect["allOnExecutor"] is { } on) Same(on.AsBool, (bool?)allOnExecutor, "allOnExecutor");
            if (expect["now"] is { } now) Same(now.AsString, Instant.Format(w.EngineClock.Now()), "now");
            if (expect["localTimes"] is { } lts) Same(string.Join(" | ", lts.AsArray!.Select(l => l.AsString)), string.Join(" | ", localTimes), "localTimes");
            if (expect["monotonicSeconds"] is { } mono) Same(mono.AsNumber, (double?)w.EngineClock.Monotonic().Seconds, "monotonic");
            return true;
        });
    }

    [Fact]
    public void TheSharedCoalescerCasesPass()
    {
        RunCases("engine/coalescer.cases.json", (id, given, expect) =>
        {
            using var w = new World();
            var emitted = new List<string>();
            Coalescer? coalescer = null;
            w.OnExecutor(() => coalescer = new Coalescer(w.EngineClock, new Duration(0.1), (key, value) =>
                emitted.Add($"{Math.Round(w.EngineClock.Monotonic().Seconds * 1000)} {key} {value.AsString}")));
            foreach (var op in given["ops"]!.AsArray!)
            {
                var (name, arg) = op.AsObject!.Single();
                switch (name)
                {
                    case "offer":
                        w.OnExecutor(() => coalescer!.Offer(arg["key"]!.AsString!, arg["value"]!));
                        break;
                    case "advance":
                        w.Clock.Advance(arg.AsInteger!.Value);
                        break;
                    case "flush":
                        w.OnExecutor(() => coalescer!.Flush(arg.AsString!));
                        break;
                    case "drop":
                        w.OnExecutor(() => coalescer!.Drop(arg.AsString!));
                        break;
                    default:
                        throw new InvalidOperationException("unknown op " + name);
                }
            }
            w.Settle();
            Same(string.Join(" | ", expect["emitted"]!.AsArray!.Select(e => e.AsString)), string.Join(" | ", emitted), "emitted");
            return true;
        });
    }
}
