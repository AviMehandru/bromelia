using System;
using System.Collections.Generic;
using System.Text;

namespace Bromelia.Adapters.Windows;

/// <summary>Output bytes into lines, as every launcher splits them: at \n, \r or \r\n, empty lines dropped, UTF-8 with
/// bad bytes replaced. A line is cut after <see cref="MaxBytes"/> bytes and ends with " [cut]"; the rest of it, up to the
/// next line break, is dropped, so a tool writing binary can't grow memory without limit.</summary>
internal sealed class LineSplitter
{
    public const int MaxBytes = 65536;
    public const string CutMark = " [cut]";

    readonly List<byte> _pending = new();
    bool _discarding;

    /// <summary>The lines <paramref name="bytes"/> completes.</summary>
    public List<string> Feed(ReadOnlySpan<byte> bytes)
    {
        var lines = new List<string>();
        foreach (var b in bytes)
        {
            if (b is (byte)'\n' or (byte)'\r')
            {
                if (!_discarding && _pending.Count > 0) lines.Add(Decode(cut: false));
                _pending.Clear();
                _discarding = false;
            }
            else if (!_discarding)
            {
                _pending.Add(b);
                if (_pending.Count == MaxBytes)
                {
                    lines.Add(Decode(cut: true));
                    _pending.Clear();
                    _discarding = true;
                }
            }
        }
        return lines;
    }

    /// <summary>The last line, when the output didn't end with a line break.</summary>
    public string? Finish() => !_discarding && _pending.Count > 0 ? Decode(cut: false) : null;

    string Decode(bool cut) => Encoding.UTF8.GetString(_pending.ToArray()) + (cut ? CutMark : "");
}
