using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>A profile: sparse, as written in the configuration (a missing field is inherited; see
/// ProfileResolver), so it stays a JSON object. Its id is empty for a template not added yet.</summary>
public sealed record Profile(JsonValue Json)
{
    public string Id => Json["id"]?.AsString ?? "";
    public string Name => Json["name"]?.AsString ?? "";
}
