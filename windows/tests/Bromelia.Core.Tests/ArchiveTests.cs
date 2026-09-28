using System.Text;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Bromelia.Core.Logic;
using Bromelia.Core.Robot;
using Xunit;

namespace Bromelia.Core.Tests;

public class IdentityTests
{
    [Theory]
    [InlineData("ONE_PIECE_S2_P7_D2", "One Piece", 2, 7, null, 2, true)]
    [InlineData("One_Piece_S3_P1_D1", "One Piece", 3, 1, null, 1, true)]
    [InlineData("THE_LORD_OF_THE_RINGS_DISC_2", "The Lord of the Rings", null, null, null, 2, false)]
    [InlineData("FRIENDS_SEASON_4_DISC_3", "Friends", 4, null, null, 3, true)]
    [InlineData("BREAKING_BAD_S1D2", "Breaking Bad", 1, null, null, 2, true)]
    [InlineData("NARUTO_VOL_12", "Naruto", null, null, 12, null, true)]
    [InlineData("BLADE_RUNNER_2049", "Blade Runner 2049", null, null, null, null, false)]
    [InlineData("ROCKY_II_WS", "Rocky II", null, null, null, null, false)]
    [InlineData("SPIDER-MAN_NO_WAY_HOME", "Spider-Man No Way Home", null, null, null, null, false)]
    [InlineData("The Matrix - Disc 1", "The Matrix", null, null, null, 1, false)]
    public void ParsesLabels(string label, string title, int? season, int? part, int? volume, int? disc, bool series)
    {
        var l = LabelParser.Parse(label);
        Assert.Equal(title, l.Title);
        Assert.Equal(season, l.Season);
        Assert.Equal(part, l.Part);
        Assert.Equal(volume, l.Volume);
        Assert.Equal(disc, l.Disc);
        Assert.Equal(series, l.LooksLikeSeries);
    }

    [Fact]
    public void ResolvesDvdFixture()
    {
        var info = DiscInfoBuilder.Build(Fixtures.Read("info-dvd"));
        var id = MediaIdentity.Resolve(info, "", false);
        Assert.Equal("One Piece", id.Name);
        Assert.Equal(MediaKind.Tv, id.Kind);
        Assert.Equal("DVD", id.FormatCode);
        Assert.Equal("Season 3 Part 1 Disc 1", id.Label.SetDescription);
        Assert.Equal("DVDe", MediaIdentity.Resolve(info, "", true).FormatCode);
        Assert.Equal("One Piece (2001)", MediaIdentity.Resolve(info, "", false, nameOverride: "One Piece (2001)").Name);
    }

    [Fact]
    public void PrefersBlurayMetadataTitleAndDetectsUhd()
    {
        var info = new DiscInfo();
        info.Attributes[(int)AttributeId.Type] = "Blu-ray disc";
        info.Attributes[(int)AttributeId.Name] = "The Dark Knight™";
        info.Attributes[(int)AttributeId.VolumeName] = "DARK_KNIGHT_D1";
        var id = MediaIdentity.Resolve(info, "", false);
        Assert.Equal("The Dark Knight", id.Name);
        Assert.Equal(MediaKind.Movie, id.Kind);
        Assert.Equal("BR", id.FormatCode);
        Assert.Equal(1, id.Label.Disc);

        var t = new TitleInfo { Index = 0 };
        var v = new TrackInfo { Index = 0 };
        v.Attributes[(int)AttributeId.Type] = "Video";
        v.Attributes[(int)AttributeId.VideoSize] = "3840x2160";
        t.Tracks.Add(v);
        info.Titles.Add(t);
        Assert.Equal(DiscFormat.Uhd, DiscFormatExtensions.Detect(info));
        Assert.Equal("4Ke", DiscFormat.Uhd.Code(true));
        Assert.Equal(DiscFormat.Bluray, DiscFormatExtensions.Detect(null, DiscFlags.BlurayFiles | DiscFlags.AacsFiles));
        Assert.Equal(DiscFormat.Unknown, DiscFormatExtensions.Detect(null));
    }

