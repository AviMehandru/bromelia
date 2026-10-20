using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Bromelia.Tests;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Adapters.Tests;

public class MessageTextRendererTests
{
    private const string Cases = "adapters/message-renderer.cases.json";

    // The cases' own catalog: texts by code, and the params that are message lists.
    private static MessageTexts TestCatalog()
    {
        var catalog = Json(Cases)["catalog"]!;
        return new MessageTexts(catalog["texts"]!.AsObject!.ToDictionary(m => m.Key, m => m.Value.AsString!),
            catalog["messageLists"]!.AsObject!.SelectMany(m => m.Value.AsArray!.Select(p => (m.Key, p.AsString!))));
    }

    [Fact]
    public void RendererCases()
    {
        var test = new MessageTextRenderer(TestCatalog());
        var english = new MessageTextRenderer(EnglishMessages.Catalog);
        RunCases(Cases, (id, given, expect) =>
        {
            var renderer = given["catalog"]?.AsString == "english" ? english : test;
            var message = given["message"]!;
            var text = given["via"]?.AsString == "port" ? ((IMessageRenderer)renderer).Render(BroMessage.Decode(message)) : renderer.RenderJson(message);
            Same(expect["text"]!.AsString, text, "text");
            return true;
        });
    }
}
