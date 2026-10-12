using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What the user chose on an opened disc (dtos.json's SessionChoices): every title with whether it is ticked,
/// the name typed for the disc ("" = from the disc), movie or TV, year, online id, first episode, library and
/// profile (null = the default).</summary>
public sealed record SessionChoices(
    IReadOnlyList<TitleChoice> Titles,
    string Name = "",
    MediaKind? Kind = null,
    int? Year = null,
    string OnlineId = "",
    int? FirstEpisode = null,
    string? Library = null,
    string? Profile = null)
{
    public static JsonValue Encode(SessionChoices choices)
    {
        JsonValue N(int? v) => v is { } x ? JsonValue.Of(x) : JsonValue.Null.Instance;
        JsonValue S(string? v) => v is null ? JsonValue.Null.Instance : JsonValue.Of(v);
        return JsonValue.Of(
            ("titles", JsonValue.Of(choices.Titles.Select(t =>
            {
                var members = new List<(string, JsonValue)> { ("index", JsonValue.Of(t.Index)), ("selected", JsonValue.Of(t.Selected)) };
                if (t.Tracks is { } tracks) members.Add(("tracks", JsonValue.Of(tracks.Select(k => JsonValue.Of(k)))));
                if (t.Name.Length > 0) members.Add(("name", JsonValue.Of(t.Name)));
                return JsonValue.Of(members.ToArray());
            }))),
            ("name", JsonValue.Of(choices.Name)), ("kind", choices.Kind is { } k ? JsonValue.Of(EnumWire.Name(k)) : JsonValue.Null.Instance),
            ("year", N(choices.Year)), ("onlineId", JsonValue.Of(choices.OnlineId)), ("firstEpisode", N(choices.FirstEpisode)),
            ("library", S(choices.Library)), ("profile", S(choices.Profile)));
    }

    /// <summary>The choices <see cref="Encode"/> wrote; what the JSON leaves out takes its default.</summary>
    public static SessionChoices Decode(JsonValue json) => new(
        (json["titles"]?.AsArray ?? new List<JsonValue>()).Select(t => new TitleChoice((int)(t["index"]?.AsInteger ?? 0), t["selected"]?.AsBool ?? false,
            t["tracks"]?.AsArray?.Select(k => (int)(k.AsInteger ?? 0)).ToList(), t["name"]?.AsString ?? "")).ToList(),
        json["name"]?.AsString ?? "", EnumWire.Parse<MediaKind>(json["kind"]?.AsString), (int?)json["year"]?.AsInteger,
        json["onlineId"]?.AsString ?? "", (int?)json["firstEpisode"]?.AsInteger, json["library"]?.AsString, json["profile"]?.AsString);
}
