using System.Collections.Concurrent;
using System.Xml;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Bromelia.Core.Logic;
using Bromelia.Core.Robot;
using Xunit;

namespace Bromelia.Core.Tests;

static class Fixtures
{
    public static string Read(string name) => File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "fixtures", name + ".txt"));
}

public class RobotParserTests
{
    [Fact]
    public void SplitsQuotedFields()
    {
        var f = RobotParser.SplitFields("1,2,\"a, b\",\"say \\\"hi\\\"\",\"back\\\\slash\",plain");
        Assert.Equal(new[] { "1", "2", "a, b", "say \"hi\"", "back\\slash", "plain" }, f);
    }

    [Fact]
    public void ParsesMessagesAndProgress()
    {
        var m = Assert.IsType<RobotEvent.Message>(RobotParser.Parse("MSG:5036,260,1,\"Copy complete. 1 titles saved.\",\"Copy complete. %1 titles saved.\",\"1\"")).Value;
        Assert.Equal(5036, m.Code);
        Assert.Equal(new[] { "1" }, m.Parameters);
        Assert.Equal(Severity.Info, m.Severity);
        Assert.Equal(new RobotEvent.ProgressValue(5957, 356, 65536), RobotParser.Parse("PRGV:5957,356,65536"));
        Assert.Equal(new RobotEvent.ProgressTotalTitle(5018, 0, "Scanning CD-ROM devices"), RobotParser.Parse("PRGT:5018,0,\"Scanning CD-ROM devices\""));
        Assert.Equal(new RobotEvent.TitleCount(3), RobotParser.Parse("TCOUNT:3"));
        Assert.IsType<RobotEvent.Raw>(RobotParser.Parse("Backup source must start with \"disc:\""));
        Assert.Null(RobotParser.Parse(""));
    }

    [Fact]
    public void ClassifiesSeverity()
    {
        Assert.Equal(Severity.Debug, new RobotMessage(1003, 16777248, "DEBUG: Code 0", "", Array.Empty<string>()).Severity);
        Assert.Equal(Severity.Error, new RobotMessage(5037, 516, "x", "", Array.Empty<string>()).Severity);
        Assert.Equal(Severity.Warning, new RobotMessage(1, 1028, "x", "", Array.Empty<string>()).Severity);
    }

    [Fact]
    public void ParsesDriveScanFixture()
    {
        var drives = Fixtures.Read("drive-scan").Split('\n').Select(RobotParser.Parse).OfType<RobotEvent.Drive>().Select(d => d.Entry).ToList();
        Assert.Equal(8, drives.Count);
        var present = drives.Where(d => d.IsPresent).ToList();
        Assert.Equal(5, present.Count);
        Assert.Equal(DriveState.Inserted, present[0].State);
        Assert.Equal("Blu-ray (AACS)", present[0].Flags.DiscTypeName());
        Assert.Equal("/dev/rdisk4", present[0].DevicePath);
        Assert.Equal("dev:/dev/rdisk4", present[0].LaneKey);
        Assert.Equal(DriveState.EmptyOpen, present[2].State);
        Assert.Equal(DriveState.Loading, present[3].State);
        Assert.Equal("DVD+RW Some \"Quoted\" Drive", present[4].DriveName);
        Assert.Equal("Disc, With Comma", present[4].DiscName);
    }

    [Fact]
    public void BuildsDiscInfoFromRealDvd()
    {
        var info = DiscInfoBuilder.Build(Fixtures.Read("info-dvd"));
        Assert.Equal(3, info.ReportedTitleCount);
        Assert.Equal(3, info.Titles.Count);
        Assert.Equal("dvd", info.TypeToken);
        Assert.Equal("One_Piece_S3_P1_D1", info.Name);
        var t0 = info.Title(0)!;
        Assert.Equal(50, t0.ChapterCount);
        Assert.Equal(2 * 3600 + 44 * 60 + 48, t0.DurationSeconds);
        Assert.Equal(8_075_685_888, t0.SizeBytes);
        Assert.Equal(11, t0.SourceTitleId);
        Assert.Equal("B1_t00.mkv", t0.OutputFileName);
        var t1 = info.Title(1)!;
        Assert.Equal(6, t1.Tracks.Count);
        Assert.Equal(TrackKind.Video, t1.Tracks[0].Kind);
        Assert.Equal("eng", t1.Tracks[1].LanguageCode);
        Assert.True(t1.Tracks[1].IsDefault);
        Assert.Equal("jpn", t1.Tracks[3].LanguageCode);
        Assert.Equal(TrackKind.Subtitle, t1.Tracks[5].Kind);
    }

