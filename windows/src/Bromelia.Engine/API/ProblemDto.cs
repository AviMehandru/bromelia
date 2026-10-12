using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>A problem every UI shows until it is cleared (dtos.json Problem): the message (code, params, severity
/// warning or error), what it is about, when it was first raised and what a UI can offer.</summary>
public sealed record ProblemDto(string Id, BroMessage Message, ProblemScope? Scope, Instant RaisedAt, IReadOnlyList<ProblemAction> Actions)
{
    /// <summary>Optional members left out when absent or empty.</summary>
    public static JsonValue Json(ProblemDto problem)
    {
        var m = new List<KeyValuePair<string, JsonValue>>
        {
            new("id", JsonValue.Of(problem.Id)),
            new("code", JsonValue.Of(MessageCode.Wire(problem.Message.Code))),
        };
        if (problem.Message.Params.Members.Count > 0) m.Add(new("params", problem.Message.Params));
        m.Add(new("severity", JsonValue.Of(EnumWire.Name(problem.Message.Severity))));
        if (problem.Scope is { } scope)
            m.Add(new("scope", JsonValue.Of(("kind", JsonValue.Of(EnumWire.Name(scope.Kind))), ("id", JsonValue.Of(scope.Id)))));
        m.Add(new("raisedAt", JsonValue.Of(Instant.Format(problem.RaisedAt))));
        if (problem.Actions.Count > 0) m.Add(new("actions", JsonValue.Of(problem.Actions.Select(a => JsonValue.Of(EnumWire.Name(a))))));
        return new JsonValue.Object(m);
    }
}
