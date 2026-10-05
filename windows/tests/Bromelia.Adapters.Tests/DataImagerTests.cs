using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Tests;

/// <summary>A disc made up on the fly: byte i is (i / 2048 + i) mod 256; some sectors can't be read.</summary>
internal sealed class FakeDisc : ISectorReader
{
    public long Sectors;
    public HashSet<long> Unreadable = new();
    public Action<int>? AfterRead;
    public int Reads;
    public bool Closed;
    public bool SizeFails;

    public static byte At(long i) => (byte)((i / 2048 + i) % 256);

    public byte[] Read(long sector, int count)
    {
        for (long s = sector; s < sector + count; s++)
            if (Unreadable.Contains(s))
                throw new BroFailure(new BroMessage(MessageCode.FsFailed, Severity.Error, ("operation", JsonValue.Of("read")),
                    ("path", JsonValue.Of("/dev/sr0")), ("reason", JsonValue.Of("I/O error"))).ToError());
        var n = (int)Math.Max(0, Math.Min(count, Sectors - sector));
        var data = new byte[n * 2048];
        for (int i = 0; i < data.Length; i++) data[i] = At(sector * 2048 + i);
        Reads++;
        AfterRead?.Invoke(Reads);
        return data;
    }

    public long SectorCount() => SizeFails
        ? throw new BroFailure(new BroMessage(MessageCode.DriveSizeUnknown, Severity.Error, ("path", JsonValue.Of("/dev/sr0")),
            ("reason", JsonValue.Of("Inappropriate ioctl for device"))).ToError())
        : Sectors;
    public void Close() => Closed = true;
}

/// <summary>Opens the fake disc (or fails as a missing drive); the rest isn't used.</summary>
internal sealed class FakeDriveControl : IDriveControl
{
    public FakeDisc? Disc;
    public bool Opened;
    public Task Eject(string device) => Task.CompletedTask;
    public Task CloseTray(string device) => Task.CompletedTask;
    public Task<string?> WaitForMount(string device, Duration timeout, CancellationToken cancel) => Task.FromResult<string?>(null);
    public DiscContent ProbeContent(string device) => DiscContent.Unknown;

    public ISectorReader OpenRaw(string device)
    {
        Opened = true;
        return Disc ?? throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.DriveNotFound)));
    }
}

/// <summary>shared/fixtures/adapters/data-imager.cases.json.</summary>
public sealed class DataImagerTests : IDisposable
{
    readonly string _dir = Path.Combine(Path.GetTempPath(), "bromelia-image-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try { Directory.Delete(_dir, true); } catch (IOException) { }
    }

    [Fact]
    public void TheSharedCasesPass()
    {
        RunCases("adapters/data-imager.cases.json", (id, given, expect) =>
        {
            if (Directory.Exists(_dir)) Directory.Delete(_dir, true);
            Directory.CreateDirectory(_dir);
            var dest = Path.Combine(_dir, "disc.iso");
            if (given["existing"]?.AsBool == true) File.WriteAllText(dest, "already here");
            var cancelSource = new CancellationSource();
            var cancel = cancelSource.Token;
            var disc = new FakeDisc { Sectors = given["sectors"]!.AsInteger!.Value, SizeFails = given["sizeFails"]?.AsBool == true };
            foreach (var s in given["unreadable"]?.AsArray ?? Array.Empty<JsonValue>()) disc.Unreadable.Add(s.AsInteger!.Value);
            if (given["cancelAfterChunks"]?.AsInteger is { } after) disc.AfterRead = n => { if (n == after) cancelSource.Cancel(); };
            var drives = new FakeDriveControl { Disc = given["openFails"]?.AsBool == true ? null : disc };
            var sink = new RecordingSink();
            try
            {
                var fs = new DiskFileSystem { SyncDirectoryFails = given["syncFails"]?.AsBool == true };
                var copy = new DataImager(drives, fs).Copy("/dev/sr0", dest, sink, cancel).GetAwaiter().GetResult();
                Assert.True(expect["error"] == null, "expected " + expect["error"]);
                Assert.Equal(expect["bytes"]!.AsInteger, copy.Bytes);
                Assert.Equal(expect["warning"]?.AsString, copy.Warning is { } w ? MessageCode.Wire(w.Code) : null);
                if (copy.Warning is { } warning) Assert.Equal(dest, warning.ToJson()["params"]!["path"]!.AsString);
            }
            catch (BroFailure f)
            {
                Assert.Equal(expect["error"]?["code"]?.AsString, f.Error.Code);
                foreach (var m in expect["error"]!["params"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                    Assert.Equal(m.Value, f.Error.Params[m.Key]);
            }
            if (expect["progress"] is { } progress)
                Assert.Equal(progress.AsArray!.Select(p => $"{p.AsArray![0].AsInteger}/{p.AsArray![1].AsInteger}/10000"),
                    sink.Events.OfType<RobotEvent.ProgressValue>().Select(p => $"{p.Current}/{p.Total}/{p.Max}"));
            if (expect["imageMatches"]?.AsBool == true)
            {
                var image = File.ReadAllBytes(dest);
                Assert.Equal(disc.Sectors * 2048, image.LongLength);
                for (long i = 0; i < image.LongLength; i++)
                    if (image[i] != FakeDisc.At(i)) Assert.Fail($"byte {i} differs");
            }
            if (expect["reads"]?.AsInteger is { } reads)
            {
                Assert.Equal(reads, disc.Reads);
                Assert.False(drives.Opened);
            }
            if (expect["closed"]?.AsBool == true) Assert.True(disc.Closed);
            Assert.Equal(expect["left"]!.AsArray!.Select(l => l.AsString!), Directory.GetFiles(_dir).Select(Path.GetFileName).OrderBy(n => n, StringComparer.Ordinal));
            return true;
        });
    }
}