    [Fact]
    public void FailureFixtureReportsErrors()
    {
        var errors = Fixtures.Read("rip-failure").Split('\n').Select(RobotParser.Parse).OfType<RobotEvent.Message>()
            .Select(m => m.Value).Where(m => m.Severity == Severity.Error).ToList();
        Assert.Equal(new[] { 2003, 5003, 5037 }, errors.Select(e => e.Code));
    }
}

public class TitleSelectorTests
{
    static TitleInfo T(int i, string dur, int ch = 10, long size = 1_000_000_000, int? src = null, string seg = "", string name = "", string file = "")
    {
        var t = new TitleInfo { Index = i };
        t.Attributes[(int)AttributeId.Duration] = dur;
        t.Attributes[(int)AttributeId.ChapterCount] = ch.ToString();
        t.Attributes[(int)AttributeId.DiskSizeBytes] = size.ToString();
        if (src != null) t.Attributes[(int)AttributeId.OriginalTitleId] = src.ToString()!;
        if (seg.Length > 0) t.Attributes[(int)AttributeId.SegmentsMap] = seg;
        if (name.Length > 0) t.Attributes[(int)AttributeId.Name] = name;
        if (file.Length > 0) t.Attributes[(int)AttributeId.SourceFileName] = file;
        return t;
    }

    static readonly List<TitleInfo> Titles = new()
    {
        T(0, "1:58:02", 24, 30_000_000_000, 800, "1,2,3", file: "00800.mpls"),
        T(1, "1:58:02", 24, 30_000_000_000, 801, "1,2,3", file: "00801.mpls"),
        T(2, "0:22:10", 5, 2_000_000_000, 10, file: "00010.mpls"),
        T(3, "0:23:15", 6, 2_100_000_000, 11, file: "00011.mpls"),
        T(4, "0:03:00", 1, 100_000_000, 20, name: "Trailer", file: "00020.mpls"),
    };

    [Fact]
    public void AllWithDuplicates()
    {
        var r = new TitleSelection();
        var res = TitleSelector.Evaluate(Titles, r);
        Assert.Equal(new[] { 0, 2, 3, 4 }, res.SelectedIndices);
        Assert.Equal("Duplicate of title 0", res.Decisions.First(d => d.TitleIndex == 1).Reason);
        r.SkipDuplicates = false;
        Assert.Equal(new[] { 0, 1, 2, 3, 4 }, TitleSelector.Evaluate(Titles, r).SelectedIndices);
    }

    [Fact]
    public void Longest()
    {
        var r = new TitleSelection { Strategy = TitleStrategy.Longest };
        Assert.Equal(new[] { 0 }, TitleSelector.Evaluate(Titles, r).SelectedIndices);
        r.LongestCount = 3;
        Assert.Equal(new[] { 0, 2, 3 }, TitleSelector.Evaluate(Titles, r).SelectedIndices);
    }

    [Fact]
    public void Filters()
    {
        Assert.Equal(new[] { 2, 3 }, TitleSelector.Evaluate(Titles, new TitleSelection { MinDurationSeconds = 1200, MaxDurationSeconds = 1800 }).SelectedIndices);
        Assert.Equal(new[] { 0, 1 }, TitleSelector.Evaluate(Titles, new TitleSelection { SkipDuplicates = false, IncludePattern = @"0080[01]\.mpls" }).SelectedIndices);
        Assert.Equal(new[] { 0, 1, 2, 3 }, TitleSelector.Evaluate(Titles, new TitleSelection { SkipDuplicates = false, ExcludePattern = "trailer" }).SelectedIndices);
        Assert.Equal(new[] { 0, 2 }, TitleSelector.Evaluate(Titles, new TitleSelection { MaxTitles = 2 }).SelectedIndices);
    }

