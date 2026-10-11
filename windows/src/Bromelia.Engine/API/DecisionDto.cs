using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>A question a job waits on (dtos.json Decision).</summary>
public sealed record DecisionDto(string Id, BroMessage Question, IReadOnlyList<DecisionAnswer> Answers)
{
    public static JsonValue Json(DecisionDto decision) => JsonValue.Of(
        ("id", JsonValue.Of(decision.Id)),
        ("question", decision.Question.ToJson()),
        ("answers", JsonValue.Of(decision.Answers.Select(a => JsonValue.Of(("id", JsonValue.Of(a.Id)), ("label", a.Label.ToJson()))))));
}
