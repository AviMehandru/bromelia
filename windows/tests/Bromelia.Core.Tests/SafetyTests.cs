using System.Text.Json;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
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