    [Fact]
    public void IndexPatterns()
    {
        var r = new TitleSelection { Strategy = TitleStrategy.Indices, SkipDuplicates = false, IndexPattern = "0, 3-" };
        Assert.Equal(new[] { 0, 3, 4 }, TitleSelector.Evaluate(Titles, r).SelectedIndices);
        r.IndexPattern = "last";
        Assert.Equal(new[] { 4 }, TitleSelector.Evaluate(Titles, r).SelectedIndices);
        r.IndexBase = IndexBase.Source;
        r.IndexPattern = "800-801,11";
        Assert.Equal(new[] { 0, 1, 3 }, TitleSelector.Evaluate(Titles, r).SelectedIndices);
        r.IndexPattern = "x";
        Assert.NotNull(TitleSelector.Evaluate(Titles, r).Error);
    }

    [Fact]
    public void Manual()
    {
        var res = TitleSelector.Evaluate(Titles, new TitleSelection { Strategy = TitleStrategy.Manual });
        Assert.True(res.RequiresManualChoice);
        Assert.Empty(res.SelectedIndices);
    }
}

public class TemplateTests
{
    [Fact]
    public void Renders()
    {
        var v = new Dictionary<string, string> { ["disc"] = "MOVIE", ["n"] = "3", ["comment"] = "", ["title"] = "A/B: C" };
        Assert.Equal("MOVIE - 03", TemplateRenderer.Render("{disc} - {n:2}", v));
        Assert.Equal("MOVIE", TemplateRenderer.Render("{disc}{comment? ({comment})}", v));
        Assert.Equal("MOVIE #3", TemplateRenderer.Render("{disc}{n? #{n}}", v));
        Assert.Equal("{unknown}", TemplateRenderer.Render("{unknown}", v));
        Assert.Equal(Path.Combine("Movies", "A-B- C"), TemplateRenderer.RenderPath("Movies/{title}", v));
        Assert.Equal("MOVIE", TemplateRenderer.RenderPath("../{disc}/./", v));
    }

    [Fact]
    public void Sanitizes()
    {
        Assert.Equal("a-b-c-d-e-f-g-h-i-j", TemplateRenderer.SanitizeComponent("a:b/c\\d*e?f\"g<h>i|j"));
        Assert.Equal("name", TemplateRenderer.SanitizeComponent("  .name. "));
    }

    [Fact]
    public void SplitsArguments()
    {
        Assert.Equal(new[] { "-i", "my file.mkv", "--x=a b", "c d" }, ArgumentSplitter.Split("-i \"my file.mkv\" --x='a b' c\\ d"));
        Assert.Equal(new[] { @"C:\Scripts\x.ps1", "D:\\" }, ArgumentSplitter.Split(@"C:\Scripts\x.ps1 D:\\"));
        Assert.Empty(ArgumentSplitter.Split("  "));
    }

    [Fact]
    public void PostProcessInvocation()
    {
        var step = new PostProcessStep { Executable = "/bin/echo", Arguments = "--dir {outputDir} {files} \"{disc} done\"" };
        var (exe, args) = PostProcessor.BuildInvocation(step, new Dictionary<string, string> { ["outputDir"] = "/out/My Disc", ["disc"] = "My Disc" },
            new[] { "/out/a.mkv", "/out/b c.mkv" });
        Assert.Equal("/bin/echo", exe);
        Assert.Equal(new[] { "--dir", "/out/My Disc", "/out/a.mkv", "/out/b c.mkv", "My Disc done" }, args);
    }

