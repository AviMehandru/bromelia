using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>MKVToolNix (plan §10.1; shared/fixtures/adapters/mkvtoolnix.cases.json): mkvmerge -J for the rip check,
/// remuxing, splitting at chapters, chapter times with mkvextract. Exit status 1 is mkvmerge's warning: the output is
/// good. tool.missing when the locator can't find a tool.</summary>
public sealed class MkvToolNix
{
    readonly IProcessLauncher _launcher;
    readonly IFileSystem _fs;
    readonly IToolLocator _locator;
    readonly string _workDirectory;

    /// <param name="workDirectory">Where chapterTimes writes mkvextract's chapter file (the job's folder).</param>
    public MkvToolNix(IProcessLauncher launcher, IFileSystem fs, IToolLocator locator, string workDirectory)
    {
        _launcher = launcher;
        _fs = fs;
        _locator = locator;
        _workDirectory = workDirectory;
    }

    /// <summary>None when mkvmerge can't read the file.</summary>
    public async Task<MkvProbe?> Probe(string file, CancellationToken cancel)
    {
        var (exit, lines) = await Run(ToolKind.Mkvmerge, new[] { "-J", file }, new Duration(300), cancel).ConfigureAwait(false);
        return exit.Status <= 1 ? MkvProbe.Parse(string.Join("\n", lines)) : null;
    }

    public async Task<bool> Remux(IReadOnlyList<string> arguments, CancellationToken cancel) =>
        (await Run(ToolKind.Mkvmerge, arguments, new Duration(600), cancel).ConfigureAwait(false)).Exit.Status <= 1;

    /// <summary>The parts in order (hidden files next to the input); none when mkvmerge failed or made another
    /// number of parts, which are removed.</summary>
    public async Task<IReadOnlyList<string>?> Split(IReadOnlyList<int> chapters, string input, CancellationToken cancel)
    {
        var folder = Path.GetDirectoryName(input) ?? ".";
        var prefix = ".bromelia-split-" + Guid.NewGuid().ToString("N")[..8];
        var (exit, _) = await Run(ToolKind.Mkvmerge, Domain.Split.Arguments(chapters, input, Path.Combine(folder, prefix + "-%03d.mkv")),
            new Duration(600), cancel).ConfigureAwait(false);
        var parts = _fs.List(folder).Where(e => e.Name.StartsWith(prefix, StringComparison.Ordinal)).Select(e => e.Name)
            .OrderBy(n => n, StringComparer.Ordinal).Select(n => Path.Combine(folder, n)).ToList();
        if (exit.Status <= 1 && parts.Count == chapters.Count + 1) return parts;
        foreach (var p in parts)
            try { _fs.Remove(p); } catch (BroFailure) { }
        return null;
    }

    public async Task<IReadOnlyList<Duration>?> ChapterTimes(string file, CancellationToken cancel)
    {
        var temp = Path.Combine(_workDirectory, ".bromelia-chapters-" + Guid.NewGuid().ToString("N")[..8] + ".txt");
        try
        {
            var (exit, _) = await Run(ToolKind.Mkvextract, new[] { file, "chapters", "--simple", temp }, new Duration(120), cancel).ConfigureAwait(false);
            if (exit.Status > 1 || !_fs.Exists(temp)) return null;
            return SimpleChapters.Parse(Encoding.UTF8.GetString(_fs.Read(temp)));
        }
        finally
        {
            if (_fs.Exists(temp))
                try { _fs.Remove(temp); } catch (BroFailure) { }
        }
    }

    async Task<(ProcessExit Exit, List<string> Lines)> Run(ToolKind tool, IReadOnlyList<string> arguments, Duration stall, CancellationToken cancel)
    {
        var info = _locator.Locate(tool);
        var exe = info.Path ?? throw new BroFailure((info.Why ?? new BroMessage(MessageCode.ToolMissing, Severity.Warning,
            ("tool", JsonValue.Of(EnumWire.Name(tool))))).ToError());
        if (cancel.IsCancelled) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
        var process = _launcher.Start(new ProcessSpec(exe, arguments, new Dictionary<string, string>(), null, StopPolicy.InterruptFirst, stall));
        using var onCancel = cancel.OnCancel(() => process.Stop(StopReason.Cancelled));
        var lines = new List<string>();
        await foreach (var line in process.Lines().ConfigureAwait(false))
            if (line.Stream == OutputSource.Stdout) lines.Add(line.Text);
        var exit = await process.Wait().ConfigureAwait(false);
        if (exit.Cancelled) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
        return (exit, lines);
    }
}
