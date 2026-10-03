using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text.RegularExpressions;

namespace Bromelia.Domain;

/// <summary>HandBrake steps: HandBrakeCLI with a preset, one encode per ripped MKV, next to the archive (never over
/// it).</summary>
public static class HandBrakeArgs
{
    private static readonly Regex Progress = new(@"Encoding: task (\d+) of \d+, (\d+)\.\d+ %", RegexOptions.CultureInvariant);

    /// <summary><c>[--preset-import-file F] [--preset P] -i input -o output [more arguments]</c>. A leading ~ in
    /// the preset file is <paramref name="home"/>.</summary>
    public static List<string> Build(StepDefinition step, string input, string output, string? home)
    {
        var h = step.Handbrake ?? new HandBrakeSettings();
        var args = new List<string>();
        var file = CommandArgs.ExpandHome(h.PresetFile.Trim(), home);
        if (file.Length > 0) args.AddRange(new[] { "--preset-import-file", file });
        if (h.Preset.Trim() is { Length: > 0 } preset) args.AddRange(new[] { "--preset", preset });
        args.AddRange(new[] { "-i", input, "-o", output });
        args.AddRange(ArgumentSplitter.Split(h.ExtraArguments));
        return args;
    }

    /// <summary>The encode's path: the step's output path (default <c>{outputDir}/Encoded/{stem}.mkv</c>) filled
    /// in for the file; a leading ~ is <paramref name="home"/>.</summary>
    public static string Output(StepDefinition step, IReadOnlyDictionary<string, string> values, string? home)
    {
        var t = (step.Handbrake ?? new HandBrakeSettings()).OutputPath.Trim();
        return CommandArgs.ExpandHome(TemplateEngine.Render(t.Length == 0 ? new HandBrakeSettings().OutputPath : t, values), home);
    }

    /// <summary>Only MKV files are encoded (not backups, ISO images or other files).</summary>
    public static bool IsSource(string path) => path.EndsWith(".mkv", StringComparison.OrdinalIgnoreCase);

    /// <summary>Keeps HandBrakeCLI's progress lines (<c>Encoding: task 1 of 1, 45.12 %</c>) at every 10 % of each
    /// task, and every other line.</summary>
    public static bool KeepLine(ref ProgressFilter filter, string line)
    {
        var m = Progress.Match(line);
        if (!m.Success) return true;
        int task = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
        int step = int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture) / 10;
        if (task != filter.Task) filter = new ProgressFilter(task, 0);
        if (step < filter.Next) return false;
        filter = filter with { Next = step + 1 };
        return true;
    }
}
