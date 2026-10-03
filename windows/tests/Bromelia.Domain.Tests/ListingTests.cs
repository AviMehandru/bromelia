using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class ListingTests
{
    internal static Listing ListingOf(string robotText)
    {
        var b = new ListingBuilder();
        foreach (var line in robotText.Split('\n'))
            if (Robot.ParseLine(line) is { } e) b.Feed(e);
        return b.Build();
    }

    private static JsonValue AttributesJson(IReadOnlyDictionary<int, string> a) =>
        JsonValue.Of(a.OrderBy(kv => kv.Key).Select(kv => (kv.Key.ToString(System.Globalization.CultureInfo.InvariantCulture), JsonValue.Of(kv.Value))).ToArray());

    private static JsonValue OrNull(int? v) => v is { } x ? JsonValue.Of(x) : JsonValue.Null.Instance;

    /// <summary>A listing as the golden writes it.</summary>
    internal static JsonValue ListingJson(Listing l) => JsonValue.Of(
        ("name", JsonValue.Of(l.Name)), ("volumeName", JsonValue.Of(l.VolumeName)), ("type", JsonValue.Of(EnumWire.Name(l.Type))),
        ("typeText", JsonValue.Of(l.TypeText)), ("reportedTitleCount", JsonValue.Of(l.ReportedTitleCount)),
        ("titles", JsonValue.Of(l.Titles.Select(t => JsonValue.Of(
            ("index", JsonValue.Of(t.Index)), ("sourceTitleId", OrNull(t.SourceTitleId)), ("sourceFile", JsonValue.Of(t.SourceFile)),
            ("name", JsonValue.Of(t.Name)), ("comment", JsonValue.Of(t.Comment)), ("duration", JsonValue.Of(t.Duration)),
            ("durationSeconds", JsonValue.Of(t.DurationSeconds)), ("chapters", JsonValue.Of(t.Chapters)), ("sizeBytes", JsonValue.Of(t.SizeBytes)),
            ("segmentMap", JsonValue.Of(t.SegmentMap)), ("outputFileName", JsonValue.Of(t.OutputFileName)), ("angle", OrNull(t.Angle)),
            ("tracks", JsonValue.Of(t.Tracks.Select(k => JsonValue.Of(
                ("index", JsonValue.Of(k.Index)), ("kind", JsonValue.Of(EnumWire.Name(k.Kind))), ("codec", JsonValue.Of(k.Codec)),
                ("language", JsonValue.Of(k.Language)), ("languageName", JsonValue.Of(k.LanguageName)), ("name", JsonValue.Of(k.Name)),
                ("isDefault", JsonValue.Of(k.IsDefault)), ("attributes", AttributesJson(k.Attributes)))))),
            ("attributes", AttributesJson(t.Attributes)))))),
        ("attributes", AttributesJson(l.Attributes)));

    [Fact]
    public void ListingOfARealDvd()
    {
        var golden = Json("domain/info-dvd.listing.expected.json");
        Same(golden["expect"], ListingJson(ListingOf(Text(golden["input"]!.AsString!))), "listing");
    }

    [Fact]
    public void FingerprintCases() => RunCases("domain/fingerprint.cases.json", (id, given, expect) =>
    {
        if (given["listingFile"] is { } file)
            Same(expect["fingerprint"]!.AsString, Fingerprint.Of(ListingOf(Text(file.AsString!))), "fingerprint");
        else if (given["listings"] is { } listings)
        {
            var prints = listings.AsArray!.Select(l => Fingerprint.Of(ListingOf(GeneratedListing(l)))).ToList();
            Same(expect["fingerprints"], JsonValue.Of(prints.Select(p => JsonValue.Of(p))), "fingerprints");
            Same(expect["equal"]!.AsBool, prints.Distinct().Count() == 1, "equal");
        }
        else if (given["listing"] is { } listing)
            Same(expect["fingerprint"]!.AsString, Fingerprint.Of(ListingOf(GeneratedListing(listing))), "fingerprint");
        else return false;
        return true;
    });
}
