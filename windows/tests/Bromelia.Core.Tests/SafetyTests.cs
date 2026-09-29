using System.Text.Json;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Bromelia.Core.Logic;
using Bromelia.Core.Robot;
using Xunit;

namespace Bromelia.Core.Tests;

// Tests for the safeguards that keep damaged, incomplete or wrong rips out of the archive:
// read errors, staging and marked folders, checks against the disc listing, and disc changes.

static class Listing
{
    /// <summary>A disc listing in makemkvcon's robot format. Each title is (duration, source title id).</summary>
    public static string Make(params (string Duration, int Source)[] titles) => Make("SAMPLE_MOVIE", titles);

    public static string Make(string volume, params (string Duration, int Source)[] titles)
    {
        var s = "MSG:1005,0,1,\"MakeMKV v1.18.1 started\",\"%1 started\",\"MakeMKV v1.18.1\"\n" +
                $"TCOUNT:{titles.Length}\nCINFO:1,6209,\"Blu-ray disc\"\nCINFO:2,0,\"{volume}\"\nCINFO:32,0,\"{volume}\"\n";
        for (int i = 0; i < titles.Length; i++)
        {
            var (d, src) = titles[i];
            s += $"TINFO:{i},8,0,\"2\"\nTINFO:{i},9,0,\"{d}\"\nTINFO:{i},16,0,\"0000{src}.mpls\"\nTINFO:{i},24,0,\"{src}\"\n" +
                 $"TINFO:{i},26,0,\"{src}\"\nTINFO:{i},27,0,\"title_t0{i}.mkv\"\nSINFO:{i},0,1,6201,\"Video\"\nSINFO:{i},1,1,6202,\"Audio\"\n";
        }
        return s + "MSG:5011,0,0,\"Operation successfully completed\",\"Operation successfully completed\"\n";
    }

    public static DiscInfo Info(string text)
    {
        var b = new DiscInfoBuilder();
        foreach (var line in text.Split('\n')) if (RobotParser.Parse(line) is { } ev) b.Consume(ev);
        return b.Info;
    }

    public const string ReadError = "MSG:2003,0,3,\"Error 'Scsi error - MEDIUM ERROR:L-EC UNCORRECTABLE ERROR' occurred while reading '/BDMV/STREAM/00001.m2ts' at offset '1048576'\",\"Error '%1' occurred while reading '%2' at offset '%3'\",\"Scsi error - MEDIUM ERROR:L-EC UNCORRECTABLE ERROR\",\"/BDMV/STREAM/00001.m2ts\",\"1048576\"";
    public const string Saved = "MSG:5036,260,1,\"Copy complete. 1 titles saved.\",\"Copy complete. %1 titles saved.\",\"1\"";
    public const string FailedSave =
        "MSG:5003,0,2,\"Failed to save title 1 to file title_t01.mkv\",\"Failed to save title %1 to file %2\",\"1\",\"title_t01.mkv\"\n" +
        "MSG:5037,516,2,\"Copy complete. 0 titles saved, 1 failed.\",\"Copy complete. %1 titles saved, %2 failed.\",\"0\",\"1\"";
}

/// <summary>A stand-in for makemkvcon (POSIX shell): <c>info</c> prints a listing, <c>mkv</c> runs the scenario's shell
/// code with <c>$title</c> and <c>$dest</c> set. Every call is recorded in calls.txt.</summary>
sealed class FakeMakeMkv
{
    public string Dir { get; } = Path.Combine(Path.GetTempPath(), "bromelia-fake-" + Guid.NewGuid().ToString("N")[..8]);
    public string Executable => Path.Combine(Dir, "makemkvcon");
    public string Calls => File.Exists(Path.Combine(Dir, "calls.txt")) ? File.ReadAllText(Path.Combine(Dir, "calls.txt")) : "";

    public FakeMakeMkv(string listing, string mkv, string backup = "")
    {
        Directory.CreateDirectory(Dir);
        var listingPath = Path.Combine(Dir, "listing.txt");
        File.WriteAllText(listingPath, listing);
        mkv = mkv.Replace("__LISTING__", listingPath);
        File.WriteAllText(Executable,
            "#!/bin/sh\n" +
            $"echo \"$@\" >> '{Dir}/calls.txt'\n" +
            "while [ $# -gt 0 ]; do\n  case \"$1\" in info|mkv|backup) cmd=\"$1\"; shift; break;; esac\n  shift\ndone\n" +
            "case \"$cmd\" in\n" +
            $"  info) cat '{listingPath}' ;;\n" +
            "  mkv) title=\"$2\"; dest=\"$3\"\n" + mkv + "\n  ;;\n" +
            "  backup) for a in \"$@\"; do dest=\"$a\"; done\n" + backup + "\n  ;;\nesac\nexit 0\n");
        // Tests that never start makemkvcon (data discs) also run on Windows, which has no Unix file modes.
        if (!OperatingSystem.IsWindows())
            File.SetUnixFileMode(Executable, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
    }

    /// <summary>Writes a small file as MakeMKV's output for the title (for <c>all</c>: titles 0-4 in the listing), then prints <paramref name="messages"/>.</summary>
    public static string WritesFile(string messages) =>
        "if [ \"$title\" = \"all\" ]; then\n" +
        "  for t in 0 1 2 3 4; do grep -q \"title_t0$t.mkv\" '__LISTING__' && printf 'mkv data' > \"$dest/title_t0$t.mkv\"; done\n" +
        "else\n  printf 'mkv data %s' \"$title\" > \"$dest/title_t0$title.mkv\"\nfi\n" +
        "cat <<'EOF'\n" + messages + "\nEOF";
}

public class SafetyTests
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-safety-" + Guid.NewGuid().ToString("N")[..8]);

    RipJob Run(FakeMakeMkv fake, List<int>? titles, DiscInfo? opened = null, string? mkvmerge = null, Action<DriveConfig>? configure = null)
    {
        Directory.CreateDirectory(_root);
        Paths.DataOverride = Path.Combine(Path.GetTempPath(), "bromelia-test-data");
        var config = new AppConfig { OutputRoot = _root };
        var drive = new DriveConfig { Name = "Test drive" };
        drive.Automation.Notify = false;
        drive.Rip.TitleSelection.SkipDuplicates = false;
        configure?.Invoke(drive);
        var job = new RipJob(new DiscSource.Iso("/nonexistent/test.iso"), drive, "iso:test", "test", "", RipMode.Mkv)
        {
            ManualTitles = titles,
            PreloadedInfo = opened,
        };
        SingleThreadContext.Run(async () =>
            await new JobRunner(job, config, fake.Executable, mkvmerge, new UiDispatcher(), new NullPlatformServices(), new[] { "dvd_MinimumTitleLength" }).RunAsync());
        return job;
    }

    List<string> RootItems => JobRunner.VisibleItems(_root);
    static List<string> AllItems(string dir) => Directory.Exists(dir)
        ? Directory.EnumerateFileSystemEntries(dir).Select(e => Path.GetFileName(e)!).OrderBy(n => n, StringComparer.Ordinal).ToList()
        : new List<string>();

