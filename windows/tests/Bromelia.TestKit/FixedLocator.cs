using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.TestKit;

/// <summary>A locator that finds makemkvcon at a fixed path (or nowhere) and no other tool.</summary>
public sealed class FixedLocator : IToolLocator
{
    readonly string? _makemkvcon;
    public FixedLocator(string? makemkvcon) { _makemkvcon = makemkvcon; }

    public ToolInfo Locate(ToolKind tool) => tool == ToolKind.Makemkvcon && _makemkvcon is { } p
        ? new ToolInfo(tool, p, null, new List<string>())
        : new ToolInfo(tool, null, null, new List<string>(), new BroMessage(MessageCode.ToolMissing, Severity.Warning, ("tool", JsonValue.Of(EnumWire.Name(tool)))));
}
