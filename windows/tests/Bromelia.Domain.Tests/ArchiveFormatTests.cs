using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Tests;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

public class ArchiveFormatTests
{
    private static JsonValue Strings(IEnumerable<string> s) => JsonValue.Of(s.Select(JsonValue.Of));

    private static List<SumEntry> EntriesOf(JsonValue v) =>
        v.AsArray!.Select(e => new SumEntry(e["path"]!.AsString!, e["sha256"]!.AsString!)).ToList();

    private static JsonValue EntriesJson(IEnumerable<SumEntry> e) =>
        JsonValue.Of(e.Select(x => JsonValue.Of(("path", JsonValue.Of(x.Path)), ("sha256", JsonValue.Of(x.Sha256)))));

    [Fact]
    public void ArchiveFormatCases() => RunCases("domain/archive-format.cases.json", (id, given, expect) =>
    {
        if (given["bytes"] is { } bytes) Same(expect["sha256"]!.AsString, Sha256Sums.Hash(Encoding.UTF8.GetBytes(bytes.AsString!)), "sha256");
        else if (given["existing"] is { } existing) Same(expect["text"]!.AsString, Sha256Sums.Merge(existing.AsString!, EntriesOf(given["entries"]!)), "merged");
        else if (given["entries"] is { } entries) Same(expect["text"]!.AsString, Sha256Sums.Render(EntriesOf(entries)), "text");
        else if (given["text"] is { } text) Same(expect["entries"], EntriesJson(Sha256Sums.Parse(text.AsString!)), "entries");
        else if (given["produced"] is { } produced)
            Same(expect["relative"], Strings(ArchiveFiles.Expand(produced.AsArray!.Select(p => p.AsString!).ToList(),
                given["tree"]!.AsArray!.Select(p => p.AsString!).ToList())), "relative");
        else if (given["name"] is { } name)
        {
            if (expect["isOwnFile"] is { } own) Same(own.AsBool, ArchiveFiles.IsOwnFile(name.AsString!), "own file");
            else Same(expect["isMetadataFile"]!.AsBool, ArchiveFiles.IsMetadataFile(name.AsString!), "metadata file");
        }
        else if (given["sums"] is { } sums)
        {
            var hashes = given["hashes"]!.AsObject!.ToDictionary(m => m.Key, m => EnumWire.Parse<FileVerdict>(m.Value.AsString)!.Value);
            var r = VerifyResult.Compare(sums.AsArray!.Select(s => s.AsString!).ToList(), hashes, given["folder"]!.AsArray!.Select(f => f.AsString!).ToList());
            Same(expect["result"]!.AsString, EnumWire.Name(r.Result), "result");
            if (expect["error"] is { } error) Same(error, r.Summary.ToJson(), "error");
            else
            {
                Same(expect["files"]!.AsInteger, (long?)r.Files, "files");
                foreach (var (key, list) in new[] { ("changed", r.Changed), ("missing", r.Missing), ("unreadable", r.Unreadable), ("unlisted", r.Unlisted) })
                    if (expect[key] is { } want) Same(want, Strings(list), key);
                Same(expect["summary"], r.Summary.ToJson(), "summary");
                Same(expect["summaryText"]!.AsString, English.Render(r.Summary.ToJson()), "summary text");
            }
        }
        else return false;
        return true;
    });

    [Fact]
    public void Version3RecordsRoundTrip()
    {
        foreach (var name in new[] { "archive/record3-movie-read-errors.json", "archive/record3-tv-play-all.json" })
        {
            var bytes = File.ReadAllBytes(Path_(name));
            var record = ArchiveRecordCodec.Decode(bytes)!;
            Assert.Equal(3, record.Version);
            Assert.Equal(Encoding.UTF8.GetString(bytes), Encoding.UTF8.GetString(ArchiveRecordCodec.EncodeV3(record)));
            // Keys out of order are written in the schema's order.
            var shuffled = (JsonValue.Object)record.Document;
            var reversed = record with { Document = new JsonValue.Object(shuffled.Members.Reverse().ToList()) };
            Assert.Equal(Encoding.UTF8.GetString(bytes), Encoding.UTF8.GetString(ArchiveRecordCodec.EncodeV3(reversed)));
        }
        var movie = ArchiveRecordCodec.Decode(File.ReadAllBytes(Path_("archive/record3-movie-read-errors.json")))!;
        Assert.Equal("errors", movie.Status);
        Assert.Equal("a1b2c3d4-0000-4000-8000-00000000abcd", movie.UnitId);
        Assert.Null(ArchiveRecordCodec.ArchivedDisc(movie, "x"));
        var tv = ArchiveRecordCodec.Decode(File.ReadAllBytes(Path_("archive/record3-tv-play-all.json")))!;
        var disc = ArchiveRecordCodec.ArchivedDisc(tv, "Shows/X")!;
        Assert.Equal(tv.Episodes.Max(), disc.LastEpisode);
    }

    [Fact]
    public void Version2RecordsAreRead()
    {
        var r = ArchiveRecordCodec.Decode(File.ReadAllBytes(Path_("archive/record2-sample-movie.json")))!;
        Assert.Equal(2, r.Version);
        Assert.Equal(("success", "Sample Movie", "movie", "SAMPLE_MOVIE"), (r.Status, r.Name, r.Kind, r.Label));
        Assert.Equal("v1:1111111111111111aaaaaaaaaaaaaaaa", r.Fingerprint);
        Assert.Single(r.Files);
        Assert.Null(r.UnitId);
        Assert.Throws<ArgumentException>(() => ArchiveRecordCodec.EncodeV3(r));
        Assert.Null(ArchiveRecordCodec.Decode(Encoding.UTF8.GetBytes("{\"format\": \"other\"}")));
        Assert.Null(ArchiveRecordCodec.Decode(Encoding.UTF8.GetBytes("{\"format\": \"bromelia-archive\", \"version\": 4}")));
    }

    [Fact]
    public void NotesSayWhyTheFilesAreNotAnArchive()
    {
        var lines = Notes.Render(new Id("b2c3d4e5-0000-4000-8000-00000000abcd"), Outcome.SucceededWithReadErrors,
            new BroError("rip.readErrors", ("count", JsonValue.Of(1))),
            new List<RobotMessage> { new(2003, 516, 0, "Error 'Scsi error - MEDIUM ERROR' occurred while reading", "", new List<string>()) },
            new List<string> { "bromelia-a1b2c3d4-log.txt", "makemkv-a1b2c3d4-log.txt" });
        var text = string.Join("\n", lines.Select(l => English.Render(l.ToJson())));
        Assert.Equal(
            "Bromelia job b2c3d4e5-0000-4000-8000-00000000abcd: Completed with read errors.\n" +
            "These files are NOT a finished archive. Rip the disc again (clean it first if it has read errors),\nor check the files yourself before using them.\n" +
            "MakeMKV reported 1 read error while reading the disc, so the files may be damaged. They were kept apart from finished archives.\n" +
            "Errors reported by MakeMKV while reading the disc:\n" +
            "Error 'Scsi error - MEDIUM ERROR' occurred while reading\n" +
            "The job's log is in this folder as bromelia-a1b2c3d4-log.txt, and everything MakeMKV printed as makemkv-a1b2c3d4-log.txt.", text);
    }
}