    static JsonElement Record(string dir) => JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "bromelia.json"))).RootElement;

    [Fact]
    public void SuccessfulRipIsMovedIntoTheOutputFolder()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1), ("0:00:20", 2)), FakeMakeMkv.WritesFile(Listing.Saved));
        var job = Run(fake, new() { 0, 1 });
        Assert.True(job.State == JobState.Succeeded, job.ErrorMessage);
        var outDir = job.OutputDirectory!;
        Assert.Equal("Sample Movie", Path.GetFileName(outDir));
        Assert.Equal(new[] { "Sample Movie" }, RootItems);
        var items = AllItems(outDir);
        Assert.DoesNotContain(items, n => n.StartsWith(".bromelia", StringComparison.Ordinal));
        Assert.Contains("SHA256SUMS", items);
        Assert.Equal(2, job.ProducedFiles.Count);
        Assert.All(job.ProducedFiles, f => Assert.Equal(outDir, Path.GetDirectoryName(f)));
        Assert.Empty(Checksums.Verify(outDir));
        Assert.Equal("success", Record(outDir).GetProperty("status").GetString());
    }

    [Fact]
    public void ReadErrorsKeepTheFilesApart()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1)), FakeMakeMkv.WritesFile(Listing.ReadError + "\n" + Listing.Saved));
        var job = Run(fake, new() { 0 });
        Assert.Equal(JobState.CompletedWithErrors, job.State);
        Assert.Single(job.DataErrors);
        Assert.Equal(new[] { "Sample Movie [READ ERRORS]" }, RootItems);
        var outDir = job.OutputDirectory!;
        var items = AllItems(outDir);
        Assert.Contains("READ ERRORS.txt", items);
        Assert.Contains("SHA256SUMS", items);
        Assert.Single(items, n => n.EndsWith(".mkv", StringComparison.Ordinal));
        Assert.Contains("MEDIUM ERROR", File.ReadAllText(Path.Combine(outDir, "READ ERRORS.txt")));
        var record = Record(outDir);
        Assert.Equal("errors", record.GetProperty("status").GetString());
        Assert.Equal(1, record.GetProperty("readErrors").GetArrayLength());
    }

    [Fact]
    public void ListingErrorsAreNotReadErrors()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.ReadError + "\n" + Listing.Make(("0:00:10", 1)), FakeMakeMkv.WritesFile(Listing.Saved));
        var job = Run(fake, new() { 0 });
        Assert.True(job.State == JobState.Succeeded, job.ErrorMessage);
        Assert.Empty(job.DataErrors);
    }

    [Fact]
    public void FailedTitleIsKeptUnderMakeMkvsNameInAnIncompleteFolder()
    {
        if (OperatingSystem.IsWindows()) return;
        var mkv = "printf 'partial' > \"$dest/title_t0$title.mkv\"\n" +
                  $"if [ \"$title\" = \"1\" ]; then cat <<'EOF'\n{Listing.FailedSave}\nEOF\nelse cat <<'EOF'\n{Listing.Saved}\nEOF\nfi";
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1), ("0:00:20", 2), ("0:00:30", 3)), mkv);
        var job = Run(fake, new() { 0, 1 });
        Assert.Equal(JobState.Failed, job.State);
        Assert.Equal(new[] { "Sample Movie [INCOMPLETE]" }, RootItems);
        var items = AllItems(job.OutputDirectory!);
        Assert.Contains("INCOMPLETE.txt", items);
        Assert.Contains("title_t01.mkv", items);
        Assert.DoesNotContain("SHA256SUMS", items);
        Assert.DoesNotContain(items, n => n.StartsWith(".bromelia", StringComparison.Ordinal));
    }

    [Fact]
    public void FailureWithoutFilesLeavesNoFolder()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1)), $"cat <<'EOF'\n{Listing.FailedSave}\nEOF");
        var job = Run(fake, new() { 0 });
        Assert.Equal(JobState.Failed, job.State);
        Assert.Empty(AllItems(_root));
        Assert.Null(job.OutputDirectory);
    }

    [Fact]
    public void FailureInASharedFolderUsesASubfolder()
    {
        if (OperatingSystem.IsWindows()) return;
        Directory.CreateDirectory(_root);
        File.WriteAllText(Path.Combine(_root, "other.mkv"), "keep");
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1)), FakeMakeMkv.WritesFile(Listing.FailedSave));
        var job = Run(fake, new() { 0 }, configure: d => { d.Output.FolderTemplate = ""; d.Output.ConflictPolicy = ConflictPolicy.Overwrite; });
        Assert.Equal(JobState.Failed, job.State);
        var items = RootItems;
        Assert.Equal(2, items.Count);
        Assert.Contains("other.mkv", items);
        var sub = Assert.Single(items, n => n.StartsWith("INCOMPLETE - ", StringComparison.Ordinal));
        Assert.Contains("title_t00.mkv", AllItems(Path.Combine(_root, sub)));
    }

    [Fact]
    public void ADifferentDiscIsNotRipped()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1)), FakeMakeMkv.WritesFile(Listing.Saved));
        var job = Run(fake, new() { 0 }, Listing.Info(Listing.Make("OTHER_DISC", ("0:00:10", 1))));
        Assert.Equal(JobState.Failed, job.State);
        Assert.Contains("not the disc that was opened", job.ErrorMessage);
        Assert.DoesNotContain(" mkv ", fake.Calls);
        Assert.Empty(AllItems(_root));
    }

    [Fact]
    public void ChosenTitlesFollowChangedTitleNumbers()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.Make(("0:00:05", 9), ("0:00:10", 1), ("0:00:20", 2)), FakeMakeMkv.WritesFile(Listing.Saved));
        var job = Run(fake, new() { 1 }, Listing.Info(Listing.Make(("0:00:10", 1), ("0:00:20", 2))));
        Assert.True(job.State == JobState.Succeeded, job.ErrorMessage);
        Assert.Equal(new List<int> { 2 }, job.ManualTitles);
        Assert.Contains("mkv iso:/nonexistent/test.iso 2 ", fake.Calls);
    }

    [Fact]
    public void AChosenTitleMissingFromTheListingFailsTheJob()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1)), FakeMakeMkv.WritesFile(Listing.Saved));
        var job = Run(fake, new() { 1 }, Listing.Info(Listing.Make(("0:00:10", 1), ("0:00:20", 2))));
        Assert.Equal(JobState.Failed, job.State);
        Assert.Contains("not in the new disc listing", job.ErrorMessage);
        Assert.DoesNotContain(" mkv ", fake.Calls);
    }

    [Fact]
    public void RipsAreCheckedAgainstTheListing()
    {
        if (OperatingSystem.IsWindows()) return;
        var ffmpeg = Paths.ResolveTool("", new[] { "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg" }, "ffmpeg");
        var mkvmerge = Paths.ResolveTool("", Paths.MkvmergeCandidates(), "mkvmerge");
        if (ffmpeg == null || mkvmerge == null) return;
        string Sample(int seconds)
        {
            var path = Path.Combine(Path.GetTempPath(), $"bromelia-sample-{seconds}-{Guid.NewGuid().ToString("N")[..6]}.mkv");
            var r = new ProcessRunner(ffmpeg, new[] { "-v", "error", "-y", "-f", "lavfi", "-i", $"testsrc=size=64x48:rate=5:duration={seconds}",
                "-f", "lavfi", "-i", $"sine=duration={seconds}", "-c:v", "mpeg4", "-c:a", "aac", path }).RunAsync(_ => { }).GetAwaiter().GetResult();
            Assert.Equal(0, r.ExitCode);
            return path;
        }
        var good = Sample(10);
        var shortFile = Sample(3);
        var mkv = $"if [ \"$title\" = \"0\" ]; then cp '{good}' \"$dest/title_t00.mkv\"; else cp '{shortFile}' \"$dest/title_t01.mkv\"; fi\n" +
                  $"cat <<'EOF'\n{Listing.Saved}\nEOF";
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1), ("0:00:20", 2), ("0:00:30", 3)), mkv);
        var job = Run(fake, new() { 0, 1 }, mkvmerge: mkvmerge);
        Assert.Equal(JobState.Failed, job.State);
        Assert.Contains("doesn't match the disc listing", job.ErrorMessage);
        Assert.Contains(job.Log, l => l.Text.StartsWith("Checked ", StringComparison.Ordinal));
        Assert.Equal(new[] { "Sample Movie [INCOMPLETE]" }, RootItems);
        Assert.Contains("title_t01.mkv", AllItems(Path.Combine(_root, "Sample Movie [INCOMPLETE]")));
    }
}