    [Fact]
    public void RunConditions()
    {
        var s = new PostProcessStep { Executable = "x" };
        Assert.True(PostProcessor.ShouldRun(s, JobState.Succeeded));
        Assert.False(PostProcessor.ShouldRun(s, JobState.Failed));
        s.RunOn = RunCondition.Failure;
        Assert.True(PostProcessor.ShouldRun(s, JobState.Cancelled));
        s.RunOn = RunCondition.Always;
        s.Enabled = false;
        Assert.False(PostProcessor.ShouldRun(s, JobState.Succeeded));
    }
}

public class FileFormatTests
{
    [Fact]
    public void SettingsRoundTrip()
    {
        var s = SettingsConf.Parse("#\n# x\n#\napp_DestinationDir = \"C:\\Movies\"\napp_ExpertMode = \"1\"\r\n");
        Assert.Equal("C:\\Movies", s["app_DestinationDir"]);
        Assert.Equal("1", s["app_ExpertMode"]);
        Assert.Equal(s, SettingsConf.Parse(SettingsConf.Serialize(s)));
    }

    [Fact]
    public void ProfileIsWellFormed()
    {
        var xml = ProfileBuilder.Build(new GeneratedProfile { Name = "Test & <P>", SelectionRule = "-sel:all,+sel:(eng|jpn)&audio" });
        var doc = new XmlDocument();
        doc.LoadXml(xml);
        Assert.Contains("app_DefaultSelectionString=\"-sel:all,+sel:(eng|jpn)&amp;audio\"", xml);
        Assert.Contains("outputSettingsName=\"flac-best\"", xml);
        Assert.Contains(ProfileBuilder.MakemkvDefaultSelection, ProfileBuilder.Build(new GeneratedProfile()));
    }

    [Fact]
    public void EnvironmentBuildsArguments()
    {
        var home = Path.Combine(Path.GetTempPath(), "bromelia-test-" + Guid.NewGuid());
        try
        {
            var cfg = new AppConfig { RegistrationKey = "T-KEY", GlobalSettings = { ["dvd_MinimumTitleLength"] = "120", ["app_Proxy"] = "" } };
            var drive = new DriveConfig { Settings = { ["dvd_MinimumTitleLength"] = "30" }, Profile = { Mode = ProfileMode.Generated } };
            drive.Rip.MinLengthSeconds = 45;
            drive.Rip.CacheMB = 256;
            drive.Rip.DirectIO = false;
            var env = MakeMKVEnvironment.Prepare("/bin/echo", cfg, drive, home, new[] { "dvd_MinimumTitleLength" }, "+sel:all");
            Assert.Equal("30", env.Settings["dvd_MinimumTitleLength"]);
            Assert.Equal("T-KEY", env.Settings["app_Key"]);
            Assert.Equal("+sel:all", env.Settings["app_DefaultSelectionString"]);
            Assert.False(env.Settings.ContainsKey("app_Proxy"));
            Assert.True(File.Exists(env.ProfilePath));
            if (!MakeMKVEnvironment.UsesRegistry)
            {
                var dir = OperatingSystem.IsMacOS() ? Path.Combine(home, "Library", "MakeMKV") : Path.Combine(home, ".MakeMKV");
                var conf = SettingsConf.Parse(File.ReadAllText(Path.Combine(dir, "settings.conf")));
                Assert.Equal("30", conf["dvd_MinimumTitleLength"]);
                Assert.Equal(home, env.ProcessEnvironment!["HOME"]);
            }
            var src = new DiscSource.Drive(2, "E:");
            var info = env.InfoArguments(src, drive.Rip);
            Assert.Equal(new[] { "info", "dev:E:" }, info.TakeLast(2));
            Assert.Contains("--minlength=45", info);
            Assert.Contains("--cache=256", info);
            Assert.Contains("--directio=false", info);
            Assert.Contains("--noscan", info);
            Assert.Equal(new[] { "mkv", "iso:C:\\a b.iso", "all", "D:\\out" }, env.MkvArguments(new DiscSource.Iso("C:\\a b.iso"), "all", "D:\\out", null).TakeLast(4));
            Assert.Equal(new[] { "backup", "--decrypt", "disc:2", "D:\\bk" }, env.BackupArguments(src, true, "D:\\bk", null)!.TakeLast(4));
            Assert.Null(env.BackupArguments(new DiscSource.Folder("x"), false, "y", null));
        }
        finally { try { Directory.Delete(home, true); } catch (IOException) { } }
    }
}

