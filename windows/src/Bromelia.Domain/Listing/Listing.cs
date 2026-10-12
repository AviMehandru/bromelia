using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What makemkvcon reported about a disc, ISO or folder: its name (2, else 32), volume name (32), type
/// (from 1), the number of titles it announced, the titles in index order, and every disc attribute.</summary>
public sealed record Listing(
    string Name,
    string VolumeName,
    DiscType Type,
    string TypeText,
    int ReportedTitleCount,
    IReadOnlyList<Title> Titles,
    IReadOnlyDictionary<int, string> Attributes)
{
    public static readonly Listing Empty = new("", "", DiscType.Disc, "", 0, new List<Title>(), new Dictionary<int, string>());

    /// <summary>The listing as JSON: dtos.json's Listing, as shared/fixtures/domain/info-dvd.listing.expected.json
    /// writes it (attributes keyed by their id as text, in id order).</summary>
    public static JsonValue Encode(Listing listing) => JsonValue.Of(
        ("name", JsonValue.Of(listing.Name)), ("volumeName", JsonValue.Of(listing.VolumeName)), ("type", JsonValue.Of(EnumWire.Name(listing.Type))),
        ("typeText", JsonValue.Of(listing.TypeText)), ("reportedTitleCount", JsonValue.Of(listing.ReportedTitleCount)),
        ("titles", JsonValue.Of(listing.Titles.Select(t => JsonValue.Of(
            ("index", JsonValue.Of(t.Index)), ("sourceTitleId", OrNull(t.SourceTitleId)), ("sourceFile", JsonValue.Of(t.SourceFile)),
            ("name", JsonValue.Of(t.Name)), ("comment", JsonValue.Of(t.Comment)), ("duration", JsonValue.Of(t.Duration)),
            ("durationSeconds", JsonValue.Of(t.DurationSeconds)), ("chapters", JsonValue.Of(t.Chapters)), ("sizeBytes", JsonValue.Of(t.SizeBytes)),
            ("segmentMap", JsonValue.Of(t.SegmentMap)), ("outputFileName", JsonValue.Of(t.OutputFileName)), ("angle", OrNull(t.Angle)),
            ("tracks", JsonValue.Of(t.Tracks.Select(k => JsonValue.Of(
                ("index", JsonValue.Of(k.Index)), ("kind", JsonValue.Of(EnumWire.Name(k.Kind))), ("codec", JsonValue.Of(k.Codec)),
                ("language", JsonValue.Of(k.Language)), ("languageName", JsonValue.Of(k.LanguageName)), ("name", JsonValue.Of(k.Name)),
                ("isDefault", JsonValue.Of(k.IsDefault)), ("attributes", AttributesJson(k.Attributes)))))),
            ("attributes", AttributesJson(t.Attributes)))))),
        ("attributes", AttributesJson(listing.Attributes)));

    /// <summary>A listing from its JSON (<see cref="Encode"/>); what the JSON leaves out is empty.</summary>
    public static Listing Decode(JsonValue json) => new(
        json["name"]?.AsString ?? "", json["volumeName"]?.AsString ?? "", EnumWire.Parse<DiscType>(json["type"]?.AsString) ?? DiscType.Disc,
        json["typeText"]?.AsString ?? "", (int)(json["reportedTitleCount"]?.AsInteger ?? 0),
        (json["titles"]?.AsArray ?? new List<JsonValue>()).Select(t => new Title(
            (int)(t["index"]?.AsInteger ?? 0), (int?)t["sourceTitleId"]?.AsInteger, t["sourceFile"]?.AsString ?? "", t["name"]?.AsString ?? "",
            t["comment"]?.AsString ?? "", t["duration"]?.AsString ?? "", (int)(t["durationSeconds"]?.AsInteger ?? 0),
            (int)(t["chapters"]?.AsInteger ?? 0), t["sizeBytes"]?.AsInteger ?? 0, t["segmentMap"]?.AsString ?? "",
            t["outputFileName"]?.AsString ?? "", (int?)t["angle"]?.AsInteger,
            (t["tracks"]?.AsArray ?? new List<JsonValue>()).Select(k => new Track(
                (int)(k["index"]?.AsInteger ?? 0), EnumWire.Parse<TrackKind>(k["kind"]?.AsString) ?? TrackKind.Unknown, k["codec"]?.AsString ?? "",
                k["language"]?.AsString ?? "", k["languageName"]?.AsString ?? "", k["name"]?.AsString ?? "", k["isDefault"]?.AsBool ?? false,
                AttributesOf(k["attributes"]))).ToList(),
            AttributesOf(t["attributes"]))).ToList(),
        AttributesOf(json["attributes"]));

    static JsonValue OrNull(int? v) => v is { } x ? JsonValue.Of(x) : JsonValue.Null.Instance;

    static JsonValue AttributesJson(IReadOnlyDictionary<int, string> a) =>
        JsonValue.Of(a.OrderBy(kv => kv.Key).Select(kv => (kv.Key.ToString(CultureInfo.InvariantCulture), JsonValue.Of(kv.Value))).ToArray());

    static Dictionary<int, string> AttributesOf(JsonValue? json)
    {
        var result = new Dictionary<int, string>();
        foreach (var (key, value) in json?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            if (int.TryParse(key, NumberStyles.None, CultureInfo.InvariantCulture, out var id) && value.AsString is { } s) result[id] = s;
        return result;
    }
}
