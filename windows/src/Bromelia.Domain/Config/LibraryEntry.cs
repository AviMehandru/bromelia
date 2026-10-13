using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>A library of the configuration (config-3.json's <c>Library</c>): where archives are committed.</summary>
public sealed record LibraryEntry(string Id, string Name, string Path)
{
    /// <summary>A library entry from its JSON ("" for what it leaves out).</summary>
    public static LibraryEntry Decode(JsonValue json) =>
        new(json["id"]?.AsString ?? "", json["name"]?.AsString ?? "", json["path"]?.AsString ?? "");
}
