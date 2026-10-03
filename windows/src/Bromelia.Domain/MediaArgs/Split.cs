using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace Bromelia.Domain;

/// <summary>mkvmerge arguments that split a file at chapters without re-encoding.</summary>
public static class Split
{
    /// <summary><c>-o output --split chapters:8,15,… input</c>; mkvmerge numbers the parts in
    /// <paramref name="output"/> (-001, -002, …).</summary>
    public static List<string> Arguments(IReadOnlyList<int> chapters, string input, string output) =>
        new() { "-o", output, "--split", "chapters:" + string.Join(",", chapters.Select(c => c.ToString(CultureInfo.InvariantCulture))), input };
}
