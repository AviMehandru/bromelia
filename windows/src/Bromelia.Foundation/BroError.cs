using System.Collections.Generic;

namespace Bromelia.Foundation;

/// <summary>An expected failure (plan §16): a message code in its wire form (<c>"library.offline"</c>), its
/// parameters and the failure that caused it. Domain's <c>MessageCode</c> gives the codes their names.</summary>
public sealed record BroError(string Code, JsonValue.Object Params, BroError? Cause = null)
{
    public BroError(string code) : this(code, new JsonValue.Object(new List<KeyValuePair<string, JsonValue>>())) { }

    public BroError(string code, params (string Key, JsonValue Value)[] parameters)
        : this(code, (JsonValue.Object)JsonValue.Of(parameters)) { }
}