public class RipCheckTests
{
    static TitleInfo Title(string duration, int chapters = 0, params string[] tracks)
    {
        var t = new TitleInfo { Index = 0 };
        t.Attributes[(int)AttributeId.Duration] = duration;
        t.Attributes[(int)AttributeId.ChapterCount] = chapters.ToString();
        foreach (var (type, i) in (tracks.Length == 0 ? new[] { "Video", "Audio" } : tracks).Select((x, i) => (x, i)))
            t.Tracks.Add(new TrackInfo { Index = i, Attributes = { [(int)AttributeId.Type] = type } });
        return t;
    }

    static RipVerifier.Probe P(double? d, int chapters, params string[] types) => new(d, types.ToList(), chapters);

    [Fact]
    public void ParsesMkvmergeJson()
    {
        var p = RipVerifier.Parse("{\"container\":{\"recognized\":true,\"properties\":{\"duration\":7212345000000}},\"tracks\":[{\"id\":0,\"type\":\"video\"},{\"id\":1,\"type\":\"audio\"},{\"id\":2,\"type\":\"subtitles\"}],\"chapters\":[{\"num_entries\":24}]}");
        Assert.NotNull(p);
        Assert.Equal(new[] { "video", "audio", "subtitles" }, p!.TrackTypes);
        Assert.Equal(24, p.ChapterCount);
        Assert.Equal(7212.345, p.DurationSeconds!.Value, 3);
        Assert.Null(RipVerifier.Parse("{\"container\":{\"recognized\":false}}"));
    }

    [Fact]
    public void AcceptsMatchingFilesAndRejectsWrongOnes()
    {
        var r = RipVerifier.Check(P(7212.4, 25, "video", "audio"), Title("2:00:10", 24));
        Assert.Empty(r.Problems);
        Assert.Empty(r.Notes);
        Assert.NotEmpty(RipVerifier.Check(P(5400, 24, "video", "audio"), Title("2:00:10")).Problems);
        Assert.NotEmpty(RipVerifier.Check(P(null, 0, "video"), Title("2:00:10")).Problems);
        Assert.NotEmpty(RipVerifier.Check(P(7210, 0), Title("2:00:10")).Problems);
        Assert.NotEmpty(RipVerifier.Check(P(7210, 0, "audio"), Title("2:00:10")).Problems);
        Assert.Empty(RipVerifier.Check(P(7240, 0, "video"), Title("2:00:10")).Problems);
        Assert.NotEmpty(RipVerifier.Check(P(7260, 0, "video"), Title("2:00:10")).Problems);
        var chapters = RipVerifier.Check(P(600, 3, "video"), Title("0:10:00", 12));
        Assert.Empty(chapters.Problems);
        Assert.Single(chapters.Notes);
    }

    [Fact]
    public void MatchesTitlesAcrossListings()
    {
        var old = Listing.Info(Listing.Make(("1:00:00", 1), ("0:20:00", 2)));
        var now = Listing.Info(Listing.Make(("0:01:00", 7), ("1:00:00", 1), ("0:20:00", 2)));
        Assert.Equal(new Dictionary<int, int> { [0] = 1, [1] = 2 }, ListingMatcher.Map(new[] { 0, 1 }, old, now, new HashSet<int>()));
        Assert.Throws<JobException>(() => ListingMatcher.Map(new[] { 0 }, old, new DiscInfo(), new HashSet<int>()));
        now.Titles[1].Tracks.Add(new TrackInfo { Index = 2, Attributes = { [(int)AttributeId.Type] = "Subtitles" } });
        Assert.Throws<JobException>(() => ListingMatcher.Map(new[] { 0 }, old, now, new HashSet<int> { 0 }));
        Assert.Null(ListingMatcher.DifferentDisc(old, now));
        Assert.NotNull(ListingMatcher.DifferentDisc(old, Listing.Info(Listing.Make("OTHER", ("1:00:00", 1)))));
        Assert.NotNull(ListingMatcher.DifferentDisc(Listing.Info(Listing.Make("", ("1:00:00", 1))), Listing.Info(Listing.Make("", ("0:45:00", 3)))));
    }

    [Fact]
    public void ChecksBackupStructure()
    {
        var dir = Path.Combine(Path.GetTempPath(), "bromelia-backup-" + Guid.NewGuid().ToString("N")[..6]);
        try
        {
            var bd = Path.Combine(dir, "bd");
            Directory.CreateDirectory(Path.Combine(bd, "BDMV"));
            Assert.NotNull(BackupVerifier.Problem(bd, false));
            File.WriteAllBytes(Path.Combine(bd, "BDMV", "index.bdmv"), Array.Empty<byte>());
            Assert.Null(BackupVerifier.Problem(bd, false));
            var dvd = Path.Combine(dir, "dvd");
            Directory.CreateDirectory(Path.Combine(dvd, "VIDEO_TS"));
            File.WriteAllBytes(Path.Combine(dvd, "VIDEO_TS", "VIDEO_TS.IFO"), Array.Empty<byte>());
            Assert.Null(BackupVerifier.Problem(dvd, false));
            Assert.NotNull(BackupVerifier.Problem(Path.Combine(dir, "missing"), false));
            var folderIso = Path.Combine(dir, "folder.iso");
            Directory.CreateDirectory(folderIso);
            Assert.NotNull(BackupVerifier.Problem(folderIso, true));
            var image = new byte[40_000];
            "BEA01"u8.ToArray().CopyTo(image, 32769);
            var iso = Path.Combine(dir, "disc.iso");
            File.WriteAllBytes(iso, image);
            Assert.Null(BackupVerifier.Problem(iso, true));
            File.WriteAllBytes(iso, new byte[40_000]);
            Assert.NotNull(BackupVerifier.Problem(iso, true));
        }
        finally { try { Directory.Delete(dir, true); } catch (IOException) { } }
    }

    [Fact]
    public void StepsForJobsWithReadErrors()
    {
        var step = new PostProcessStep { Executable = "/bin/true", RunOn = RunCondition.Success };
        Assert.False(PostProcessor.ShouldRun(step, JobState.CompletedWithErrors));
        step.RunOn = RunCondition.Failure;
        Assert.True(PostProcessor.ShouldRun(step, JobState.CompletedWithErrors));
        Assert.Equal("errors", JobState.CompletedWithErrors.StatusWord());
        Assert.True(JobState.CompletedWithErrors.IsFinished());
    }
}

/// <summary>shared/fixtures/rip-outcomes.json: how a job must end for each recorded makemkvcon run. The same file is
/// replayed by the macOS and Linux tests, so the three implementations can't drift apart.</summary>
public class RecordedRunTests
{
    sealed class Outcomes
    {
        public List<Case> Cases { get; set; } = new();
    }

    sealed class Case
    {
        public string Fixture { get; set; } = "";
        public int ExitCode { get; set; }
        public bool WritesFile { get; set; }
        public string Status { get; set; } = "";
        public string? Error { get; set; }
        public bool KeptApart { get; set; }
    }

    static string FixturePath(string name) => Path.Combine(AppContext.BaseDirectory, "fixtures", name);

