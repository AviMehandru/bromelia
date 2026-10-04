using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Adapters.Tests;

/// <summary>shared/fixtures/adapters/video-ts-byte-source.cases.json, and the real DVD ISO when BROMELIA_TEST_DVD_ISO
/// is set.</summary>
public sealed class VideoTsByteSourceTests
{
    /// <summary>What DvdNav.analyse found, in short: each title's number and chapter count, and the jumps.</summary>
    static string Summary(IByteSource source)
    {
        var a = DvdNav.Analyse(source);
        if (a is null) return "none";
        return string.Join(",", a.Titles.Select(t => $"{t.Number}:{t.Chapters.Count}")) + " / " + string.Join(",", a.Jumps.Select(j => $"{j.Title}.{j.Chapter} {j.How}"));
    }

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/video-ts-byte-source.cases.json", (id, given, expect) =>
        {
            using var source = VideoTsByteSource.Open(Path_(given["open"]!.AsString!));
            if (expect["opens"]?.AsBool == false)
            {
                Assert.Null(source);
                return true;
            }
            Assert.NotNull(source);
            Assert.Equal(expect["label"]!.AsString, source!.Label());
            if (expect["files"] is { } files)
                Assert.Equal(files.AsArray!.Select(f => $"{f.AsArray![0].AsString} {f.AsArray![1].AsInteger}"), source.Files().Select(f => $"{f.Name} {f.Size}"));
            foreach (var r in expect["reads"]?.AsArray ?? Array.Empty<JsonValue>())
            {
                var bytes = source.Read(r["file"]!.AsString!, r["offset"]!.AsInteger!.Value, (int)r["length"]!.AsInteger!.Value);
                if (r["text"]?.AsString is { } text) Assert.Equal(text, Encoding.ASCII.GetString(bytes));
                if (r["hex"]?.AsString is { } hex) Assert.Equal(hex, Convert.ToHexString(bytes).ToLowerInvariant());
                if (r["count"]?.AsInteger is { } count) Assert.Equal(count, bytes.Length);
            }
            if (expect["sameAnalysisAs"]?.AsString is { } other)
            {
                using var folder = VideoTsByteSource.Open(Path_(other))!;
                var mine = Summary(source);
                Assert.NotEqual("none", mine);
                Assert.Equal(Summary(folder), mine);
            }
            return true;
        });
    }

    [Fact]
    public void TheRealDvdIsoReadsLikeItsFiles()
    {
        if (Environment.GetEnvironmentVariable("BROMELIA_TEST_DVD_ISO") is not { Length: > 0 } iso) return;
        using var source = VideoTsByteSource.Open(iso);
        Assert.NotNull(source);
        Assert.Contains(source!.Files(), f => f.Name == "VIDEO_TS.IFO");
        Assert.Equal("DVDVIDEO-VMG", Encoding.ASCII.GetString(source.Read("VIDEO_TS.IFO", 0, 12)));
        Assert.NotEqual("none", Summary(source));
    }
}
