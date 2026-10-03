using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Tests;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class NotifyTests
{
    /// <summary>A delivery against a fixture's expectation: the URL, the headers it names, the body as JSON or text,
    /// or the Apprise URL; <c>delivery: null</c> for none.</summary>
    internal static void SameDelivery(JsonValue expect, Delivery? got)
    {
        if (expect.AsObject!.Any(m => m.Key == "delivery")) { Assert.Null(got); return; }
        if (expect["apprise"] is { } apprise)
        {
            Same(apprise.AsString, (got as Delivery.Apprise)?.Url, "apprise");
            return;
        }
        var request = Assert.IsType<Delivery.Http>(got).Request;
        Same("POST", request.Method, "method");
        if (expect["url"] is { } url) Same(url.AsString, request.Url, "url");
        foreach (var h in expect["headers"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            Same(h.Value.AsString, request.Headers.FirstOrDefault(x => x.Key == h.Key).Value, "header " + h.Key);
        if (expect["json"] is { } json) Same(Sorted(json), Sorted(JsonValue.Parse(request.Body!)!), "json");
        if (expect["text"] is { } text) Same(text.AsString, Encoding.UTF8.GetString(request.Body!), "text");
    }

    private static StatusWord Status(JsonValue v) => EnumWire.Parse<StatusWord>(v.AsString)!.Value;

    [Fact]
    public void NotifyCases() => RunCases("domain/notify.cases.json", (id, given, expect) =>
    {
        if (given["fixture"] is { } fixture)
        {
            foreach (var c in Json(fixture.AsString!)["cases"]!.AsArray!)
                SameDelivery(c["expect"]!, NotifyRequests.Build(c["url"]!.AsString!, "T", "B", Status(c["status"]!)));
            return true;
        }
        if (given["url"] is { } url)
        {
            SameDelivery(expect, NotifyRequests.Build(url.AsString!, given["title"]!.AsString!, given["body"]!.AsString!, Status(given["status"]!)));
            return true;
        }
        if (given["targets"] is { } targets)
        {
            var list = targets.AsArray!.Select(t => new NotifyTarget(t["id"]!.AsString!, "url." + t["id"]!.AsString!,
                OnlyProblems: t["onlyProblems"]?.AsBool ?? false)).ToList();
            foreach (var m in expect["sentTo"]!.AsObject!)
                Same(m.Value, JsonValue.Of(NotifyRequests.TargetsFor(list, Status(JsonValue.Of(m.Key))).Select(t => JsonValue.Of(t.Id))), "sentTo " + m.Key);
            return true;
        }
        var outcome = EnumWire.Parse<Outcome>(given["outcome"]!.AsString)!.Value;
        var job = new JobSummary(EnumWire.Parse<RipMode>(given["mode"]?.AsString) ?? RipMode.Mkv, given["what"]?.AsString ?? "",
            (int)(given["files"]?.AsInteger ?? 0), given["path"]?.AsString ?? "");
        var message = NotifyMessages.ForJob(job, outcome);
        if (expect["title"] is { } title) Same(title, message.Title.ToJson(), "title");
        if (expect["body"] is { } body) Same(body, Assert.Single(message.Lines).ToJson(), "body");
        if (expect["text"] is { } text)
            Same(text.AsString, English.Render((given["mode"] != null ? message.Title : message.Lines[0]).ToJson()), "text");
        return true;
    });

    private static FolderCheck Folder(string name, bool ok)
    {
        var verdicts = new Dictionary<string, FileVerdict> { ["a.mkv"] = ok ? FileVerdict.Same : FileVerdict.Changed };
        return new FolderCheck(name, VerifyResult.Compare(new[] { "a.mkv" }, verdicts, new[] { "a.mkv" }));
    }

    [Fact]
    public void CheckNotifications()
    {
        var allOk = NotifyMessages.ForCheck(new[] { Folder("A", true), Folder("B", true) });
        Assert.Equal("Archive check: all 2 folder(s) OK", English.Render(allOk.Title.ToJson()));
        Assert.Equal("Every file matches its checksum.", English.Render(Assert.Single(allOk.Lines).ToJson()));
        var results = Enumerable.Range(0, 12).Select(i => Folder("F" + i, false)).Append(Folder("Good", true)).ToList();
        var damaged = NotifyMessages.ForCheck(results);
        Assert.Equal("Archive check: 12 of 13 folder(s) damaged", English.Render(damaged.Title.ToJson()));
        Assert.Equal(11, damaged.Lines.Count);
        Assert.Equal("F0: 1 changed of 1 file", English.Render(damaged.Lines[0].ToJson()));
        Assert.Equal("… and 2 more", English.Render(damaged.Lines[10].ToJson()));
        var failed = NotifyMessages.ForJob(new JobSummary(RipMode.Backup, "X", 0, "/p"), Outcome.Cancelled);
        Assert.Equal("Cancelled", English.Render(failed.Lines[0].ToJson()));
    }
}
