using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class MessagesTests
{
    [Fact]
    public void EveryCodeOfTheCatalogueIsACase()
    {
        var codes = SharedJson("messages/codes.json")["codes"]!.AsObject!.Select(m => m.Key).ToList();
        Assert.Equal(codes, MessageCode.All().Select(MessageCode.Wire).ToList());
        foreach (var code in codes)
            Assert.Equal(code, MessageCode.Wire(MessageCode.Parse(code)!.Value));
        Assert.Null(MessageCode.Parse("no.suchCode"));
        Assert.Equal("rip.readErrors", MessageCode.Wire(MessageCode.RipReadErrors));
    }

    [Fact]
    public void MessagesBecomeErrorsAndJson()
    {
        var m = new BroMessage(MessageCode.LibraryOffline, Severity.Error, ("library", JsonValue.Of("Films")));
        Assert.Equal(new BroError("library.offline", ("library", JsonValue.Of("Films"))), m.ToError());
        Assert.Equal(JsonValue.Parse("{\"code\": \"library.offline\", \"params\": {\"library\": \"Films\"}}"), m.ToJson());
    }

    [Fact]
    public void JobEnumsMatchTheSharedSchema()
    {
        var defs = SharedJson("schema/common.json")["$defs"]!;
        void Check<T>(string name) where T : struct, Enum
        {
            var expected = defs[name]!["enum"]!.AsArray!.Select(v => v.AsString!).ToList();
            Assert.Equal(expected, Enum.GetValues<T>().Select(EnumWire.Name).ToList());
            foreach (var e in expected) Assert.Equal(e, EnumWire.Name(EnumWire.Parse<T>(e)!.Value));
        }
        Check<JobKind>("JobKind");
        Check<JobState>("JobState");
        Check<Outcome>("Outcome");
        Check<StepKind>("StepKind");
        Check<StepState>("StepState");
        Check<Queue>("Queue");
        Check<Severity>("Severity");
        Assert.Null(EnumWire.Parse<Outcome>("Succeeded"));
        Assert.Null(EnumWire.Parse<Outcome>("nope"));
    }
}