    [Fact]
    public void DetectsUhdBackupFolder()
    {
        var dir = Path.Combine(Path.GetTempPath(), "bromelia-bdmv-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(Path.Combine(dir, "BDMV"));
            File.WriteAllText(Path.Combine(dir, "BDMV", "index.bdmv"), "INDX0300xxxxxxxx");
            Assert.Equal(DiscFormat.Uhd, DiscFormatExtensions.DetectBackupFolder(dir));
            File.WriteAllText(Path.Combine(dir, "BDMV", "index.bdmv"), "INDX0200xxxxxxxx");
            Assert.Equal(DiscFormat.Bluray, DiscFormatExtensions.DetectBackupFolder(dir));
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void TvShowFromEpisodeLengthTitles()
    {
        var info = new DiscInfo();
        info.Attributes[(int)AttributeId.Type] = "Blu-ray disc";
        info.Attributes[(int)AttributeId.VolumeName] = "SOME_SHOW";
        int i = 0;
        foreach (var d in new[] { 2710, 2650, 2690, 2705, 300 })
        {
            var t = new TitleInfo { Index = i++ };
            t.Attributes[(int)AttributeId.Duration] = TitleInfo.FormatDuration(d);
            info.Titles.Add(t);
        }
        Assert.Equal(new[] { 0, 1, 2, 3 }, MediaIdentity.EpisodeLikeTitles(info.Titles).Select(t => t.Index));
        Assert.Equal(MediaKind.Tv, MediaIdentity.Resolve(info, "", false).Kind);
    }

    [Fact]
    public void TrackLabels()
    {
        var dvd = new TitleInfo { Index = 0 };
        dvd.Attributes[(int)AttributeId.OriginalTitleId] = "11";
        Assert.Equal("Title 11", MediaIdentity.TrackLabel(dvd));
        var bd = new TitleInfo { Index = 3 };
        bd.Attributes[(int)AttributeId.SourceFileName] = "00800.mpls";
        Assert.Equal("Playlist 00800", MediaIdentity.TrackLabel(bd));
        Assert.Equal("Episode 007", MediaIdentity.EpisodeLabel(7, 3));
    }
}

public class NamingTests
{
    static string Render(MediaIdentity id, string rip, params (string K, string V)[] extra)
    {
        var v = id.TemplateValues(rip);
        foreach (var (k, x) in extra) v[k] = x;
        return TemplateRenderer.RenderPath(OutputConfig.DefaultFileNameTemplate, v);
    }

    [Fact]
    public void DefaultTemplate()
    {
        var tv = new MediaIdentity("One Piece", MediaKind.Tv, DiscFormat.Dvd, false, LabelParser.Parse("ONE_PIECE_S2_P7_D2"), "");
        Assert.Equal("One Piece - Episode 138 - Season 2 Part 7 Disc 2 - Rip - Title 11 Ch 1-7 - DVD",
            Render(tv, "Rip", ("episode", "Episode 138"), ("track", "Title 11 Ch 1-7")));
        Assert.Equal("One Piece - Season 2 Part 7 Disc 2 - Backup - DVDe", Render(tv with { Encrypted = true }, "Backup"));
        var movie = new MediaIdentity("Inception", MediaKind.Movie, DiscFormat.Uhd, false, new LabelInfo(), "");
        Assert.Equal("Inception - Rip - Playlist 00800 - 4K", Render(movie, "Rip", ("track", "Playlist 00800")));
        Assert.Equal("Inception", TemplateRenderer.RenderPath(OutputConfig.DefaultFolderTemplate, movie.TemplateValues("Rip")));
    }

    [Fact]
    public void PluginMatching()
    {
        var step = new PostProcessStep();
        Assert.True(PluginMatcher.Matches(step, "Anything", "X", "BR"));
        step.MatchName = "^one piece$";
        Assert.True(PluginMatcher.Matches(step, "One Piece", "ONE_PIECE_S2", "DVD"));
        Assert.False(PluginMatcher.Matches(step, "Naruto", "NARUTO", "DVD"));
        step.MatchName = "S2_P7";
        Assert.True(PluginMatcher.Matches(step, "One Piece", "ONE_PIECE_S2_P7_D2", "DVD"));
        step.MatchName = "";
        step.MatchFormats = new List<string> { "BR*", "4K" };
        Assert.True(PluginMatcher.Matches(step, "", "", "BRe"));
        Assert.True(PluginMatcher.Matches(step, "", "", "4K"));
        Assert.False(PluginMatcher.Matches(step, "", "", "4Ke"));
        Assert.False(PluginMatcher.Matches(step, "", "", "DVD"));
        step.MatchName = "(";
        Assert.False(PluginMatcher.Matches(step, "x", "", "BR"));
        Assert.NotNull(PluginMatcher.Validate("("));
    }

    [Fact]
    public void UpgradesVersion1Naming()
    {
        var c = ConfigJson.Parse("""
            {"version":1,"defaultDrive":{"output":{"folderTemplate":"{disc}","fileNameTemplate":""}},
             "drives":[{"name":"A","output":{"folderTemplate":"{type}/{disc}","fileNameTemplate":"{disc} {n}"}}],
             "plugins":[{"name":"P","matchName":"piece","matchFormats":["DVD"]}]}
            """);
        Assert.Equal(2, c.Version);
        Assert.Equal(OutputConfig.DefaultFolderTemplate, c.DefaultDrive.Output.FolderTemplate);
        Assert.Equal(OutputConfig.DefaultFileNameTemplate, c.DefaultDrive.Output.FileNameTemplate);
        Assert.Equal("{type}/{disc}", c.Drives[0].Output.FolderTemplate);
        Assert.Equal("{disc} {n}", c.Drives[0].Output.FileNameTemplate);
        Assert.Equal(new[] { "DVD" }, c.Plugins[0].MatchFormats);
        Assert.True(c.DefaultDrive.Archive.Checksums);
        Assert.True(c.DefaultDrive.Episodes.SplitPlayAll);
        Assert.Equal("", ConfigJson.Parse("""{"version":2,"defaultDrive":{"output":{"fileNameTemplate":""}}}""").DefaultDrive.Output.FileNameTemplate);
        Assert.Equal(OutputConfig.DefaultFileNameTemplate, ConfigJson.Parse("""{"defaultDrive":{"output":{"fileNameTemplate":""}}}""").DefaultDrive.Output.FileNameTemplate);
        Assert.Equal(2, ConfigJson.Parse(ConfigJson.Serialize(new AppConfig())).Version);
    }
}

public class ChecksumTests
{
    [Fact]
    public void HashesFilesAndFolders()
    {
        var dir = Path.Combine(Path.GetTempPath(), "bromelia-sums-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(Path.Combine(dir, "backup", "VIDEO_TS"));
            File.WriteAllText(Path.Combine(dir, "a.mkv"), "abc");
            File.WriteAllBytes(Path.Combine(dir, "backup", "VIDEO_TS", "VIDEO_TS.IFO"), Array.Empty<byte>());
            var files = Checksums.Files(new[] { Path.Combine(dir, "a.mkv"), Path.Combine(dir, "backup") }, dir);
            Assert.Equal(new[] { "a.mkv", "backup/VIDEO_TS/VIDEO_TS.IFO" }, files.Select(f => f.Relative));
            Assert.Equal("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", Checksums.Sha256(Path.Combine(dir, "a.mkv")));
            Checksums.WriteMerged(dir, files.Select(f => new Checksums.Entry(f.Relative, 0, Checksums.Sha256(f.Full))));
            Assert.Empty(Checksums.Verify(dir));
            File.WriteAllText(Path.Combine(dir, "a.mkv"), "abd");
            Assert.Equal(new[] { "a.mkv" }, Checksums.Verify(dir));
            var text = File.ReadAllText(Path.Combine(dir, Checksums.FileName));
            Assert.StartsWith("ba7816bf", text);
            Assert.Contains("  backup/VIDEO_TS/VIDEO_TS.IFO\n", text);
        }
        finally { Directory.Delete(dir, true); }
    }
}

public class DvdNavigationTests
{
    static string Fixture => Path.Combine(AppContext.BaseDirectory, "fixtures", "dvd-play-all");

    [Fact]
    public void FindsPlayAllEpisodes()
    {
        using var reader = FolderVideoTs.Open(Fixture, "ONE_PIECE_S2_P7_D2")!;
        Assert.NotNull(reader);
        var plan = DvdNavigation.Plans(DvdNavigation.Analyse(reader)!).First();
        Assert.Equal(11, plan.Title);
        Assert.Equal(new[] { 1, 8, 15, 22, 29, 36 }, plan.Starts);
        Assert.Equal(42, plan.LastEnd);
        Assert.Equal("VOB boundary", plan.EndRule);
        Assert.Equal(new[] { 43 }, plan.Tail);
        Assert.Equal(new[] { 8, 15, 22, 29, 36, 43 }, plan.SplitChapters);
        Assert.Equal("2:21:51.370", DvdNavigation.Hms(plan.Duration));
        Assert.Equal("0:23:36.815", DvdNavigation.Hms(plan.ChapterStarts[7]));
        Assert.Equal((36, 42), plan.ChapterRange(5));
        Assert.True(plan.IsPlausible(true));
        Assert.True(plan.MatchesChapterCount(43));
        Assert.Equal("if GPRM7 == 21: LinkPTTN 8", plan.Reasons[1]);
    }

    [Fact]
    public void DecodesJumps()
    {
        Assert.Equal(new DvdNavigation.Jump(true, 3, 5, ""), DvdNavigation.DecodeJump(new byte[] { 0x30, 0x05, 0x00, 0x05, 0x00, 0x03, 0x00, 0x00 }));
        Assert.Equal(new DvdNavigation.Jump(true, null, 8, "if GPRM7 == 21: "), DvdNavigation.DecodeJump(new byte[] { 0x20, 0xA5, 0x00, 0x07, 0x00, 0x15, 0x00, 0x08 }));
        Assert.Equal(new DvdNavigation.Jump(false, null, 4, ""), DvdNavigation.DecodeJump(new byte[] { 0x30, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00 }));
        Assert.Null(DvdNavigation.DecodeJump(new byte[8]));
    }

    [Fact]
    public void PicksFirstEpisodeAndMapsChapters()
    {
        Assert.Equal(138, DvdNavigation.FirstEpisode(new HashSet<int> { 138, 139, 140, 142, 143 }, 6));
        Assert.Null(DvdNavigation.FirstEpisode(new HashSet<int> { 5 }, 6));
        Assert.Equal(new HashSet<int> { 138, 12, 9 }, DvdNavigation.EpisodeNumbersInText("EPISODE 138\nEpis0de #12 foo episode9"));
        using var reader = FolderVideoTs.Open(Fixture)!;
        var plan = DvdNavigation.Plans(DvdNavigation.Analyse(reader)!).First();
        var shifted = new[] { 0.0 }.Concat(plan.ChapterStarts.Select(s => s + 0.02)).ToList();
        Assert.Equal(new[] { 9, 16, 23, 30, 37, 44 }, DvdNavigation.MkvChapters(plan.SplitChapters, plan, shifted));
        Assert.Null(DvdNavigation.MkvChapters(new[] { 8 }, plan, new[] { 0.0, 10, 20 }));
        Assert.Equal(new[] { 0, 1416.815 }, EpisodeSplitter.ParseSimpleChapters("CHAPTER01=00:00:00.000\nCHAPTER01NAME=x\nCHAPTER02=00:23:36.815\n"));
    }

    /// <summary>Set BROMELIA_TEST_DVD_ISO to One_Piece_S2_P7_D2.iso to read the same disc from the ISO (and OCR its menus).</summary>
    [Fact]
    public void ReadsIso()
    {
        var iso = Environment.GetEnvironmentVariable("BROMELIA_TEST_DVD_ISO");
        if (string.IsNullOrEmpty(iso)) return;
        using var reader = IsoVideoTs.Open(iso)!;
        Assert.Equal("ONE_PIECE_S2_P7_D2", reader.Label);
        var plan = DvdNavigation.Plans(DvdNavigation.Analyse(reader)!).First();
        Assert.Equal(new[] { 1, 8, 15, 22, 29, 36 }, plan.Starts);
        var (numbers, _) = EpisodeSplitter.OcrEpisodeNumbersAsync(reader, default).GetAwaiter().GetResult();
        if (numbers != null) Assert.Equal(138, DvdNavigation.FirstEpisode(numbers, plan.EpisodeCount));
    }

    /// <summary>Rips title 11 of One_Piece_S2_P7_D2 and checks the split into named episodes (BROMELIA_TEST_PLAYALL_ISO).</summary>
    [Fact]
    public void SplitsPlayAllTitleIntoEpisodes()
    {
        var iso = Environment.GetEnvironmentVariable("BROMELIA_TEST_PLAYALL_ISO");
        if (string.IsNullOrEmpty(iso) || OperatingSystem.IsWindows()) return;
        var exe = Paths.ResolveTool("", Paths.MakemkvconCandidates(), "makemkvcon");
        var mkvmerge = Paths.ResolveTool("", Paths.MkvmergeCandidates(), "mkvmerge");
        if (exe == null || mkvmerge == null) return;
        var outDir = Path.Combine(Path.GetTempPath(), "bromelia-split-" + Guid.NewGuid().ToString("N")[..6]);
        Paths.DataOverride = Path.Combine(outDir, ".data");
        try
        {
            var config = new AppConfig { OutputRoot = outDir };
            var drive = new DriveConfig();
            drive.Automation.Notify = false;
            drive.Rip.TitleSelection = new TitleSelection { Strategy = TitleStrategy.Indices, IndexBase = IndexBase.Source, IndexPattern = "11" };
            drive.Episodes.KeepPlayAll = false;
            config.Plugins.Add(new PostProcessStep { Name = "One Piece DVDs", Executable = "/bin/sh", MatchName = "one piece", MatchFormats = new() { "DVD" },
                Arguments = "-c 'echo \"$BROMELIA_NAME|$BROMELIA_FORMAT|$BROMELIA_FILE_COUNT\" > plugin.txt'" });
            config.Plugins.Add(new PostProcessStep { Name = "Blu-ray only", Executable = "/bin/sh", MatchFormats = new() { "BR*" }, Arguments = "-c 'touch wrong.txt'" });
            var job = new RipJob(new DiscSource.Iso(iso), drive, "iso:split", "test", "", RipMode.Mkv);
            SingleThreadContext.Run(async () =>
                await new JobRunner(job, config, exe, mkvmerge, new UiDispatcher(), new NullPlatformServices(), new[] { "dvd_MinimumTitleLength" }).RunAsync());
            Assert.True(job.State == JobState.Succeeded, job.ErrorMessage + "\n" + string.Join("\n", job.Log.TakeLast(20).Select(l => l.Text)));
            var dir = job.OutputDirectory!;
            Assert.Equal("One Piece - Season 2 Part 7 Disc 2", Path.GetFileName(dir));
            var expected = Enumerable.Range(0, 6).Select(i => $"One Piece - Episode {138 + i} - Season 2 Part 7 Disc 2 - Rip - Title 11 Ch {1 + 7 * i}-{7 + 7 * i} - DVD.mkv")
                .Append("One Piece - Season 2 Part 7 Disc 2 - Rip - Title 11 Ch 43 - DVD.mkv");
            Assert.Equal(expected, job.ProducedFiles.Select(Path.GetFileName));
            Assert.Equal(7, Checksums.Parse(File.ReadAllText(Path.Combine(dir, Checksums.FileName))).Count);
            Assert.Empty(Checksums.Verify(dir));
            Assert.True(File.Exists(Path.Combine(dir, "bromelia.json")));
            Assert.Equal("One Piece|DVD|7\n", File.ReadAllText(Path.Combine(dir, "plugin.txt")));
            Assert.False(File.Exists(Path.Combine(dir, "wrong.txt")));
        }
        finally { try { Directory.Delete(outDir, true); } catch (IOException) { } }
    }
}