    [Fact]
    public void EveryRecordedRunEndsAsExpected()
    {
        if (OperatingSystem.IsWindows()) return;
        var outcomes = JsonSerializer.Deserialize<Outcomes>(File.ReadAllText(FixturePath("rip-outcomes.json")),
            new JsonSerializerOptions { PropertyNameCaseInsensitive = true })!;
        Assert.True(outcomes.Cases.Count >= 7);
        foreach (var c in outcomes.Cases)
        {
            var root = Path.Combine(Path.GetTempPath(), "bromelia-rec-" + Guid.NewGuid().ToString("N")[..8]);
            Directory.CreateDirectory(root);
            Paths.DataOverride = Path.Combine(Path.GetTempPath(), "bromelia-test-data");
            var mkv = (c.WritesFile ? "printf 'mkv data' > \"$dest/title_t0$title.mkv\"\n" : "") +
                      $"cat '{FixturePath(c.Fixture + ".txt")}'\nexit {c.ExitCode}";
            var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1), ("0:00:20", 2)), mkv);
            var config = new AppConfig { OutputRoot = root };
            var drive = new DriveConfig();
            drive.Automation.Notify = false;
            var job = new RipJob(new DiscSource.Iso("/nonexistent/test.iso"), drive, "iso:rec", "test", "", RipMode.Mkv) { ManualTitles = new List<int> { 0 } };
            SingleThreadContext.Run(async () =>
                await new JobRunner(job, config, fake.Executable, null, new UiDispatcher(), new NullPlatformServices(), new[] { "dvd_MinimumTitleLength" }).RunAsync());

            Assert.True(job.State.StatusWord() == c.Status, $"{c.Fixture}: {job.State} {job.ErrorMessage}");
            if (c.Error != null)
                Assert.True(job.ErrorMessage?.Contains(c.Error, StringComparison.OrdinalIgnoreCase) == true, $"{c.Fixture}: {job.ErrorMessage}");
            var items = JobRunner.VisibleItems(root);
            if (c.Status == "success") Assert.Equal(new[] { "Sample Movie" }, items);
            else if (c.KeptApart)
            {
                var kept = Assert.Single(items);
                Assert.EndsWith("]", kept);
                Assert.Contains(JobRunner.VisibleItems(Path.Combine(root, kept)), n => n.EndsWith(".mkv", StringComparison.Ordinal));
            }
            else Assert.True(items.Count == 0, $"{c.Fixture}: nothing should be left, found {string.Join(", ", items)}");
        }
    }
}

public class ReliabilityTests
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-rel-" + Guid.NewGuid().ToString("N")[..8]);

    [Fact]
    public void StuckProcessIsStopped()
    {
        if (OperatingSystem.IsWindows()) return;
        var watch = System.Diagnostics.Stopwatch.StartNew();
        var lines = new LineCollector();
        var r = new ProcessRunner("/bin/sh", new[] { "-c", "echo started; sleep 60" }).RunAsync(lines.Add, stallTimeout: TimeSpan.FromSeconds(2)).GetAwaiter().GetResult();
        Assert.True(r.Stalled);
        Assert.NotEqual(0, r.ExitCode);
        Assert.True(watch.Elapsed < TimeSpan.FromSeconds(15), watch.Elapsed.ToString());
        Assert.Equal(new[] { "started" }, lines.All);
    }

    [Fact]
    public void BusyProcessIsNotStopped()
    {
        if (OperatingSystem.IsWindows()) return;
        var r = new ProcessRunner("/bin/sh", new[] { "-c", "for i in 1 2 3 4 5 6; do echo $i; sleep 0.5; done" })
            .RunAsync(_ => { }, stallTimeout: TimeSpan.FromSeconds(2)).GetAwaiter().GetResult();
        Assert.False(r.Stalled);
        Assert.Equal(0, r.ExitCode);
    }

    RipJob Run(FakeMakeMkv fake, RipMode mode, DiscSource source, List<int>? titles = null)
    {
        Directory.CreateDirectory(_root);
        Paths.DataOverride = Path.Combine(Path.GetTempPath(), "bromelia-test-data");
        var config = new AppConfig { OutputRoot = _root };
        var drive = new DriveConfig();
        drive.Automation.Notify = false;
        drive.Automation.EjectWhenDone = false;
        drive.Rip.BackupFormat = BackupFormat.Iso;
        var job = new RipJob(source, drive, "lane", "test", "", mode) { ManualTitles = titles };
        SingleThreadContext.Run(async () =>
            await new JobRunner(job, config, fake.Executable, null, new UiDispatcher(), new NullPlatformServices(), new[] { "dvd_MinimumTitleLength" }).RunAsync());
        return job;
    }

    [Fact]
    public void NotEnoughFreeSpaceStopsBeforeRipping()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1)) + $"TINFO:0,11,0,\"{1L << 60}\"\n", FakeMakeMkv.WritesFile(Listing.Saved));
        var job = Run(fake, RipMode.Mkv, new DiscSource.Iso("/nonexistent/test.iso"), new List<int> { 0 });
        Assert.Equal(JobState.Failed, job.State);
        Assert.Contains("Not enough free space", job.ErrorMessage);
        Assert.DoesNotContain(" mkv ", fake.Calls);
        Assert.Empty(JobRunner.VisibleItems(_root));
    }

    [Fact]
    public void RequiredSpaceHasAMargin()
    {
        Assert.Equal(1_000 + (256L << 20), DiskSpace.Required(1_000));
        Assert.Equal((100L << 30) + (2L << 30), DiskSpace.Required(100L << 30));
        Assert.True(DiskSpace.Available(Path.GetTempPath()) > 0);
    }

    const string SavedLine = "echo 'MSG:5036,260,1,\"Copy complete. 1 titles saved.\",\"Copy complete. %1 titles saved.\",\"1\"'";

    [Fact]
    public void IsoBackupIsKept()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1)), ":",
            "head -c 40000 /dev/zero > \"$dest\"\nprintf 'BEA01' | dd of=\"$dest\" bs=1 seek=32769 conv=notrunc 2>/dev/null\n" + SavedLine);
        var job = Run(fake, RipMode.BackupDecrypted, new DiscSource.Drive(0, ""));
        Assert.True(job.State == JobState.Succeeded, job.ErrorMessage);
        Assert.EndsWith(".iso", job.ProducedFiles[0]);
    }

    [Fact]
    public void FolderWrittenInsteadOfAnIsoIsKeptAsAFolder()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1)), FakeMakeMkv.WritesFile(Listing.Saved),
            "mkdir -p \"$dest/BDMV\" && printf x > \"$dest/BDMV/index.bdmv\"\n" + SavedLine);
        var job = Run(fake, RipMode.BackupThenMkv, new DiscSource.Drive(0, ""));
        Assert.True(job.State == JobState.Succeeded, job.ErrorMessage);
        var folder = Assert.Single(job.ProducedFiles, Directory.Exists);
        Assert.Equal("", Path.GetExtension(folder));
        Assert.Contains(job.Log, l => l.Text.Contains("instead of an ISO image"));
        Assert.Contains("mkv file:", fake.Calls);
    }

    [Fact]
    public void FolderWithoutADiscIsNotAcceptedAsAnIso()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.Make(("0:00:10", 1)), ":", "mkdir -p \"$dest/junk\" && printf x > \"$dest/junk/file\"\n" + SavedLine);
        var job = Run(fake, RipMode.BackupDecrypted, new DiscSource.Drive(0, ""));
        Assert.Equal(JobState.Failed, job.State);
        Assert.Contains("Backup failed the check", job.ErrorMessage);
    }

    [Fact]
    public void ArchiveEverythingPreset()
    {
        var d = new DriveConfig { Name = "Left" };
        d.Rip.TitleSelection.Strategy = TitleStrategy.Longest;
        d.ApplyArchiveEverything();
        Assert.Equal("Left", d.Name);
        Assert.Equal(RipMode.BackupThenMkv, d.Rip.Mode);
        Assert.True(d.Rip.KeepBackupAfterMkv);
        Assert.Equal(TitleStrategy.All, d.Rip.TitleSelection.Strategy);
        Assert.Equal(ProfileMode.Generated, d.Profile.Mode);
        Assert.Equal("+sel:all", d.Profile.Generated.SelectionRule);
        Assert.True(d.Archive.VerifyRips && d.Rip.WriteDiscInfoJson);
    }

    [Fact]
    public void SettingsHaveSafeDefaults()
    {
        var c = JsonSerializer.Deserialize<AppConfig>("{}", ConfigJson.Options)!;
        Assert.Equal(30, c.StallTimeoutMinutes);
        Assert.True(c.PreventSleep);
    }

    [Fact]
    public void FailuresNameTheirCause()
    {
        string? First(string fixture) => File.ReadAllLines(Path.Combine(AppContext.BaseDirectory, "fixtures", fixture + ".txt"))
            .Select(RobotParser.Parse).OfType<RobotEvent.Message>().Select(m => m.Value)
            .FirstOrDefault(m => m.Severity == Severity.Error && m.Code is not (5037 or 5004))?.Text;
        Assert.Contains("megabytes free on the destination", First("rip-disk-full"));
        Assert.Contains("does not exist", First("rip-missing-source"));
        Assert.Contains("MEDIUM ERROR", First("rip-failure"));
    }
}

