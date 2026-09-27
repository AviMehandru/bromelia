using System.Text.Json;
using Bromelia.Core.Config;
using Bromelia.Core.Logic;
using Bromelia.Core.Robot;

namespace Bromelia.Core.Engine;

/// <summary>Runs a drive's post-processing steps after a job.</summary>
public static class PostProcessor
{
    public sealed record Context(JobState Status, IReadOnlyDictionary<string, string> Values, string? OutputDirectory,
        IReadOnlyList<string> Files, IReadOnlyDictionary<string, string> Environment);

    public sealed record StepResult(Guid StepId, string Name, int ExitCode, bool TimedOut);

    public static bool ShouldRun(PostProcessStep step, JobState status)
    {
        if (!step.Enabled || string.IsNullOrWhiteSpace(step.Executable)) return false;
        return step.RunOn switch
        {
            RunCondition.Always => true,
            RunCondition.Success => status == JobState.Succeeded,
            _ => status is JobState.Failed or JobState.Cancelled,
        };
    }

    /// <summary>Builds argv. Arguments are split first and rendered afterwards, so values with spaces stay
    /// single arguments; a lone {files} expands to one argument per file. Scripts are run through a
    /// suitable interpreter based on their extension when no interpreter is configured.</summary>
    public static (string Executable, List<string> Arguments) BuildInvocation(PostProcessStep step, IReadOnlyDictionary<string, string> values, IReadOnlyList<string> files)
    {
        var args = new List<string>();
        foreach (var token in ArgumentSplitter.Split(step.Arguments))
        {
            if (token == "{files}") args.AddRange(files);
            else args.Add(TemplateRenderer.Render(token, values));
        }
        var exe = Paths.ExpandUser(TemplateRenderer.Render(step.Executable, values));
        var interp = Paths.ExpandUser(step.Interpreter);
        if (interp.Length > 0) return (interp, new[] { exe }.Concat(args).ToList());

        var ext = Path.GetExtension(exe).ToLowerInvariant();
        if (OperatingSystem.IsWindows())
        {
            switch (ext)
            {
                case ".ps1":
                    return ("powershell.exe", new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", exe }.Concat(args).ToList());
                case ".bat":
                case ".cmd":
                    return (Environment.GetEnvironmentVariable("ComSpec") ?? "cmd.exe", new[] { "/c", exe }.Concat(args).ToList());
                case ".py":
                    return ("py.exe", new[] { exe }.Concat(args).ToList());
            }
        }
        else if (ext is ".sh" or "" && File.Exists(exe) && !IsExecutable(exe))
        {
            return ("/bin/sh", new[] { exe }.Concat(args).ToList());
        }
        return (exe, args);
    }

    static bool IsExecutable(string path)
    {
        if (OperatingSystem.IsWindows()) return true;
        try { return (File.GetUnixFileMode(path) & (UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute)) != 0; }
        catch { return true; }
    }

    public static Dictionary<string, string> EnvironmentFor(PostProcessStep step, Context ctx, string? file, IReadOnlyDictionary<string, string> values)
    {
        var env = new Dictionary<string, string>(ctx.Environment);
        if (file != null) env["BROMELIA_FILE"] = file;
        foreach (var kv in step.Environment) env[kv.Key] = TemplateRenderer.Render(kv.Value, values);
        return env;
    }

    public static async Task<List<StepResult>> RunAsync(IEnumerable<PostProcessStep> steps, Context ctx,
        Action<ProcessRunner?> register, Action<string, Severity> log, CancellationToken ct = default)
    {
        var results = new List<StepResult>();
        foreach (var step in steps.Where(s => ShouldRun(s, ctx.Status)))
        {
            var targets = step.PerFile ? ctx.Files.Select(f => (string?)f).ToList() : new List<string?> { null };
            foreach (var file in targets)
            {
                if (ct.IsCancellationRequested) return results;
                var values = new Dictionary<string, string>(ctx.Values);
                if (file != null)
                {
                    values["file"] = file;
                    values["filename"] = Path.GetFileName(file);
                    values["stem"] = Path.GetFileNameWithoutExtension(file);
                }
                var (exe, args) = BuildInvocation(step, values, ctx.Files);
                var wdTemplate = step.WorkingDirectory.Trim();
                var wd = wdTemplate.Length == 0 ? ctx.OutputDirectory : Paths.ExpandUser(TemplateRenderer.Render(wdTemplate, values));
                var runner = new ProcessRunner(exe, args, EnvironmentFor(step, ctx, file, values), wd);
                var label = file != null ? $"{step.Name} ({Path.GetFileName(file)})" : step.Name;
                log($"▶ {label}: {runner.CommandLine}", Severity.Info);
                register(runner);
                try
                {
                    var r = await runner.RunAsync(l => log($"  [{step.Name}] {l}", Severity.Info),
                        step.TimeoutSeconds > 0 ? TimeSpan.FromSeconds(step.TimeoutSeconds) : default, ct);
                    var sev = r.ExitCode == 0 ? Severity.Info : step.FailJobOnError ? Severity.Error : Severity.Warning;
                    log(r.TimedOut ? $"{label} timed out after {step.TimeoutSeconds} s" : $"{label} exited with status {r.ExitCode}",
                        r.TimedOut ? Severity.Warning : sev);
                    results.Add(new StepResult(step.Id, step.Name, r.ExitCode, r.TimedOut));
                }
                catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or FileNotFoundException)
                {
                    log($"{label} could not be started: {e.Message}", step.FailJobOnError ? Severity.Error : Severity.Warning);
                    results.Add(new StepResult(step.Id, step.Name, -1, false));
                }
                finally
                {
                    register(null);
                }
            }
        }
        return results;
    }
}

