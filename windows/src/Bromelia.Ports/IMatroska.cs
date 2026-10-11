using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>MKVToolNix (plan §10.1): what an MKV file holds, remuxing, splitting at chapters, chapter times.</summary>
public interface IMatroska
{
    /// <summary>None when mkvmerge can't read the file.</summary>
    Task<MkvProbe?> Probe(string file, CancellationToken cancel);

    Task<bool> Remux(IReadOnlyList<string> arguments, CancellationToken cancel);

    /// <summary>The parts in order; none when it failed or made another number of parts.</summary>
    Task<IReadOnlyList<string>?> Split(IReadOnlyList<int> chapters, string input, CancellationToken cancel);

    Task<IReadOnlyList<Duration>?> ChapterTimes(string file, CancellationToken cancel);
}