public class ConfigurationTests
{
    [Fact]
    public void ReadsConfigWrittenByMacVersion()
    {
        const string json = """
        { "outputRoot": "/rips", "drives": [ { "id": "45C805B7-47BE-4674-A759-A6EAEA53521A", "name": "Left",
          "match": { "driveName": "BD-RE  HL-DT-ST" },
          "rip": { "mode": "backupThenMkv", "keepBackupAfterMKV": false, "titleSelection": { "strategy": "longest", "indexBase": "source" } },
          "profile": { "mode": "generated", "generated": { "lpcmMultichannel": "flac-fast", "useISO639Type2T": true } },
          "output": { "conflictPolicy": "uniqueSuffix" },
          "postProcess": [ { "executable": "/bin/true", "runOn": "always" } ], "futureField": 1 } ] }
        """;
        var cfg = ConfigJson.Parse(json);
        Assert.Equal("/rips", cfg.OutputRoot);
        Assert.Equal(10, cfg.PollIntervalSeconds);
        var d = cfg.Drives.Single();
        Assert.Equal(Guid.Parse("45C805B7-47BE-4674-A759-A6EAEA53521A"), d.Id);
        Assert.Equal(RipMode.BackupThenMkv, d.Rip.Mode);
        Assert.False(d.Rip.KeepBackupAfterMkv);
        Assert.Equal(TitleStrategy.Longest, d.Rip.TitleSelection.Strategy);
        Assert.Equal(IndexBase.Source, d.Rip.TitleSelection.IndexBase);
        Assert.True(d.Rip.TitleSelection.SkipDuplicates);
        Assert.Equal(ProfileMode.Generated, d.Profile.Mode);
        Assert.Equal(LpcmOutput.FlacFast, d.Profile.Generated.LpcmMultichannel);
        Assert.True(d.Profile.Generated.UseIso639Type2T);
        Assert.Equal(RunCondition.Always, d.PostProcess[0].RunOn);
        Assert.True(d.Automation.EjectWhenDone);
    }

    [Fact]
    public void WritesSameShapeAsMacVersion()
    {
        var cfg = new AppConfig();
        cfg.Drives.Add(new DriveConfig { Name = "Right" });
        cfg.Drives[0].Rip.Mode = RipMode.BackupDecrypted;
        var json = ConfigJson.Serialize(cfg);
        Assert.Contains("\"mode\": \"backupDecrypted\"", json);
        Assert.Contains("\"lpcmMultichannel\": \"flac-best\"", json);
        Assert.Contains("\"keepBackupAfterMKV\": true", json);
        Assert.Contains("\"makemkvconPath\"", json);
        Assert.Contains("\"minSizeMB\"", json);
        Assert.DoesNotContain("minLengthSeconds", json);
        var back = ConfigJson.Parse(json);
        Assert.Equal(RipMode.BackupDecrypted, back.Drives[0].Rip.Mode);
    }

    [Fact]
    public void MatchesDrives()
    {
        var e = new DriveScanEntry(0, DriveState.Inserted, DiscFlags.None, "BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325", "", "E:");
        Assert.True(new DriveMatch { DriveName = "bd-re hl-dt-st bd-re wh16ns60 1.02 klam6e84325" }.Matches(e));
        Assert.False(new DriveMatch { DriveName = "BD-RE ASUS" }.Matches(e));
        Assert.True(new DriveMatch { DevicePath = "e:" }.Matches(e));
    }