/// <summary>Uses mkvmerge to keep only chosen tracks of a file ripped with every track selected.</summary>
public static class Remuxer
{
    public sealed record TrackLayout(int Id, string Type);

    public static async Task<List<TrackLayout>?> IdentifyAsync(string mkvmerge, string file)
    {
        var lines = new LineCollector();
        var r = await new ProcessRunner(mkvmerge, new[] { "-J", file }).RunAsync(lines.Add, TimeSpan.FromMinutes(2));
        if (r.ExitCode != 0) return null;
        try
        {
            using var doc = JsonDocument.Parse(lines.Joined);
            return doc.RootElement.GetProperty("tracks").EnumerateArray()
                .Select(t => new TrackLayout(t.GetProperty("id").GetInt32(), t.GetProperty("type").GetString() ?? "")).ToList();
        }
        catch (Exception) { return null; }
    }

    static string ExpectedType(TrackKind k) => k switch
    {
        TrackKind.Video => "video",
        TrackKind.Audio => "audio",
        TrackKind.Subtitle => "subtitles",
        _ => "other",
    };

    /// <summary>mkvmerge arguments, or null when the file does not match the disc's track list.</summary>
    public static List<string>? Arguments(IReadOnlyList<TrackLayout> layout, TitleInfo title, ISet<int> keep, string input, string output)
    {
        if (layout.Count != title.Tracks.Count) return null;
        for (int i = 0; i < layout.Count; i++)
            if (title.Tracks[i].Kind != TrackKind.Other && ExpectedType(title.Tracks[i].Kind) != layout[i].Type) return null;
        var video = new List<int>(); var audio = new List<int>(); var subs = new List<int>();
        for (int i = 0; i < layout.Count; i++)
        {
            if (!keep.Contains(title.Tracks[i].Index)) continue;
            switch (layout[i].Type)
            {
                case "video": video.Add(layout[i].Id); break;
                case "audio": audio.Add(layout[i].Id); break;
                case "subtitles": subs.Add(layout[i].Id); break;
            }
        }
        static string L(List<int> a) => string.Join(",", a);
        var args = new List<string> { "-o", output };
        args.AddRange(video.Count == 0 ? new[] { "--no-video" } : new[] { "--video-tracks", L(video) });
        args.AddRange(audio.Count == 0 ? new[] { "--no-audio" } : new[] { "--audio-tracks", L(audio) });
        args.AddRange(subs.Count == 0 ? new[] { "--no-subtitles" } : new[] { "--subtitle-tracks", L(subs) });
        args.Add(input);
        return args;
    }
}
