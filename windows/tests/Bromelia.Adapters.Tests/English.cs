using Bromelia.Adapters;
using Bromelia.Foundation;

namespace Bromelia.Tests;

/// <summary>Renders a message <c>{code, params, cause}</c> in English, for fixtures that check a message's text: the
/// host's MessageTextRenderer over the generated English catalog (shared/messages/en.json).</summary>
public static class English
{
    private static readonly MessageTextRenderer Renderer = new(EnglishMessages.Catalog);

    public static string Render(JsonValue message) => Renderer.RenderJson(message);
}
