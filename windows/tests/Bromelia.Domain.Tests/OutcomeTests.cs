using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using Bromelia.Tests;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class OutcomeTests
{
    internal static JsonValue ErrorJson(BroError e)
    {
        var members = new List<(string, JsonValue)> { ("code", JsonValue.Of(e.Code)) };
        if (e.Params.Members.Count > 0) members.Add(("params", e.Params));
        if (e.Cause is { } cause) members.Add(("cause", ErrorJson(cause)));
        return JsonValue.Of(members.ToArray());
    }

    private static StepResult StepOf(JsonValue s) => new(
        EnumWire.Parse<StepKind>(s["kind"]!.AsString)!.Value,
        EnumWire.Parse<StepState>(s["state"]!.AsString)!.Value,
        s["error"] is { } e ? new BroError(e["code"]!.AsString!, (JsonValue.Object?)e["params"] ?? (JsonValue.Object)JsonValue.Of()) : null,
        (int)(s["readErrors"]?.AsInteger ?? 0),
        s["quarantined"]?.AsBool ?? false,
        s["notice"]?.AsString switch
        {
            "keyExpired" => new MakemkvNotice.KeyExpired(),
            "evaluationNotStarted" => new MakemkvNotice.EvaluationNotStarted(),
            "versionTooOld" => new MakemkvNotice.VersionTooOld(),
            "libreDriveRequired" => new MakemkvNotice.LibreDriveRequired(),
            _ => null,
        },
        s["skip"] is { } skip ? new BroMessage(MessageCode.Parse(skip["code"]!.AsString!)!.Value, (JsonValue.Object?)skip["params"] ?? (JsonValue.Object)JsonValue.Of()) : null,
        s["affectsOutcome"]?.AsBool ?? true);

    [Fact]
    public void OutcomeCases() => RunCases("domain/outcome.cases.json", (id, given, expect) =>
    {
        if (given["fixture"] is { } fixture)
        {
            var acc = RobotTests.Accumulate(fixture.AsString!, readsData: true);
            var run = RunOutcome.Classify(acc, new ProcessExit((int)given["exitCode"]!.AsInteger!), (int)given["producedFiles"]!.AsInteger!);
            Same(expect["statusWord"]!.AsString, EnumWire.Name(run.Status), "status word");
            Same(expect["failed"]!.AsBool, run.Status == StatusWord.Failed, "failed");
            Same(expect["readErrors"]!.AsBool, run.Status == StatusWord.Errors, "read errors");
            var text = run.Error is { } e ? English.Render(e.ToJson()) : null;
            if (expect["errorContains"]!.AsString is { } part)
            {
                if (text is null || !text.Contains(part)) throw new Xunit.Sdk.XunitException($"error \"{text}\" doesn't contain \"{part}\"");
            }
            else Same(null, text, "error");
            return true;
        }
        if (given["steps"] is { } steps)
        {
            var d = JobOutcome.Decide(steps.AsArray!.Select(StepOf).ToList(), given["cancelled"]!.AsBool!.Value, OutcomePolicy.Default);
            Same(expect["outcome"]!.AsString, EnumWire.Name(d.Outcome), "outcome");
            if (expect["error"] is { } error) Same(error, d.Error is { } e ? ErrorJson(e) : JsonValue.Null.Instance, "error");
            else if (d.Outcome == Outcome.Succeeded) Same(null, d.Error, "error");
            return true;
        }
        return false;
    });

    [Fact]
    public void StalledAndCancelledRuns()
    {
        var acc = new RunAccumulator(readsData: true);
        var stalled = RunOutcome.Classify(acc, new ProcessExit(-1, 9, new Duration(600), Abandoned: true), 0);
        Assert.Equal(StatusWord.Failed, stalled.Status);
        Assert.Equal("makemkvcon printed nothing for 10 minutes and was stopped (it did not exit; the drive may need to be reset). The drive or disc may be stuck: eject the disc and retry.",
            English.Render(stalled.Error!.ToJson()));
        Assert.Equal(StatusWord.Cancelled, RunOutcome.Classify(acc, new ProcessExit(143, 15, Cancelled: true), 1).Status);
    }
}
