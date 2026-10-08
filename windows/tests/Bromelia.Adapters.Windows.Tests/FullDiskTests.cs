using System.Runtime.Versioning;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>What a full disk does to Bromelia's writes (review follow-ups, failure testing; the same checks as Linux's
/// /full-disk/* and macOS's FullDiskTests). Opt-in: BROMELIA_TEST_FULL_DIR names an empty folder on a small volume of
/// its own (a 32 MB VHD); each test fills it around the step it checks.</summary>
[SupportedOSPlatform("windows")]
public sealed class FullDiskTests
{
    static readonly string? Dir = Environment.GetEnvironmentVariable("BROMELIA_TEST_FULL_DIR") is { Length: > 0 } d ? d : null;

    static long FreeBytes(string dir) => new DriveInfo(Path.GetPathRoot(Path.GetFullPath(dir))!).AvailableFreeSpace;

    /// <summary>Fills the volume until at most <paramref name="leave"/> bytes are free; returns the filler's path.</summary>
    static string Fill(string dir, long leave)
    {
        var filler = Path.Combine(dir, "filler.bin");
        var block = new byte[64 * 1024];
        using var f = new FileStream(filler, FileMode.Create, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough);
        try
        {
            for (long free; (free = FreeBytes(dir)) > leave;) f.Write(block, 0, (int)Math.Min(block.Length, free - leave));
        }
        catch (IOException) { } // full
        return filler;
    }

    static string[] NamesIn(string dir) =>
        Directory.EnumerateFileSystemEntries(dir).Select(Path.GetFileName).Where(n => n != "filler.bin").Order().ToArray()!;

    sealed class NoSink : IRunSink { public void Event(RobotEvent @event) { } }

    sealed class Disc : ISectorReader
    {
        public long Sectors;
        public byte[] Read(long sector, int count) => new byte[(int)Math.Max(0, Math.Min(count, Sectors - sector)) * 2048];
        public long SectorCount() => Sectors;
        public void Close() { }
    }

    sealed class Drives : IDriveControl
    {
        public Disc? Disc;
        public Task Eject(string device) => Task.CompletedTask;
        public Task CloseTray(string device) => Task.CompletedTask;
        public Task<string?> WaitForMount(string device, Duration timeout, CancellationToken cancel) => Task.FromResult<string?>(null);
        public DiscContent ProbeContent(string device) => DiscContent.Unknown;
        public ISectorReader OpenRaw(string device) => Disc!;
    }

    /// <summary>A data disc bigger than the free space: the copy fails and leaves nothing, never a partial disc.iso.</summary>
    [Fact]
    public async Task ADataDiscThatDoesntFitLeavesNothing()
    {
        if (Dir is null) return;
        var drives = new Drives { Disc = new Disc { Sectors = FreeBytes(Dir) / 2048 + 4096 } };
        var e = await Assert.ThrowsAsync<BroFailure>(() =>
            new DataImager(drives, new PlatformFileSystem()).Copy("D:", Path.Combine(Dir, "disc.iso"), new NoSink(), new CancellationSource().Token));
        Assert.Equal("fs.failed", e.Error.Code);
        Assert.Empty(NamesIn(Dir));
    }

    /// <summary>A full disk under writeAtomically: it fails, the old file is still whole, no temporary file is left.</summary>
    [Fact]
    public void WriteAtomicallyKeepsTheOldFile()
    {
        if (Dir is null) return;
        var fs = new PlatformFileSystem();
        var path = Path.Combine(Dir, "r.json");
        fs.WriteAtomically(path, "old"u8.ToArray(), 0x1A4);
        var filler = Fill(Dir, 64 * 1024);
        try
        {
            Assert.Throws<BroFailure>(() => fs.WriteAtomically(path, new byte[1 << 20], 0x1A4));
            Assert.Equal("old", File.ReadAllText(path));
            Assert.Equal(new[] { "r.json" }, NamesIn(Dir));
        }
        finally { File.Delete(filler); File.Delete(path); }
    }

    /// <summary>A transcript that fills the disk: the tool runs to the end, every line still reaches the reader, and the
    /// run reports process.noTranscript.</summary>
    [Fact]
    public async Task ATranscriptThatFillsTheDiskIsReported()
    {
        if (Dir is null) return;
        var filler = Fill(Dir, 32 * 1024);
        var transcript = Path.Combine(Dir, "t.txt");
        try
        {
            var spec = new ProcessSpec("cmd.exe", new[] { "/c", "for /L %i in (1,1,20000) do @echo line-%i-0123456789012345678901234567890123456789" },
                new Dictionary<string, string>(), null, StopPolicy.InterruptFirst, Transcript: transcript);
            var p = new PlatformProcessLauncher().Start(spec);
            var lines = 0;
            var read = Task.Run(async () => { await foreach (var _ in p.Lines()) lines++; });
            var exit = await p.Wait();
            await read;
            Assert.Equal(0, exit.Status);
            Assert.Equal(20000, lines);
            Assert.Equal(MessageCode.ProcessNoTranscript, p.TranscriptProblem()?.Code);
        }
        finally { File.Delete(filler); File.Delete(transcript); }
    }

    /// <summary>The database on a full disk: a write fails with store.failed, and once there is space again the
    /// database opens, is intact, and still has what was written before.</summary>
    [Fact]
    public async Task TheDatabaseSurvivesAFullDisk()
    {
        if (Dir is null) return;
        var path = Path.Combine(Dir, "s.sqlite");
        var store = SqliteStore.Open(path, new SystemClock());
        await store.Migrate();
        await store.Kv().Set("before", JsonValue.Of("kept"));
        var filler = Fill(Dir, 48 * 1024);
        BroFailure? failure = null;
        try
        {
            var pad = new string('x', 8000);
            for (var i = 0; i < 2000 && failure is null; i++)
                try { await store.Kv().Set("k" + i, JsonValue.Of(pad)); }
                catch (BroFailure e) { failure = e; }
        }
        finally { store.Close(); File.Delete(filler); }
        Assert.Equal("store.failed", failure?.Error.Code);
        using (var reopened = SqliteStore.Open(path, new SystemClock()))
            Assert.Equal(JsonValue.Of("kept"), await reopened.Kv().Get("before"));
        using (var db = new Microsoft.Data.Sqlite.SqliteConnection("Data Source=" + path))
        {
            db.Open();
            using var cmd = db.CreateCommand();
            cmd.CommandText = "PRAGMA integrity_check";
            Assert.Equal("ok", cmd.ExecuteScalar());
        }
        Microsoft.Data.Sqlite.SqliteConnection.ClearAllPools();
        foreach (var f in Directory.GetFiles(Dir)) File.Delete(f);
    }
}
