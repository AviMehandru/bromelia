using System;
using System.Collections.Generic;
using System.IO;
using Bromelia.Ports;

namespace Bromelia.Adapters.Windows;

/// <summary>Where each tool is usually installed on Windows, and its program names (today's candidate lists). Tools
/// shipped next to Bromelia (cyanrip, apprise) are found there first.</summary>
public static class PlatformToolPaths
{
    public static IReadOnlyDictionary<ToolKind, IReadOnlyList<string>> Candidates(string home)
    {
        var x86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
        var x64 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        var here = AppContext.BaseDirectory;
        IReadOnlyList<string> In(params string[] relative)
        {
            var list = new List<string>();
            foreach (var root in new[] { x64, x86 })
                if (root.Length > 0)
                    foreach (var r in relative) list.Add(Path.Combine(root, r));
            return list;
        }
        return new Dictionary<ToolKind, IReadOnlyList<string>>
        {
            [ToolKind.Makemkvcon] = new[] { Path.Combine(x86, "MakeMKV", "makemkvcon64.exe"), Path.Combine(x86, "MakeMKV", "makemkvcon.exe"),
                                            Path.Combine(x64, "MakeMKV", "makemkvcon64.exe"), Path.Combine(x64, "MakeMKV", "makemkvcon.exe") },
            [ToolKind.Mkvmerge] = In(@"MKVToolNix\mkvmerge.exe"),
            [ToolKind.Mkvextract] = In(@"MKVToolNix\mkvextract.exe"),
            [ToolKind.Handbrake] = In(@"HandBrake\HandBrakeCLI.exe", @"HandBrakeCLI\HandBrakeCLI.exe"),
            [ToolKind.Tesseract] = In(@"Tesseract-OCR\tesseract.exe"),
            [ToolKind.Ffmpeg] = In(@"ffmpeg\bin\ffmpeg.exe"),
            [ToolKind.Cyanrip] = new[] { Path.Combine(here, "cyanrip.exe"), Path.Combine(here, "cyanrip", "cyanrip.exe") },
            [ToolKind.Par2] = In(@"MultiPar\par2j64.exe", @"par2cmdline\par2.exe"),
            [ToolKind.Apprise] = new[] { Path.Combine(here, "apprise.exe") },
        };
    }

    public static IReadOnlyDictionary<ToolKind, IReadOnlyList<string>> Names() => new Dictionary<ToolKind, IReadOnlyList<string>>
    {
        [ToolKind.Makemkvcon] = new[] { "makemkvcon64.exe", "makemkvcon.exe" },
        [ToolKind.Mkvmerge] = new[] { "mkvmerge.exe" },
        [ToolKind.Mkvextract] = new[] { "mkvextract.exe" },
        [ToolKind.Ffmpeg] = new[] { "ffmpeg.exe" },
        [ToolKind.Tesseract] = new[] { "tesseract.exe" },
        [ToolKind.Handbrake] = new[] { "HandBrakeCLI.exe" },
        [ToolKind.Cyanrip] = new[] { "cyanrip.exe" },
        [ToolKind.Abcde] = new[] { "abcde" },
        [ToolKind.Par2] = new[] { "par2.exe", "par2j64.exe" },
        [ToolKind.Apprise] = new[] { "apprise.exe" },
    };
}
