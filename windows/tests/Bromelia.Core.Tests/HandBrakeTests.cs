using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Xunit;

namespace Bromelia.Core.Tests;

public class HandBrakeTests
{
    [Fact]
    public void Arguments()
    {
        var s = PostProcessStep.HandBrake();
        Assert.True(s.Kind == StepKind.Handbrake && s.Background && s.Preset == "H.265 MKV 1080p30");
        Assert.True(PostProcessor.ShouldRun(s, JobState.Succeeded) && !PostProcessor.ShouldRun(s, JobState.Failed));
        s.PresetFile = "/p/archive.json";
        s.ExtraArguments = "--all-subtitles --subtitle-burned=none";
        var values = new Dictionary<string, string> { ["outputDir"] = "/out/Show", ["stem"] = "Show - S01E01", ["name"] = "Show" };
        var output = HandBrake.Output(s, values);
        Assert.Equal(Path.GetFullPath("/out/Show/Encoded/Show - S01E01.mkv"), output);
        Assert.Equal(new[] { "--preset-import-file", "/p/archive.json", "--preset", "H.265 MKV 1080p30", "-i", "/out/Show/Show - S01E01.mkv",
                             "-o", output, "--all-subtitles", "--subtitle-burned=none" },
            HandBrake.Arguments(s, "/out/Show/Show - S01E01.mkv", output));
        Assert.True(HandBrake.IsSource("/x/a.MKV") && !HandBrake.IsSource("/x/a.iso"));
        var f = new HandBrake.ProgressFilter();
        var kept = new[] { "Encoding: task 1 of 2, 0.50 % (0.0 fps)", "Encoding: task 1 of 2, 3.00 %", "Encoding: task 1 of 2, 10.01 %",
                           "Encoding: task 1 of 2, 19.99 %", "x264 [info]: done", "Encoding: task 2 of 2, 1.00 %", "Encoding: task 2 of 2, 95.00 %" }
            .Where(f.Keep).ToArray();
        Assert.Equal(new[] { "Encoding: task 1 of 2, 0.50 % (0.0 fps)", "Encoding: task 1 of 2, 10.01 %", "x264 [info]: done",
                             "Encoding: task 2 of 2, 1.00 %", "Encoding: task 2 of 2, 95.00 %" }, kept);
        Assert.Equal("\"handbrake\"", System.Text.Json.JsonSerializer.Serialize(StepKind.Handbrake, ConfigJson.Options));
    }

    /// <summary>A stand-in HandBrakeCLI copies the input to the output: each MKV gets an encode, an existing one is kept.</summary>
    [Fact]
    public async Task EncodesEveryMkvWithoutReplacingAnything()
    {
        if (OperatingSystem.IsWindows()) return;
        var dir = Path.Combine(Path.GetTempPath(), "bromelia-hb-" + Guid.NewGuid().ToString("N")[..6]);
        Directory.CreateDirectory(Path.Combine(dir, "Encoded"));
        try
        {
            var tool = Path.Combine(dir, "HandBrakeCLI");
            File.WriteAllText(tool, "#!/bin/sh\nwhile [ $# -gt 0 ]; do case $1 in -i) i=$2;; -o) o=$2;; esac; shift; done\necho 'Encoding: task 1 of 1, 50.00 %'\ncp \"$i\" \"$o\"\n");
            File.SetUnixFileMode(tool, (UnixFileMode)0b111_101_101);
            string a = Path.Combine(dir, "A.mkv"), b = Path.Combine(dir, "B.mkv"), iso = Path.Combine(dir, "C.iso");
            File.WriteAllText(a, "a"); File.WriteAllText(b, "b"); File.WriteAllText(iso, "c");
            File.WriteAllText(Path.Combine(dir, "Encoded", "A.mkv"), "old");
            var s = PostProcessStep.HandBrake();
            s.Executable = tool;
            var ctx = new PostProcessor.Context(JobState.Succeeded, new Dictionary<string, string> { ["outputDir"] = dir }, dir, new[] { a, b, iso },
                new Dictionary<string, string>());
            var log = new List<string>();
            var results = await PostProcessor.RunAsync(new[] { s }, ctx, _ => { }, (t, _) => { lock (log) log.Add(t); });
            Assert.True(results.Count == 2 && results.All(r => r.ExitCode == 0), string.Join("\n", log));
            Assert.Equal("old", File.ReadAllText(Path.Combine(dir, "Encoded", "A.mkv")));
            Assert.Equal("a", File.ReadAllText(Path.Combine(dir, "Encoded", "A (2).mkv")));
            Assert.Equal("b", File.ReadAllText(Path.Combine(dir, "Encoded", "B.mkv")));
            Assert.False(File.Exists(Path.Combine(dir, "Encoded", "C.mkv")));
            s.Executable = Path.Combine(dir, "missing");
            var missing = await PostProcessor.RunAsync(new[] { s }, ctx, _ => { }, (t, _) => { });
            Assert.True(missing.Count == 1 && missing[0].ExitCode == -1);
        }
        finally { Directory.Delete(dir, true); }
    }
}