public class ParityTests
{
    static RobotMessage Msg(string line) => ((RobotEvent.Message)RobotParser.Parse(line)!).Value;

    [Fact]
    public void RecognisesNotices()
    {
        Assert.Equal(new MakeMKVNotice(NoticeKind.LibreDrive, "v06.3 id=4FBA32AEC678"),
            MakeMKVNotice.From(Msg("MSG:1011,0,1,\"Using LibreDrive mode (v06.3 id=4FBA32AEC678)\",\"%1\",\"Using LibreDrive mode (v06.3 id=4FBA32AEC678)\"")));
        Assert.Equal(NoticeKind.KeyExpired, MakeMKVNotice.From(Msg("MSG:5055,0,0,\"Evaluation period has expired, shareware functionality unavailable.\",\"x\""))!.Kind);
        Assert.Equal(NoticeKind.KeyExpired, MakeMKVNotice.From(Msg("MSG:5052,516,0,\"Evaluation period has expired. Please purchase an activation key.\",\"x\""))!.Kind);
        Assert.Equal(NoticeKind.VersionTooOld, MakeMKVNotice.From(Msg("MSG:5021,260,1,\"This application version is too old.  Please download the latest version at http://www.makemkv.com/ or enter a registration key to continue using the current version.\",\"x\",\"http://www.makemkv.com/\""))!.Kind);
        Assert.Equal(NoticeKind.LibreDriveRequired, MakeMKVNotice.From(Msg("MSG:2024,0,0,\"LibreDrive compatible drive is required to open this disc - video can't be decrypted.\",\"x\""))!.Kind);
        Assert.Null(MakeMKVNotice.From(Msg("MSG:3007,0,0,\"Using direct disc access mode\",\"Using direct disc access mode\"")));
        Assert.True(new MakeMKVNotice(NoticeKind.KeyExpired).IsLicenseProblem);
        Assert.False(new MakeMKVNotice(NoticeKind.LibreDriveRequired).IsLicenseProblem);
    }

    [Fact]
    public void ReadsTheBetaKeyFromTheForumPage()
    {
        var key = "T-" + string.Concat(Enumerable.Repeat("aB3@_x", 11));
        var html = "<div class=\"codebox\"><p>Code: <a href=\"#\">Select all</a></p><pre><code>" + key + "</code></pre></div> and is valid until end of October";
        Assert.Equal(key, BetaKey.Parse(html));
        Assert.Null(BetaKey.Parse("no key here"));
    }

    [Theory]
    [InlineData("/Rips/Disc/VIDEO_TS/VTS_01_1.VOB", false, "file:/Rips/Disc")]
    [InlineData("/Rips/Disc/VIDEO_TS/VIDEO_TS.IFO", false, "file:/Rips/Disc")]
    [InlineData("/Rips/Disc/BDMV/PLAYLIST/00800.mpls", false, "file:/Rips/Disc")]
    [InlineData("/Rips/Disc/BDMV/STREAM/00001.m2ts", false, "file:/Rips/Disc")]
    [InlineData("/Rips/Disc/BDMV", true, "file:/Rips/Disc")]
    [InlineData("/Rips/Disc", true, "file:/Rips/Disc")]
    [InlineData("/Rips/Movie.ISO", false, "iso:/Rips/Movie.ISO")]
    [InlineData("/Rips/loose.m2ts", false, "file:/Rips/loose.m2ts")]
    public void OpensTheDiscAFileBelongsTo(string path, bool isDirectory, string expected)
    {
        if (OperatingSystem.IsWindows()) return;
        Assert.Equal(expected, SourceResolver.Resolve(path, isDirectory).InfoArgument);
    }

    static DiscInfo Titles(params int[] durations)
    {
        var d = new DiscInfo();
        for (int i = 0; i < durations.Length; i++)
            d.Titles.Add(new TitleInfo
            {
                Index = i,
                Attributes = { [(int)AttributeId.Duration] = TitleInfo.FormatDuration(durations[i]), [(int)AttributeId.OriginalTitleId] = (i + 1).ToString() },
            });
        return d;
    }

    [Fact]
    public void OnePassNeedsTheLongestTitlesAndEnoughOfThem()
    {
        var info = Titles(600, 30, 900, 1200, 40);
        Assert.Equal(41, OnePass.MinimumLength(new[] { 0, 2, 3 }, info, null));
        Assert.Null(OnePass.MinimumLength(new[] { 0, 2 }, info, null));
        Assert.Null(OnePass.MinimumLength(new[] { 0, 1, 2, 3, 4 }, info, null));
        Assert.Null(OnePass.MinimumLength(new[] { 1, 2, 3 }, info, null));
        Assert.Null(OnePass.MinimumLength(new[] { 0, 2, 3 }, Titles(600, 599, 900, 1200), null));
        Assert.Null(OnePass.MinimumLength(new[] { 0, 2, 3 }, info, 120));
        var shifted = Titles(600, 900, 1200);
        for (int i = 0; i < 3; i++) shifted.Titles[i].Attributes[(int)AttributeId.OriginalTitleId] = new[] { "1", "3", "4" }[i];
        Assert.True(OnePass.Matches(shifted, new[] { 0, 2, 3 }, info));
        Assert.False(OnePass.Matches(Titles(600, 900), new[] { 0, 2, 3 }, info));
    }
}

