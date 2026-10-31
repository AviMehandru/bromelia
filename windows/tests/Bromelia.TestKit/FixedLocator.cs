using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.TestKit;

/// <summary>A locator that finds makemkvcon (and mkvmerge, when given) at a fixed path, with the version given, or
/// nowhere, and no other tool.</summary>
public sealed class FixedLocator : IToolLocator
{
    readonly string? _makemkvcon;
    readonly string? _mkvmerge;
    readonly string? _makemkvconVersion;

    public FixedLocator(string? makemkvcon, string? mkvmerge = null, string? makemkvconVersion = null)
    {
        _makemkvcon = makemkvcon;
        _mkvmerge = mkvmerge;
        _makemkvconVersion = makemkvconVersion;
    }

    public ToolInfo Locate(ToolKind tool) => tool switch
    {
        ToolKind.Makemkvcon when _makemkvcon is { } p => new ToolInfo(tool, p, _makemkvconVersion, new List<string>()),
        ToolKind.Mkvmerge when _mkvmerge is { } p => new ToolInfo(tool, p, null, new List<string>()),
        _ => new ToolInfo(tool, null, null, new List<string>(), new BroMessage(MessageCode.ToolMissing, Severity.Warning, ("tool", JsonValue.Of(EnumWire.Name(tool))))),
    };
}
