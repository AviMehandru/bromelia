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
        if (!step.Enabled || (step.Kind == StepKind.Command && string.IsNullOrWhiteSpace(step.Executable))) return false;
        return step.RunOn switch
        {
            RunCondition.Always => true,
            RunCondition.Success => status == JobState.Succeeded,
            _ => status is JobState.Failed or JobState.Cancelled or JobState.CompletedWithErrors,
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
            if (step.Kind == StepKind.Handbrake)
            {
                var sources = ctx.Files.Where(HandBrake.IsSource).ToList();
                if (HandBrake.Tool(step) is not { } tool)
                {
                    log($"{step.Name}: HandBrakeCLI was not found. Download HandBrake's command line version (handbrake.fr), or set its location in the step.",
                        step.FailJobOnError ? Severity.Error : Severity.Warning);
                    results.Add(new StepResult(step.Id, step.Name, -1, false));
                    continue;
                }
                if (sources.Count == 0) log($"{step.Name}: no MKV files to transcode", Severity.Info);
                foreach (var file in sources)
                {
                    if (ct.IsCancellationRequested) return results;
                    var values = FileValues(ctx.Values, file);
                    var output = Paths.UniquePath(HandBrake.Output(step, values));
                    try { Directory.CreateDirectory(Path.GetDirectoryName(output)!); }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                    {
                        log($"{step.Name}: can't create {Path.GetDirectoryName(output)}: {e.Message}", step.FailJobOnError ? Severity.Error : Severity.Warning);
                        results.Add(new StepResult(step.Id, step.Name, -1, false));
                        continue;
                    }
                    var filter = new HandBrake.ProgressFilter();
                    results.Add(await InvokeAsync(step, $"{step.Name} ({Path.GetFileName(file)} → {Path.GetFileName(output)})", tool,
                        HandBrake.Arguments(step, file, output), EnvironmentFor(step, ctx, file, values), Path.GetDirectoryName(output), filter.Keep,
                        register, log, ct));
                }
                continue;
            }
            var targets = step.PerFile ? ctx.Files.Select(f => (string?)f).ToList() : new List<string?> { null };
            foreach (var file in targets)
            {
                if (ct.IsCancellationRequested) return results;
                var values = file != null ? FileValues(ctx.Values, file) : new Dictionary<string, string>(ctx.Values);
                var (exe, args) = BuildInvocation(step, values, ctx.Files);
                var wdTemplate = step.WorkingDirectory.Trim();
                var wd = wdTemplate.Length == 0 ? ctx.OutputDirectory : Paths.ExpandUser(TemplateRenderer.Render(wdTemplate, values));
                results.Add(await InvokeAsync(step, file != null ? $"{step.Name} ({Path.GetFileName(file)})" : step.Name, exe, args,
                    EnvironmentFor(step, ctx, file, values), wd, null, register, log, ct));
            }
        }
        return results;
    }

    /// <summary>The template values for one file of a per-file step.</summary>
    static Dictionary<string, string> FileValues(IReadOnlyDictionary<string, string> baseValues, string file) => new(baseValues)
    {
        ["file"] = file, ["filename"] = Path.GetFileName(file), ["stem"] = Path.GetFileNameWithoutExtension(file),
    };

    /// <summary>Runs one command of a step and logs its output (lines <paramref name="filter"/> rejects are left out).</summary>
    static async Task<StepResult> InvokeAsync(PostProcessStep step, string label, string exe, List<string> args, Dictionary<string, string> env, string? wd,
        Func<string, bool>? filter, Action<ProcessRunner?> register, Action<string, Severity> log, CancellationToken ct)
    {
        var runner = new ProcessRunner(exe, args, env, wd);
        log($"▶ {label}: {runner.CommandLine}", Severity.Info);
        register(runner);
        try
        {
            var r = await runner.RunAsync(l => { if (filter?.Invoke(l) ?? true) log($"  [{step.Name}] {l}", Severity.Info); },
                step.TimeoutSeconds > 0 ? TimeSpan.FromSeconds(step.TimeoutSeconds) : default, ct);
            var sev = r.ExitCode == 0 ? Severity.Info : step.FailJobOnError ? Severity.Error : Severity.Warning;
            log(r.TimedOut ? $"{label} timed out after {step.TimeoutSeconds} s" : $"{label} exited with status {r.ExitCode}", r.TimedOut ? Severity.Warning : sev);
            return new StepResult(step.Id, step.Name, r.ExitCode, r.TimedOut);
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or FileNotFoundException)
        {
            log($"{label} could not be started: {e.Message}", step.FailJobOnError ? Severity.Error : Severity.Warning);
            return new StepResult(step.Id, step.Name, -1, false);
        }
        finally
        {
            register(null);
        }
    }
}

