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

    [Fact]
    public void ListingOfARealDvd()
    {
        var golden = Json("domain/info-dvd.listing.expected.json");
        Same(golden["expect"], Listing.Encode(ListingOf(Text(golden["input"]!.AsString!))), "listing");
        Same(golden["expect"], Listing.Encode(Listing.Decode(golden["expect"]!)), "listing read back");
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
