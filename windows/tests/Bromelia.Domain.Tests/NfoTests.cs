using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class NfoTests
{
    [Fact]
    public void NfoCases()
    {
        foreach (var c in Json("nfo/nfo-cases.json")["cases"]!.AsArray!)
        {
            var want = Text("nfo/" + c["nfo"]!.AsString!);
            bool tv = c["kind"]?.AsString == "tv";
            string got;
            if (c["seasonFile"] is { } seasonFile)
            {
                var bytes = File.ReadAllBytes(Path_("lookup/" + seasonFile.AsString!));
                var season = c["provider"]!.AsString == "tmdb" ? TmdbParse.Season(bytes) : OmdbParse.Season(bytes);
                int e = (int)c["episode"]!.AsInteger!;
                got = Nfo.Episode(c["show"]!.AsString!, (int)c["season"]!.AsInteger!, e, season[e]);
            }
            else
            {
                var kind = tv ? MediaKind.Tv : MediaKind.Movie;
                Candidate match;
                if (c["details"] is { } details)
                {
                    var bytes = File.ReadAllBytes(Path_("lookup/" + details.AsString!));
                    match = (c["provider"]!.AsString == "tmdb" ? TmdbParse.Details(bytes, kind) : OmdbParse.Details(bytes, kind))!;
                }
                else match = new Candidate(c["match"]!["title"]!.AsString!, null, null, null, "");
                got = tv ? Nfo.Show(match) : Nfo.Movie(match);
            }
            Same(want, got, c["nfo"]!.AsString!);
        }
    }
}
