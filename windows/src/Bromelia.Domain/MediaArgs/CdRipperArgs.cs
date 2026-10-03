using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>The audio CD command.</summary>
public static class CdRipperArgs
{
    /// <summary>The profile's otherDiscs.audioCommand ({device} is the drive; its program is resolved by the
    /// adapter), else cyanrip, else abcde, when <paramref name="available"/> (the tools found) has it; null when
    /// there is none. The command runs in the output folder.</summary>
    public static CommandLine? Build(JsonValue otherDiscs, string device, IReadOnlyCollection<string> available)
    {
        var custom = (otherDiscs["audioCommand"]?.AsString ?? "").Trim();
        if (custom.Length > 0)
        {
            var values = new Dictionary<string, string> { ["device"] = device };
            var parts = ArgumentSplitter.Split(custom).Select(p => TemplateEngine.Render(p, values)).ToList();
            return parts.Count == 0 ? null : new CommandLine(parts[0], parts.Skip(1).ToList());
        }
        if (available.Contains("cyanrip")) return new CommandLine("cyanrip", new[] { "-d", device, "-o", "flac" });
        if (available.Contains("abcde")) return new CommandLine("abcde", new[] { "-d", device, "-o", "flac", "-N" });
        return null;
    }
}