    [Fact]
    public void EffectiveSettingsOverlay()
    {
        var cfg = new AppConfig { GlobalSettings = { ["a"] = "1", ["b"] = "2" } };
        var d = new DriveConfig { Settings = { ["b"] = "3", ["c"] = "4" } };
        Assert.Equal(new Dictionary<string, string> { ["a"] = "1", ["b"] = "3", ["c"] = "4" }, cfg.EffectiveSettings(d));
    }

    [Fact]
    public void CatalogLoads()
    {
        var c = SettingsCatalog.Load();
        Assert.Contains(c.AllSettings, s => s.Key == "dvd_MinimumTitleLength");
        Assert.NotEmpty(c.SelectionPresets);
    }
}

public class RemuxTests
{
    [Fact]
    public void BuildsTrackArguments()
    {
        var title = DiscInfoBuilder.Build(Fixtures.Read("info-dvd")).Title(1)!;
        var layout = new List<Remuxer.TrackLayout> { new(0, "video"), new(1, "audio"), new(2, "audio"), new(3, "audio"), new(4, "audio"), new(5, "subtitles") };
        Assert.Equal(new[] { "-o", "out.mkv", "--video-tracks", "0", "--audio-tracks", "1,3", "--no-subtitles", "in.mkv" },
            Remuxer.Arguments(layout, title, new HashSet<int> { 0, 1, 3 }, "in.mkv", "out.mkv"));
        Assert.Null(Remuxer.Arguments(layout.Take(5).ToList(), title, new HashSet<int> { 0 }, "i", "o"));
    }
}

/// <summary>Runs async code on a single thread with a message loop, like a UI thread.</summary>
public sealed class SingleThreadContext : SynchronizationContext
{
    readonly BlockingCollection<(SendOrPostCallback, object?)> _queue = new();

    public override void Post(SendOrPostCallback d, object? state) => _queue.Add((d, state));

    public static void Run(Func<Task> func)
    {
        var prev = Current;
        var ctx = new SingleThreadContext();
        SetSynchronizationContext(ctx);
        try
        {
            var task = func();
            task.ContinueWith(_ => ctx._queue.CompleteAdding(), TaskScheduler.Default);
            foreach (var (d, s) in ctx._queue.GetConsumingEnumerable()) d(s);
            task.GetAwaiter().GetResult();
        }
        finally { SetSynchronizationContext(prev); }
    }
}

/// <summary>End-to-end test with a real disc image; set BROMELIA_TEST_ISO to run it.</summary>
public class IntegrationTests
{
    [Fact]
    public void RipsTitleWithCustomTracksRenameAndScript()
    {
        var iso = Environment.GetEnvironmentVariable("BROMELIA_TEST_ISO");
        if (string.IsNullOrEmpty(iso) || OperatingSystem.IsWindows()) return;
        var exe = Paths.ResolveTool("", Paths.MakemkvconCandidates(), "makemkvcon");
        if (exe == null) return;
        var mkvmerge = Paths.ResolveTool("", Paths.MkvmergeCandidates(), "mkvmerge");
        var outDir = Path.Combine(Path.GetTempPath(), "bromelia-it-" + Guid.NewGuid().ToString("N")[..6]);
        Paths.DataOverride = Path.Combine(outDir, ".data");
        var marker = Path.Combine(outDir, "post.txt");
        try
        {
            var config = new AppConfig { OutputRoot = outDir };
            var drive = new DriveConfig { Name = "Test", Output = { FolderTemplate = "{type}/{disc}", FileNameTemplate = "{disc} - {n:2}" } };
            drive.Automation.Notify = false;
            drive.PostProcess.Add(new PostProcessStep
            {
                Executable = "/bin/sh",
                Arguments = $"-c 'echo \"$BROMELIA_STATUS|$BROMELIA_FILE_COUNT\" > \"$0\"' {marker}",
            });
            var job = new RipJob(new DiscSource.Iso(iso), drive, "iso:t", "test", "", RipMode.Mkv) { ManualTitles = new List<int> { 1 } };
            if (mkvmerge != null) job.TrackSelections[1] = new HashSet<int> { 0, 1, 5 };
            SingleThreadContext.Run(async () =>
            {
                var runner = new JobRunner(job, config, exe, mkvmerge, new UiDispatcher(), new NullPlatformServices(), new[] { "dvd_MinimumTitleLength" });
                await runner.RunAsync();
            });
            Assert.True(job.State == JobState.Succeeded, job.ErrorMessage + "\n" + string.Join("\n", job.Log.TakeLast(15).Select(l => l.Text)));
            var file = Assert.Single(job.ProducedFiles);
            Assert.Equal($"{job.DiscLabel} - 01.mkv", Path.GetFileName(file));
            Assert.True(File.Exists(file));
            if (mkvmerge != null)
            {
                var layout = Remuxer.IdentifyAsync(mkvmerge, file).GetAwaiter().GetResult();
                Assert.Equal(3, layout!.Count);
            }
            Assert.StartsWith("success|1", File.ReadAllText(marker));
        }
        finally { try { Directory.Delete(outDir, true); } catch (IOException) { } }
    }
}

