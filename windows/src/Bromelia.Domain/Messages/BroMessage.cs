using System.Collections.Generic;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What logs, progress, problems and notifications carry (plan §16): a code, its parameters and a
/// severity. The engine never builds sentences; clients render the code from shared/messages.</summary>
public sealed record BroMessage(MessageCode Code, JsonValue.Object Params, Severity Severity = Severity.Info)
{
    public BroMessage(MessageCode code, Severity severity = Severity.Info, params (string Key, JsonValue Value)[] parameters)
        : this(code, (JsonValue.Object)JsonValue.Of(parameters), severity) { }

    /// <summary>The message as an expected failure.</summary>
    public BroError ToError(BroError? cause = null) => new(MessageCode.Wire(Code), Params, cause);

    /// <summary><c>{"code": …, "params": {…}}</c>, as in shared/schema/common.json (params left out when empty).</summary>
    public JsonValue ToJson()
    {
        var members = new List<KeyValuePair<string, JsonValue>> { new("code", new JsonValue.String(MessageCode.Wire(Code))) };
        if (Params.Members.Count > 0) members.Add(new("params", Params));
        return new JsonValue.Object(members);
    }
}