public class OnePassJobTests
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-1p-" + Guid.NewGuid().ToString("N")[..8]);

    /// <summary>A stand-in makemkvcon whose info command honours --minlength (titles of at least N seconds, renumbered).</summary>
    static FakeMakeMkv Fake((string Duration, int Source)[] all, string extraInfo = "")
    {
        const string mkv = "if [ \"$title\" = \"all\" ]; then\n" +
                           "  for t in 0 1 2 3 4 5; do grep -q \"title_t0$t.mkv\" \"$LISTING\" && printf 'mkv data' > \"$dest/title_t0$t.mkv\"; done\n" +
                           "else\n  printf 'mkv data' > \"$dest/title_t0$title.mkv\"\nfi\n" +
                           "echo 'MSG:5036,260,1,\"Copy complete. 1 titles saved.\",\"Copy complete. %1 titles saved.\",\"1\"'";
        var f = new FakeMakeMkv(extraInfo + Listing.Make(all), mkv);
        foreach (var n in new[] { 6, 11, 21 })
            File.WriteAllText(Path.Combine(f.Dir, $"min{n}.txt"), Listing.Make(all.Where(t => TitleInfo.ParseDuration(t.Duration) >= n).ToArray()));
        var listing = Path.Combine(f.Dir, "listing.txt");
        var script = File.ReadAllText(f.Executable)
            .Replace("#!/bin/sh\n", "#!/bin/sh\nMIN=0; for a in \"$@\"; do case \"$a\" in --minlength=*) MIN=\"${a#--minlength=}\";; esac; done\n" +
                                    $"LISTING='{f.Dir}'/min$MIN.txt; [ -f \"$LISTING\" ] || LISTING='{listing}'\n")
            .Replace($"info) cat '{listing}' ;;", "info) cat \"$LISTING\" ;;");
        File.WriteAllText(f.Executable, script);
        return f;
    }

    RipJob Run(FakeMakeMkv f, Action<DriveConfig> configure)
    {
        Directory.CreateDirectory(_root);
        Paths.DataOverride = Path.Combine(Path.GetTempPath(), "bromelia-test-data");
        var config = new AppConfig { OutputRoot = _root };
        var drive = new DriveConfig();
        drive.Automation.Notify = false;
        configure(drive);
        var job = new RipJob(new DiscSource.Iso("/nonexistent/test.iso"), drive, "iso:1p", "test", "", RipMode.Mkv);
        SingleThreadContext.Run(async () =>
            await new JobRunner(job, config, f.Executable, null, new UiDispatcher(), new NullPlatformServices(), new[] { "dvd_MinimumTitleLength" }).RunAsync());
        return job;
    }

    static readonly (string, int)[] FiveTitles = { ("0:00:30", 1), ("0:00:05", 2), ("0:00:20", 3), ("0:00:40", 4), ("0:00:10", 5) };

    [Fact]
    public void LongestTitlesAreRippedInOnePass()
    {
        if (OperatingSystem.IsWindows()) return;
        var f = Fake(FiveTitles);
        var job = Run(f, d => { d.Rip.TitleSelection.Strategy = TitleStrategy.Longest; d.Rip.TitleSelection.LongestCount = 3; });
        Assert.True(job.State == JobState.Succeeded, job.ErrorMessage + "\n" + f.Calls);
        var rips = f.Calls.Split('\n').Where(l => l.Contains(" mkv ")).ToList();
        var rip = Assert.Single(rips);
        Assert.Contains("--minlength=11", rip);
        Assert.Contains("mkv iso:/nonexistent/test.iso all ", rip);
        Assert.Equal(3, job.ProducedFiles.Count);
        Assert.Equal(new int?[] { 1, 3, 4 }, job.RipInfo!.Titles.Select(t => t.SourceTitleId));
    }

    [Fact]
    public void OtherSelectionsAreRippedTitleByTitle()
    {
        if (OperatingSystem.IsWindows()) return;
        var f = Fake(FiveTitles);
        var job = Run(f, d => { d.Rip.TitleSelection.Strategy = TitleStrategy.Indices; d.Rip.TitleSelection.IndexPattern = "0,1,3"; });
        Assert.True(job.State == JobState.Succeeded, job.ErrorMessage);
        Assert.Equal(3, f.Calls.Split('\n').Count(l => l.Contains(" mkv ")));
        Assert.DoesNotContain("--minlength", f.Calls);
    }

    [Fact]
    public void ExpiredKeyExplainsTheFailure()
    {
        if (OperatingSystem.IsWindows()) return;
        var f = new FakeMakeMkv("MSG:5055,0,0,\"Evaluation period has expired, shareware functionality unavailable.\",\"x\"\nMSG:5010,0,0,\"Failed to open disc\",\"Failed to open disc\"\n", ":");
        var job = Run(f, _ => { });
        Assert.Equal(JobState.Failed, job.State);
        Assert.Equal(NoticeKind.KeyExpired, job.MakemkvProblem?.Kind);
        Assert.Contains("key has expired", job.ErrorMessage);
    }

    /// <summary>Rips the three longest titles of a real disc image in one makemkvcon run. Set BROMELIA_TEST_ONEPASS_ISO to a
    /// DVD image with at least four titles.</summary>
    [Fact]
    public void RealDiscImageIsRippedInOnePass()
    {
        var iso = Environment.GetEnvironmentVariable("BROMELIA_TEST_ONEPASS_ISO");
        var exe = Paths.ResolveTool("", Paths.MakemkvconCandidates(), "makemkvcon");
        if (string.IsNullOrEmpty(iso) || exe == null || OperatingSystem.IsWindows()) return;
        Directory.CreateDirectory(_root);
        Paths.DataOverride = Path.Combine(_root, ".data");
        try
        {
            var config = new AppConfig { OutputRoot = _root };
            var drive = new DriveConfig();
            drive.Automation.Notify = false;
            drive.Rip.MinLengthSeconds = 0;
            drive.Rip.TitleSelection.Strategy = TitleStrategy.Longest;
            drive.Rip.TitleSelection.LongestCount = 3;
            drive.Episodes.SplitPlayAll = false;
            var job = new RipJob(new DiscSource.Iso(iso), drive, "iso:1p", "test", "", RipMode.Mkv);
            var mkvmerge = Paths.ResolveTool("", Paths.MkvmergeCandidates(), "mkvmerge");
            SingleThreadContext.Run(async () =>
                await new JobRunner(job, config, exe, mkvmerge, new UiDispatcher(), new NullPlatformServices(), new[] { "dvd_MinimumTitleLength" }).RunAsync());
            Assert.True(job.State == JobState.Succeeded, job.ErrorMessage + "\n" + string.Join("\n", job.Log.TakeLast(20).Select(l => l.Text)));
            var rip = Assert.Single(job.Commands, c => c.Contains(" mkv "));
            Assert.Contains(" all ", rip);
            Assert.Equal(3, job.ProducedFiles.Count);
            Assert.Contains(job.Log, l => l.Text.Contains("in one pass"));
            Assert.Empty(Checksums.Verify(job.OutputDirectory!));
        }
        finally { try { Directory.Delete(_root, true); } catch (IOException) { } }
    }

    [Fact]
    public void LibreDriveIsRecorded()
    {
        if (OperatingSystem.IsWindows()) return;
        var f = Fake(new[] { ("0:00:30", 1) }, "MSG:1011,0,1,\"Using LibreDrive mode (v06.3 id=4FBA32AEC678)\",\"%1\",\"x\"\n");
        var job = Run(f, _ => { });
        Assert.True(job.State == JobState.Succeeded, job.ErrorMessage);
        Assert.Equal("v06.3 id=4FBA32AEC678", job.LibreDrive);
        var record = JsonDocument.Parse(File.ReadAllText(Path.Combine(job.OutputDirectory!, "bromelia.json"))).RootElement;
        Assert.Equal("v06.3 id=4FBA32AEC678", record.GetProperty("libreDrive").GetString());
    }
}

public class IntegrationFeatureTests
{
    static JsonElement BodyJson(NotificationSender.Delivery? d) => JsonDocument.Parse(d!.Body).RootElement;

    [Fact]
    public void NotificationDeliveries()
    {
        Assert.Equal("**T**\nB", BodyJson(NotificationSender.For("https://discord.com/api/webhooks/1/abc", "T", "B", "success")).GetProperty("content").GetString());
        Assert.Equal("*T*\nB", BodyJson(NotificationSender.For("https://hooks.slack.com/services/x/y/z", "T", "B", "failed")).GetProperty("text").GetString());
        var ntfy = NotificationSender.For("ntfy://rips", "T", "B", "failed")!;
        Assert.Equal("https://ntfy.sh/rips", ntfy.Url!.ToString());
        Assert.Equal("T", ntfy.Headers["Title"]);
        Assert.Equal("B", System.Text.Encoding.UTF8.GetString(ntfy.Body));
        Assert.Equal("https://ntfy.example.org/rips", NotificationSender.For("ntfys://ntfy.example.org/rips", "T", "B", "success")!.Url!.ToString());
        Assert.Equal("errors", BodyJson(NotificationSender.For("https://example.org/hook", "T", "B", "errors")).GetProperty("status").GetString());
        Assert.Equal("tgram://bot/chat", NotificationSender.For("tgram://bot/chat", "T", "B", "success")!.AppriseUrl);
        Assert.Null(NotificationSender.For("", "T", "B", "success"));
    }

