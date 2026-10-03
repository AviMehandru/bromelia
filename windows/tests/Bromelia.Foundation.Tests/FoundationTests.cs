using System.Text;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Foundation.Tests;

public class FoundationTests
{
    [Fact]
    public void FoundationCases() => RunCases("foundation/foundation.cases.json", (id, given, expect) =>
    {
        if (given["id"] is { } idText)
        {
            Same(expect["short"]!.AsString, Id.Short(new Id(idText.AsString!)), "short id");
            return true;
        }
        if (given["instant"] is { } instant)
        {
            var parsed = Instant.Parse(instant.AsString!);
            Same(expect["formatted"]!.AsString, parsed is { } p ? Instant.Format(p) : null, "instant");
            return true;
        }
        if (given["clock"] is { } clock)
        {
            var d = Duration.ParseClock(clock.AsString!);
            Same(expect["seconds"]!.AsInteger, d is { } x ? (long)x.Seconds : null, "seconds");
            Same(expect["formatted"]!.AsString, d is { } y ? Duration.FormatClock(y) : null, "formatted");
            return true;
        }
        if (given["json"] is { } json)
        {
            var v = JsonValue.Parse(json.AsString!);
            Same(expect["canonical"]!.AsString, v is null ? null : Encoding.UTF8.GetString(JsonValue.EncodeCanonical(v)), "canonical");
            return true;
        }
        return false;
    });

    [Fact]
    public void JsonValuesCompareByContent()
    {
        Assert.Equal(JsonValue.Parse("{\"a\": [1, {\"b\": null}]}"), JsonValue.Parse("{ \"a\" : [ 1 , { \"b\" : null } ] }"));
        Assert.NotEqual(JsonValue.Parse("{\"a\": 1, \"b\": 2}"), JsonValue.Parse("{\"b\": 2, \"a\": 1}"));
        Assert.NotEqual(JsonValue.Parse("1"), JsonValue.Parse("1.0"));
    }

    [Fact]
    public void CancellationRunsHandlersOnce()
    {
        var token = new CancellationToken();
        var child = CancellationToken.Child(token);
        int calls = 0;
        using var _ = child.OnCancel(() => calls++);
        Assert.False(child.IsCancelled);
        token.Cancel();
        token.Cancel();
        Assert.True(child.IsCancelled);
        Assert.Equal(1, calls);
    }
}
