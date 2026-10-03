using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>A problem with the configuration: where (a JSON path such as <c>profiles[0].mode.default</c>), what
/// (an issue code with its parameters) and how bad (warning: ignored; error: the document is rejected).</summary>
public sealed record Issue(string Path, MessageCode Code, JsonValue.Object Params, Severity Severity)
{
    /// <summary><c>{path, code, params?, severity}</c>, as the API's Issue.</summary>
    public JsonValue ToJson()
    {
        var members = new System.Collections.Generic.List<(string, JsonValue)> { ("path", JsonValue.Of(Path)), ("code", JsonValue.Of(MessageCode.Wire(Code))) };
        if (Params.Members.Count > 0) members.Add(("params", Params));
        members.Add(("severity", JsonValue.Of(EnumWire.Name(Severity))));
        return JsonValue.Of(members.ToArray());
    }
}