    [Fact]
    public void MetadataResults()
    {
        var m = MetadataLookup.Parse("{\"results\":[{\"id\":1,\"title\":\"Inception Behind\",\"release_date\":\"2011-01-01\"},{\"id\":27205,\"title\":\"Inception\",\"release_date\":\"2010-07-15\"}]}", MetadataProvider.Tmdb, "INCEPTION")!;
        Assert.Equal(("Inception", 2010, 27205), (m.Title, m.Year!.Value, m.TmdbId!.Value));
        Assert.Equal(1999, MetadataLookup.Parse("{\"results\":[{\"id\":37854,\"name\":\"One Piece\",\"first_air_date\":\"1999-10-20\"}]}", MetadataProvider.Tmdb, "One Piece")!.Year);
        var o = MetadataLookup.Parse("{\"Title\":\"Friends\",\"Year\":\"1994–2004\",\"imdbID\":\"tt0108778\",\"Response\":\"True\"}", MetadataProvider.Omdb, "Friends")!;
        Assert.Equal((1994, "tt0108778"), (o.Year!.Value, o.ImdbId!));
        Assert.Null(MetadataLookup.Parse("{\"Response\":\"False\"}", MetadataProvider.Omdb, "x"));
        var c = new MetadataConfig { Provider = MetadataProvider.Tmdb, ApiKey = "0123456789abcdef0123456789abcdef" };
        var r = MetadataLookup.Request("One Piece", MediaKind.Tv, c)!.Value;
        Assert.Equal("/3/search/tv", r.Url.AbsolutePath);
        Assert.Contains("api_key=0123", r.Url.Query);
        c.ApiKey = "eyJ" + new string('x', 60);
        var b = MetadataLookup.Request("One Piece", MediaKind.Movie, c)!.Value;
        Assert.StartsWith("eyJ", b.Bearer);
        Assert.DoesNotContain("api_key", b.Url.Query);
    }

    [Fact]
    public void OtherDiscModesAndBetaKeys()
    {
        var d = new DriveConfig();
        d.Rip.Mode = RipMode.BackupThenMkv;
        Assert.Equal(RipMode.BackupThenMkv, DiscContentRules.ModeFor(DiscFlags.BlurayFiles, () => DiscContent.Data, d));
        Assert.Equal(RipMode.BackupThenMkv, DiscContentRules.ModeFor(DiscFlags.None, () => DiscContent.Video, d));
        Assert.Equal(RipMode.AudioCD, DiscContentRules.ModeFor(DiscFlags.None, () => DiscContent.Audio, d));
        Assert.Equal(RipMode.DataImage, DiscContentRules.ModeFor(DiscFlags.None, () => DiscContent.Data, d));
        d.Other.RipAudioCDs = false;
        d.Other.ImageDataDiscs = false;
        Assert.Null(DiscContentRules.ModeFor(DiscFlags.None, () => DiscContent.Audio, d));
        Assert.Null(DiscContentRules.ModeFor(DiscFlags.None, () => DiscContent.Data, d));
        Assert.True(BetaKey.MayReplace(null) && BetaKey.MayReplace("") && BetaKey.MayReplace("T-abc"));
        Assert.False(BetaKey.MayReplace("M-purchasedkey"));
        d.Rip.FormatModes["bluray"] = RipMode.Backup;
        Assert.Equal(RipMode.Backup, d.Rip.ModeFor(DiscFormat.Bluray));
        Assert.Equal(RipMode.BackupThenMkv, d.Rip.ModeFor(DiscFormat.Dvd));
        var json = ConfigJson.Serialize(new AppConfig { WebUI = { Enabled = true }, Notifications = { new NotificationTarget { Url = "ntfy://x" } } });
        Assert.Contains("\"webUI\"", json);
        Assert.Contains("\"audioCommand\"", ConfigJson.Serialize(new AppConfig()) + "");
        var c2 = JsonSerializer.Deserialize<AppConfig>("{}", ConfigJson.Options)!;
        Assert.Equal(1, c2.BackgroundJobs);
        Assert.Equal("\"audioCD\"", JsonSerializer.Serialize(RipMode.AudioCD, ConfigJson.Options));
        Assert.Equal("\"mediaServer\"", JsonSerializer.Serialize(LibraryLayout.MediaServer, ConfigJson.Options));
    }

    [Fact]
    public void WebAccessRules()
    {
        var c = new WebUIConfig();
        Assert.True(WebAccess.Allowed("GET", "localhost:51280", null, null, null, c).Ok);
        Assert.Equal(403, WebAccess.Allowed("GET", "attacker.example:51280", null, null, null, c).Status);
        Assert.Equal(403, WebAccess.Allowed("POST", "127.0.0.1:51280", null, null, null, c).Status);
        Assert.True(WebAccess.Allowed("POST", "127.0.0.1:51280", null, null, "1", c).Ok);
        c.Token = "s3cret";
        Assert.True(WebAccess.Allowed("GET", "attacker.example", null, "s3cret", null, c).Ok);
        Assert.Equal(401, WebAccess.Allowed("GET", "localhost", null, null, null, c).Status);
        Assert.True(WebAccess.Allowed("GET", "x", "Bearer s3cret", null, null, c).Ok);
        Assert.NotNull(WebAccess.StartProblem(new WebUIConfig { Address = "0.0.0.0" }));
        Assert.Null(WebAccess.StartProblem(new WebUIConfig { Address = "0.0.0.0", Token = "t" }));
        Assert.NotNull(WebServer.Page());
    }

    [Fact]
    public void WebServerAnswersStatusAndActions()
    {
        var dir = Path.Combine(Path.GetTempPath(), "bromelia-web-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(dir);
        Paths.DataOverride = dir;
        try
        {
            SingleThreadContext.Run(async () =>
            {
                var state = new AppState(new NullPlatformServices(), Path.Combine(dir, "config.json"));
                state.Web.Apply(new WebUIConfig { Enabled = true, Port = Random.Shared.Next(52000, 58000) });
                Assert.Null(state.Web.LastError);
                try
                {
                    var (s1, _, status) = state.Web.Handle("GET", "/api/status", "127.0.0.1:1", null, null, null);
                    Assert.Equal(200, s1);
                    var doc = JsonDocument.Parse(status).RootElement;
                    Assert.Equal(JsonValueKind.Array, doc.GetProperty("drives").ValueKind);
                    Assert.Equal(JsonValueKind.Array, doc.GetProperty("background").ValueKind);
                    var (s2, _, page) = state.Web.Handle("GET", "/", "localhost", null, null, null);
                    Assert.Equal(200, s2);
                    Assert.Contains("<title>Bromelia</title>", System.Text.Encoding.UTF8.GetString(page));
                    Assert.Equal(403, state.Web.Handle("POST", "/api/jobs/nope/cancel", "localhost", null, null, null).Item1);
                    var (s4, _, body) = state.Web.Handle("POST", "/api/jobs/nope/cancel", "localhost", null, null, "1");
                    Assert.Equal((400, "No such job"), (s4, System.Text.Encoding.UTF8.GetString(body)));
                }
                finally { state.Web.Stop(); }
                await Task.CompletedTask;
            });
        }
        finally
        {
            Paths.DataOverride = null;
            try { Directory.Delete(dir, true); } catch (IOException) { }
        }
    }
}

public class IntegrationJobTests
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-int-" + Guid.NewGuid().ToString("N")[..8]);