/// <summary>Presets built into HandBrake 1.6 and later, offered in the step editor (any preset name works).</summary>
public static class HandBrakePresets
{
    public static readonly string[] Common = { "H.265 MKV 1080p30", "H.265 MKV 2160p60 4K", "H.264 MKV 1080p30", "H.264 MKV 480p30",
        "Fast 1080p30", "HQ 1080p30 Surround", "Super HQ 1080p30 Surround", "Fast 2160p60 4K HEVC" };
}

/// <summary>HandBrake steps: HandBrakeCLI with a preset, one encode per ripped MKV, next to the archive (never over it).</summary>
public static class HandBrake
{
    /// <summary>The HandBrakeCLI of the step, or the one installed.</summary>
    public static string? Tool(PostProcessStep step)
    {
        var exe = Paths.ExpandUser(step.Executable.Trim());
        if (exe.Length > 0) return File.Exists(exe) ? exe : null;
        return EpisodeSplitter.FindTool("HandBrakeCLI");
    }

    /// <summary>Only MKV files are transcoded (not backups, ISO images or other files).</summary>
    public static bool IsSource(string file) => file.EndsWith(".mkv", StringComparison.OrdinalIgnoreCase) && !Directory.Exists(file);

    /// <summary>The encode's path: the step's output path rendered for the file (the default is {outputDir}/Encoded/{stem}.mkv).</summary>
    public static string Output(PostProcessStep step, IReadOnlyDictionary<string, string> values)
    {
        var t = step.OutputPath.Trim();
        return Path.GetFullPath(Paths.ExpandUser(TemplateRenderer.Render(t.Length == 0 ? PostProcessStep.DefaultEncodePath : t, values)));
    }

    /// <summary><c>[--preset-import-file F] --preset P -i input -o output [extra arguments]</c>.</summary>
    public static List<string> Arguments(PostProcessStep step, string input, string output)
    {
        var args = new List<string>();
        var file = Paths.ExpandUser(step.PresetFile.Trim());
        if (file.Length > 0) args.AddRange(new[] { "--preset-import-file", file });
        if (step.Preset.Trim() is { Length: > 0 } preset) args.AddRange(new[] { "--preset", preset });
        args.AddRange(new[] { "-i", input, "-o", output });
        args.AddRange(ArgumentSplitter.Split(step.ExtraArguments));
        return args;
    }

    /// <summary>Keeps HandBrakeCLI's progress lines (<c>Encoding: task 1 of 1, 45.12 %</c>) at every 10 % of each task, and every
    /// other line.</summary>
    public sealed class ProgressFilter
    {
        int _task = -1, _last = -1;

        public bool Keep(string line)
        {
            var m = System.Text.RegularExpressions.Regex.Match(line, @"Encoding: task (\d+) of \d+, (\d+)\.\d+ %");
            if (!m.Success) return true;
            lock (this)
            {
                int t = int.Parse(m.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture);
                int p = int.Parse(m.Groups[2].Value, System.Globalization.CultureInfo.InvariantCulture);
                if (t != _task) { _task = t; _last = -1; }
                if (p / 10 <= _last) return false;
                _last = p / 10;
                return true;
            }
        }
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
