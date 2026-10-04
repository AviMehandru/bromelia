using System.Collections.Generic;
using System.IO;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Adapters;

/// <summary>ToolLocator (plan §9; shared/fixtures/adapters/tool-locator.cases.json): the configured path (when set,
/// nothing else is tried), else mkvextract next to the mkvmerge that was found, else the candidates in order, else each
/// folder of the search path with each program name in order. Versions and capabilities come from the tools
/// themselves.</summary>
public sealed class SystemToolLocator : IToolLocator
{
    readonly IFileSystem _fs;
    readonly IReadOnlyDictionary<ToolKind, string> _configured;
    readonly IReadOnlyDictionary<ToolKind, IReadOnlyList<string>> _candidates;
    readonly IReadOnlyDictionary<ToolKind, IReadOnlyList<string>> _names;
    readonly IReadOnlyList<string> _searchPath;
    readonly string _home;

    /// <param name="configured">The paths set in the configuration (tools.*, makemkv.path); empty means unset.</param>
    /// <param name="candidates">PlatformToolPaths.Candidates: where each tool is usually installed.</param>
    /// <param name="names">PlatformToolPaths.Names: its program names, tried in order.</param>
    /// <param name="searchPath">The folders of PATH.</param>
    /// <param name="home">What "~/" stands for.</param>
    public SystemToolLocator(IFileSystem fs, IReadOnlyDictionary<ToolKind, string> configured, IReadOnlyDictionary<ToolKind, IReadOnlyList<string>> candidates,
        IReadOnlyDictionary<ToolKind, IReadOnlyList<string>> names, IReadOnlyList<string> searchPath, string home)
    {
        _fs = fs;
        _configured = configured;
        _candidates = candidates;
        _names = names;
        _searchPath = searchPath;
        _home = home;
    }

    public ToolInfo Locate(ToolKind tool)
    {
        if (_configured.TryGetValue(tool, out var configured) && configured.Trim().Length > 0)
        {
            var path = Expand(configured.Trim());
            return IsFile(path) ? Found(tool, path)
                : new ToolInfo(tool, null, null, new List<string>(), new BroMessage(MessageCode.ToolNotFoundAt, Severity.Error,
                    ("tool", JsonValue.Of(EnumWire.Name(tool))), ("path", JsonValue.Of(path))));
        }
        if (tool == ToolKind.Mkvextract && Locate(ToolKind.Mkvmerge).Path is { } mkvmerge && Path.GetDirectoryName(mkvmerge) is { } dir)
            foreach (var name in Names(tool))
                if (IsFile(Path.Combine(dir, name))) return Found(tool, Path.Combine(dir, name));
        if (_candidates.TryGetValue(tool, out var candidates))
            foreach (var c in candidates)
                if (IsFile(Expand(c))) return Found(tool, Expand(c));
        foreach (var folder in _searchPath)
        {
            if (folder.Length == 0) continue;
            foreach (var name in Names(tool))
                if (IsFile(Path.Combine(folder, name))) return Found(tool, Path.Combine(folder, name));
        }
        return new ToolInfo(tool, null, null, new List<string>(),
            new BroMessage(MessageCode.ToolMissing, Severity.Warning, ("tool", JsonValue.Of(EnumWire.Name(tool)))));
    }

    IReadOnlyList<string> Names(ToolKind tool) => _names.TryGetValue(tool, out var n) ? n : new[] { EnumWire.Name(tool) };

    static ToolInfo Found(ToolKind tool, string path) => new(tool, path, null, new List<string>());

    string Expand(string path) => path.StartsWith("~/") || path.StartsWith("~\\") ? Path.Combine(_home, path[2..]) : path;

    bool IsFile(string path)
    {
        try { return !_fs.Stat(path).IsDirectory; }
        catch (BroFailure) { return false; }
    }
}
