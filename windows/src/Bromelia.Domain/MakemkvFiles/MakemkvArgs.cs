using System.Collections.Generic;
using System.Globalization;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>makemkvcon command lines.</summary>
public static class MakemkvArgs
{
    /// <summary>A drive is dev:device when the device is known (stable when drives are renumbered), else disc:N.</summary>
    private static string Source(MakemkvSource source) => source switch
    {
        MakemkvSource.Drive d => d.Device.Length == 0 ? "disc:" + d.Index.ToString(CultureInfo.InvariantCulture) : "dev:" + d.Device,
        MakemkvSource.Iso i => "iso:" + i.Path,
        MakemkvSource.File f => "file:" + f.Path,
        _ => "",
    };

    private static List<string> Common(MakemkvOptions options)
    {
        var a = new List<string> { "-r", "--progress=-same", "--messages=-stdout" };
        if (!options.Scan) a.Add("--noscan");
        if (options.ProfilePath != null) a.Add("--profile=" + options.ProfilePath);
        if (options.MinLengthSeconds is { } m) a.Add("--minlength=" + m.ToString(CultureInfo.InvariantCulture));
        if (options.CacheMB is { } c && c > 0) a.Add("--cache=" + c.ToString(CultureInfo.InvariantCulture));
        if (options.DirectIO is { } d) a.Add("--directio=" + (d ? "true" : "false"));
        a.AddRange(ArgumentSplitter.Split(options.ExtraArguments));
        return a;
    }

    /// <summary><c>… info source</c>.</summary>
    public static List<string> Info(MakemkvSource source, MakemkvOptions options)
    {
        var a = Common(options);
        a.Add("info");
        a.Add(Source(source));
        return a;
    }

    /// <summary><c>… mkv source title destination</c> (title: an index or "all").</summary>
    public static List<string> Mkv(MakemkvSource source, string title, string destination, MakemkvOptions options)
    {
        var a = Common(options);
        a.AddRange(new[] { "mkv", Source(source), title, destination });
        return a;
    }

    /// <summary><c>… backup [--decrypt] disc:N destination</c>. Throws <see cref="BroFailure"/> (backup.needsDrive)
    /// for an image or folder: backup takes disc:N only.</summary>
    public static List<string> Backup(MakemkvSource source, bool decrypt, string destination, MakemkvOptions options)
    {
        if (source is not MakemkvSource.Drive d) throw new BroFailure(new BroMessage(MessageCode.BackupNeedsDrive, Severity.Error).ToError());
        var a = Common(options);
        a.Add("backup");
        if (decrypt) a.Add("--decrypt");
        a.Add("disc:" + d.Index.ToString(CultureInfo.InvariantCulture));
        a.Add(destination);
        return a;
    }

    /// <summary>The drive scan: <c>-r --cache=1 info disc:9999</c>.</summary>
    public static List<string> ScanDrives() => new() { "-r", "--cache=1", "info", "disc:9999" };
}