    RipJob Run(FakeMakeMkv fake, Action<DriveConfig> configure, DiscSource? source = null, RipMode mode = RipMode.Mkv, Action<RipJob>? prepare = null)
    {
        Directory.CreateDirectory(_root);
        Paths.DataOverride = Path.Combine(Path.GetTempPath(), "bromelia-test-data");
        var config = new AppConfig { OutputRoot = _root };
        var drive = new DriveConfig();
        drive.Automation.Notify = false;
        drive.Automation.EjectWhenDone = false;
        configure(drive);
        var job = new RipJob(source ?? new DiscSource.Iso("/nonexistent/test.iso"), drive, "lane", "test", "", mode);
        prepare?.Invoke(job);
        SingleThreadContext.Run(async () =>
            await new JobRunner(job, config, fake.Executable, null, new UiDispatcher(), new NullPlatformServices(), new[] { "dvd_MinimumTitleLength" }).RunAsync());
        return job;
    }

    List<string> Files() => Directory.Exists(_root)
        ? Directory.EnumerateFiles(_root, "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(_root, f).Replace('\\', '/'))
            .Where(f => !f.Split('/').Any(p => p.StartsWith('.'))).OrderBy(f => f, StringComparer.Ordinal).ToList()
        : new();

    [Fact]
    public void MoviesAndEpisodesAreNamedForMediaServers()
    {
        if (OperatingSystem.IsWindows()) return;
        var movie = Run(new FakeMakeMkv(Listing.Make(("1:50:00", 1), ("0:05:00", 2)), FakeMakeMkv.WritesFile(Listing.Saved)), d => d.Output.Layout = LibraryLayout.MediaServer);
        Assert.True(movie.State == JobState.Succeeded, movie.ErrorMessage);
        Assert.Equal(new[] { "Movies/Sample Movie/Other/Sample Movie - Playlist 00002.mkv", "Movies/Sample Movie/Sample Movie.mkv" },
            Files().Where(f => f.EndsWith(".mkv")));
        var tv = Run(new FakeMakeMkv(Listing.Make("SAMPLE_SHOW_S2_D1", ("0:22:00", 1), ("0:22:30", 2), ("0:21:40", 3)), FakeMakeMkv.WritesFile(Listing.Saved)),
            d => d.Output.Layout = LibraryLayout.MediaServer);
        Assert.True(tv.State == JobState.Succeeded, tv.ErrorMessage);
        Assert.Equal(Enumerable.Range(1, 3).Select(i => $"TV Shows/Sample Show/Season 02/Sample Show - S02E0{i}.mkv"),
            Files().Where(f => f.StartsWith("TV Shows") && f.EndsWith(".mkv")));
        // The next disc of the season is added to the same show and Season folders.
        var tv2 = Run(new FakeMakeMkv(Listing.Make("SAMPLE_SHOW_S2_D2", ("0:22:00", 1), ("0:22:30", 2), ("0:21:40", 3)), FakeMakeMkv.WritesFile(Listing.Saved)),
            d => d.Output.Layout = LibraryLayout.MediaServer, prepare: j => j.FirstEpisode = 4);
        Assert.True(tv2.State == JobState.Succeeded, tv2.ErrorMessage);
        Assert.Equal(Enumerable.Range(1, 6).Select(i => $"TV Shows/Sample Show/Season 02/Sample Show - S02E0{i}.mkv"),
            Files().Where(f => f.StartsWith("TV Shows") && f.EndsWith(".mkv")));
        Assert.All(tv2.ProducedFiles, f => Assert.True(File.Exists(f), f));
        Assert.Empty(Checksums.Verify(Path.Combine(_root, "TV Shows", "Sample Show")));
    }

    [Fact]
    public void BackgroundStepsRunAfterTheJob()
    {
        if (OperatingSystem.IsWindows()) return;
        var marker = Path.Combine(Path.GetTempPath(), "bromelia-bg-" + Guid.NewGuid().ToString("N")[..8] + ".txt");
        var job = Run(new FakeMakeMkv(Listing.Make(("0:30:00", 1)), FakeMakeMkv.WritesFile(Listing.Saved)), d => d.PostProcess.Add(new PostProcessStep
        {
            Name = "Encode", Executable = "/bin/sh", Arguments = $"-c 'sleep 0.3; echo \"$BROMELIA_STATUS\" > \"$0\"' {marker}", Background = true,
        }));
        Assert.Equal(JobState.Succeeded, job.State);
        Assert.False(File.Exists(marker), "a background step must not run during the job");
        Assert.NotNull(job.Background);
        SingleThreadContext.Run(async () =>
        {
            var queue = new BackgroundQueue(new UiDispatcher());
            queue.Enqueue(job.Background!);
            for (int i = 0; i < 200 && queue.IsBusy; i++) await Task.Delay(20);
            Assert.Equal("done", queue.Items[0].State);
        });
        Assert.Equal("success\n", File.ReadAllText(marker));
    }

    [Fact]
    public void FormatModesReplaceTheConfiguredMode()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv(Listing.Make(("0:30:00", 1)), FakeMakeMkv.WritesFile(Listing.Saved));
        var job = Run(fake, d => d.Rip.FormatModes["bluray"] = RipMode.InfoOnly, prepare: j => j.UsesConfiguredMode = true);
        Assert.True(job.State == JobState.Succeeded, job.ErrorMessage);
        Assert.Equal(RipMode.InfoOnly, job.Mode);
        Assert.Equal("disc-info.json", Path.GetFileName(Assert.Single(job.ProducedFiles)));
        Assert.DoesNotContain(" mkv ", fake.Calls);
    }

    [Fact]
    public void AudioCDsUseTheAudioCommand()
    {
        if (OperatingSystem.IsWindows()) return;
        var fake = new FakeMakeMkv("", ":");
        var job = Run(fake, d => d.Other.AudioCommand = "/bin/sh -c 'mkdir -p \"Artist - Album\" && printf \"%s\" \"$0\" > \"Artist - Album/01 - Track.flac\"' {device}",
            new DiscSource.Drive(0, "/dev/fake9"), RipMode.AudioCD);
        Assert.True(job.State == JobState.Succeeded, job.ErrorMessage);
        var flac = Assert.Single(Files(), f => f.EndsWith(".flac"));
        Assert.EndsWith("Artist - Album/01 - Track.flac", flac);
        Assert.Equal("/dev/fake9", File.ReadAllText(Path.Combine(_root, flac)));
        Assert.DoesNotContain("info", fake.Calls);
        var missing = Run(new FakeMakeMkv("", ":"), d => d.Other.AudioCommand = "no-such-ripper-xyz {device}", new DiscSource.Drive(0, "/dev/fake9"), RipMode.AudioCD);
        Assert.Equal(JobState.Failed, missing.State);
        Assert.Contains("cyanrip or abcde", missing.ErrorMessage);
    }

    [Fact]
    public void DataDiscsAreCopiedExactly()
    {
        // A file stands in for the disc device; set BROMELIA_TEST_DVD_ISO to copy a real disc image.
        var iso = Environment.GetEnvironmentVariable("BROMELIA_TEST_DVD_ISO");
        var source = iso;
        if (string.IsNullOrEmpty(source))
        {
            source = Path.Combine(Path.GetTempPath(), "bromelia-disc-" + Guid.NewGuid().ToString("N")[..8] + ".bin");
            var image = new byte[3 * 1024 * 1024 + 2048];
            new Random(7).NextBytes(image);
            System.Text.Encoding.ASCII.GetBytes("CD001").CopyTo(image, 32769);
            File.WriteAllBytes(source, image);
        }
        var job = Run(new FakeMakeMkv("", ":"), _ => { }, new DiscSource.Drive(0, source), RipMode.DataImage);
        Assert.True(job.State == JobState.Succeeded, job.ErrorMessage);
        var copy = Assert.Single(job.ProducedFiles);
        Assert.EndsWith(".iso", copy);
        Assert.Equal(Checksums.Sha256(source), Assert.Single(job.Checksums).Sha256);
    }
}
