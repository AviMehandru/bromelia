using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>Episode numbers from a DVD's menu stills (plan §10.1; shared/fixtures/adapters/menu-ocr.cases.json):
/// ffmpeg turns each still cell into a large grey PNG, tesseract reads it, MenuNumbers.Parse finds the numbers.</summary>
public sealed class MenuOcr
{
    const int Sector = 2048;
    static readonly Duration Stall = new(60);

    readonly IProcessLauncher _launcher;
    readonly IToolLocator _locator;
    readonly IFileSystem _fs;

    public MenuOcr(IProcessLauncher launcher, IToolLocator locator, IFileSystem fs)
    {
        _launcher = launcher;
        _locator = locator;
        _fs = fs;
    }

    /// <summary>The PNG of each cell ffmpeg could convert, in order, in <paramref name="dir"/> (which must exist).</summary>
    public async Task<IReadOnlyList<string>> ExtractStills(IByteSource source, IReadOnlyList<CellRef> cells, string dir, CancellationToken cancel)
    {
        var ffmpeg = Tool(ToolKind.Ffmpeg);
        var stills = new List<string>();
        for (int i = 0; i < cells.Count; i++)
        {
            var cell = cells[i];
            var mpg = Path.Combine(dir, $"menu{i}.mpg");
            var png = Path.Combine(dir, $"menu{i}.png");
            _fs.WriteAtomically(mpg, source.Read(cell.File, cell.FirstSector * Sector, (int)((cell.EndSector - cell.FirstSector) * Sector)), 0x1A4);
            try
            {
                var (exit, _) = await Run(ffmpeg, new[] { "-v", "quiet", "-y", "-f", "mpeg", "-i", mpg, "-frames:v", "1", "-vf", "scale=2160:1440,format=gray", png },
                    cancel).ConfigureAwait(false);
                if (exit.Status == 0 && _fs.Exists(png)) stills.Add(png);
            }
            finally
            {
                try { _fs.Remove(mpg); } catch (BroFailure) { }
            }
        }
        return stills;
    }

    /// <summary>The episode numbers on <paramref name="stills"/>, each once, in the order first read.</summary>
    public async Task<IReadOnlyList<int>> ReadNumbers(IReadOnlyList<string> stills, CancellationToken cancel)
    {
        var tesseract = Tool(ToolKind.Tesseract);
        var numbers = new List<int>();
        foreach (var still in stills)
        {
            var (exit, lines) = await Run(tesseract, new[] { still, "stdout", "--psm", "11" }, cancel).ConfigureAwait(false);
            if (exit.Status != 0) continue;
            foreach (var n in MenuNumbers.Parse(string.Join("\n", lines)))
                if (!numbers.Contains(n)) numbers.Add(n);
        }
        return numbers;
    }

    string Tool(ToolKind tool)
    {
        var info = _locator.Locate(tool);
        return info.Path ?? throw new BroFailure((info.Why ?? new BroMessage(MessageCode.ToolMissing, Severity.Warning,
            ("tool", JsonValue.Of(EnumWire.Name(tool))))).ToError());
    }

    async Task<(ProcessExit Exit, List<string> Lines)> Run(string exe, IReadOnlyList<string> arguments, CancellationToken cancel)
    {
        if (cancel.IsCancelled) throw Cancelled();
        var process = _launcher.Start(new ProcessSpec(exe, arguments, new Dictionary<string, string>(), null, StopPolicy.InterruptFirst, Stall));
        using var onCancel = cancel.OnCancel(() => process.Stop(StopReason.Cancelled));
        var lines = new List<string>();
        await foreach (var line in process.Lines().ConfigureAwait(false))
            if (line.Stream == OutputSource.Stdout) lines.Add(line.Text);
        var exit = await process.Wait().ConfigureAwait(false);
        if (exit.Cancelled) throw Cancelled();
        return (exit, lines);
    }

    static BroFailure Cancelled() => new(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
}
