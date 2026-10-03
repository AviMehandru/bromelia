using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>The apprise command's arguments (after <c>apprise</c> or <c>python -m apprise</c>).</summary>
public static class AppriseArgs
{
    /// <summary><c>-t title -b body url</c>.</summary>
    public static List<string> Build(string url, string title, string body) => new() { "-t", title, "-b", body, url };
}
