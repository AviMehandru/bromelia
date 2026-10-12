using System.Collections.Generic;
using System.Linq;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Engine;

/// <summary>The configuration as clients get it (dtos.json ConfigEnvelope): the saved revision, the document, its
/// issues, and every secret it refers to with whether it is set.</summary>
public sealed record ConfigEnvelope(long Version, Config Config, IReadOnlyList<Issue> Issues, IReadOnlyList<SecretStatus> Secrets)
{
    public static JsonValue Json(ConfigEnvelope envelope) => JsonValue.Of(
        ("version", JsonValue.Of(envelope.Version)),
        ("doc", envelope.Config.Document),
        ("issues", JsonValue.Of(envelope.Issues.Select(i => i.ToJson()))),
        ("secrets", JsonValue.Of(envelope.Secrets.Select(s => JsonValue.Of(("name", JsonValue.Of(s.Name)), ("set", JsonValue.Of(s.Set)))))));
}