public class DriveDetectionTests
{
    static DriveScanEntry E(int i, DriveState s, string name, string dev, string disc = "") =>
        new(i, s, s == DriveState.Inserted ? DiscFlags.BlurayFiles : DiscFlags.None, name, disc, dev);

    [Fact]
    public void AutoRipQueuesOnInsertOnly()
    {
        var dir = Path.Combine(Path.GetTempPath(), "bromelia-dd-" + Guid.NewGuid().ToString("N")[..6]);
        Paths.DataOverride = dir;
        try
        {
            var state = new AppState(new NullPlatformServices(), Path.Combine(dir, "config.json"), new SettingsCatalog());
            var left = new DriveConfig { Name = "Left", Match = { DriveName = "BD-RE TEST DRIVE 1.00 SERIAL1" } };
            left.Automation.AutoRipOnInsert = true;
            left.Automation.AutoRipDelaySeconds = 60;
            left.Rip.Mode = RipMode.BackupThenMkv;
            state.Config.Drives.Add(left);

            var emptyRight = E(1, DriveState.EmptyClosed, "DVD OTHER 2.00", "F:");
            state.ApplyScan(new[] { E(0, DriveState.Inserted, "BD-RE  TEST DRIVE 1.00 SERIAL1", "E:", "OLD"), emptyRight });
            Assert.Empty(state.Jobs);
            state.ApplyScan(new[] { E(0, DriveState.EmptyClosed, "BD-RE  TEST DRIVE 1.00 SERIAL1", "E:"), emptyRight });
            Assert.Empty(state.Jobs);

            var inserted = E(0, DriveState.Inserted, "BD-RE  TEST DRIVE 1.00 SERIAL1", "E:", "MOVIE");
            state.ApplyScan(new[] { inserted, emptyRight });
            var job = Assert.Single(state.Jobs);
            Assert.Equal(JobState.Waiting, job.State);
            Assert.True(job.IsAutomatic);
            Assert.Equal(RipMode.BackupThenMkv, job.Mode);
            Assert.Equal("Left", job.Drive.Name);
            Assert.Equal("dev:E:", job.LaneKey);

            state.ApplyScan(new[] { inserted, emptyRight });
            Assert.Single(state.Jobs);
            state.ApplyScan(new[] { inserted, E(1, DriveState.Inserted, "DVD OTHER 2.00", "F:", "X") });
            Assert.Single(state.Jobs);

            var items = state.DriveItems;
            Assert.Equal(2, items.Count);
            Assert.Equal("Left", items.First(i => i.Entry?.DevicePath == "E:").Config?.Name);
            Assert.Null(items.First(i => i.Entry?.DevicePath == "F:").Config);

            state.Cancel(job);
            Assert.Equal(JobState.Cancelled, job.State);
            Assert.Equal(job.Id, state.History[0].Id);
        }
        finally
        {
            Paths.DataOverride = null;
            try { Directory.Delete(dir, true); } catch (IOException) { }
        }
    }
}
