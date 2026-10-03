using System;
using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>Known MakeMKV message codes → severity and notices.</summary>
public static class MessageCatalog
{
    /// <summary>2018: write error (e.g. "No space left on device"); 5006: the source file doesn't exist.</summary>
    private static readonly HashSet<int> ErrorCodes = new() { 2003, 2004, 2018, 2023, 5003, 5006, 5010, 5021, 5037, 5055, 5069, 5077 };
    private static readonly HashSet<int> WarningCodes = new() { 3038, 3041, 5042 };

    /// <summary>Debug for code 1003 and debug-flagged "DEBUG…" text; error and warning by flag (0x200 / 0x400)
    /// or known code; info otherwise.</summary>
    public static MessageKind Severity(RobotMessage message)
    {
        if (message.Code == 1003 || ((message.Flags & 0x20) != 0 && message.Text.StartsWith("DEBUG", StringComparison.Ordinal))) return MessageKind.Debug;
        if ((message.Flags & 0x200) != 0 || ErrorCodes.Contains(message.Code)) return MessageKind.Error;
        if ((message.Flags & 0x400) != 0 || WarningCodes.Contains(message.Code)) return MessageKind.Warning;
        return MessageKind.Info;
    }

    public static MakemkvNotice? Notice(RobotMessage message)
    {
        var t = message.Text;
        if (t.StartsWith("Using LibreDrive mode", StringComparison.Ordinal))
        {
            int open = t.IndexOf('('), close = t.LastIndexOf(')');
            return new MakemkvNotice.LibreDrive(open >= 0 && close > open ? t.Substring(open + 1, close - open - 1) : "");
        }
        if (t.Contains("LibreDrive compatible drive is required", StringComparison.Ordinal)) return new MakemkvNotice.LibreDriveRequired();
        if (message.Code is 5052 or 5055 || ContainsIgnoringCase(t, "evaluation period has expired") || ContainsIgnoringCase(t, "evaluation period expired"))
            return new MakemkvNotice.KeyExpired();
        if (t.Contains("Evaluation period not started", StringComparison.Ordinal)
            || t.Contains("start MakeMKV evaluation from a third-party application", StringComparison.Ordinal))
            return new MakemkvNotice.EvaluationNotStarted();
        if (t.Contains("application version is too old", StringComparison.Ordinal)) return new MakemkvNotice.VersionTooOld();
        return null;
    }

    // ASCII case folding, as on the other platforms.
    private static bool ContainsIgnoringCase(string text, string part) => AsciiLower(text).Contains(AsciiLower(part), StringComparison.Ordinal);

    internal static string AsciiLower(string s)
    {
        var chars = s.ToCharArray();
        for (int i = 0; i < chars.Length; i++)
            if (chars[i] >= 'A' && chars[i] <= 'Z') chars[i] = (char)(chars[i] + 32);
        return new string(chars);
    }
}
