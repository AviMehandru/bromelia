using System;
using System.Collections.Generic;
using System.Text;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What one makemkvcon run said, fed one event at a time: the saved / failed counts, the errors and
/// the first of them (usually the cause), MakeMKV's space warning, read errors, a renumbered drive, the debug
/// log, notices and the version. When <see cref="StopReason"/> is set, the caller stops the process.</summary>
public sealed class RunAccumulator
{
    private readonly bool _readsData;
    private readonly int? _expectedIndex;
    private readonly string _expectedDevice;

    /// <param name="readsData">The run reads the disc's data (a rip or backup): its errors count as read errors.</param>
    /// <param name="expectedIndex">The MakeMKV drive index the run uses, with its device, to catch renumbered drives.</param>
    public RunAccumulator(bool readsData = false, int? expectedIndex = null, string expectedDevice = "")
    {
        _readsData = readsData;
        _expectedIndex = expectedIndex;
        _expectedDevice = expectedDevice;
    }

    /// <summary>Titles saved, from 5036 / 5005 or the summary 5037 / 5004.</summary>
    public int? Saved { get; private set; }
    /// <summary>Titles failed, from the summary 5037 / 5004.</summary>
    public int? Failed { get; private set; }
    /// <summary>Every message of severity error, in order.</summary>
    public List<RobotMessage> Errors { get; } = new();
    /// <summary>The first error other than the saved / failed summary (5037, 5004): usually the cause.</summary>
    public RobotMessage? FirstError { get; private set; }
    /// <summary>MakeMKV's warning that the output may not fit (5038).</summary>
    public RobotMessage? SpaceWarning { get; private set; }
    /// <summary>Errors while reading the disc's data (only when the run reads data).</summary>
    public List<RobotMessage> ReadErrors { get; } = new();
    /// <summary>The drive index now names another device (drive.renumbered).</summary>
    public BroMessage? DriveMismatch { get; private set; }
    /// <summary>MakeMKV's debug log (app_ShowDebug), named by message 1004, as a path.</summary>
    public string? DebugLog { get; private set; }
    /// <summary>The LibreDrive detail of message 1011, when the drive uses LibreDrive.</summary>
    public string? LibreDrive { get; private set; }
    /// <summary>The first notice other than LibreDrive: a key, version or drive problem.</summary>
    public MakemkvNotice? Problem { get; private set; }
    /// <summary>MakeMKV's version (message 1005).</summary>
    public string? MakemkvVersion { get; private set; }
    /// <summary>Why the run must stop now (space.makemkvWarning or drive.renumbered); null while it may go on.</summary>
    public BroMessage? StopReason { get; private set; }

    public void Feed(RobotEvent @event)
    {
        switch (@event)
        {
            case RobotEvent.Message { Value: var m }:
                FeedMessage(m);
                break;
            case RobotEvent.Drive d when _expectedIndex is { } index && d.Index == index && _expectedDevice.Length > 0
                                         && d.Device.Length > 0 && !EqualsIgnoringCase(d.Device, _expectedDevice):
                DriveMismatch ??= new BroMessage(MessageCode.DriveRenumbered, Severity.Error,
                    ("index", JsonValue.Of(index)), ("now", JsonValue.Of(d.Device)), ("expected", JsonValue.Of(_expectedDevice)));
                StopReason ??= DriveMismatch;
                break;
        }
    }

    private void FeedMessage(RobotMessage m)
    {
        if (MessageCatalog.Notice(m) is { } notice)
        {
            if (notice is MakemkvNotice.LibreDrive l) LibreDrive ??= l.Detail;
            else Problem ??= notice;
        }
        // "Debug logging enabled, log will be saved as file:///C:/…/MakeMKV_log.txt"
        if (m.Code == 1004 && m.Params.Count > 0 && FilePath(m.Params[0]) is { } path) DebugLog = path;
        if (MessageCatalog.Severity(m) == MessageKind.Error)
        {
            if (FirstError == null && m.Code is not (5037 or 5004)) FirstError = m;
            Errors.Add(m);
            if (_readsData) ReadErrors.Add(m);
        }
        if (m.Code == 1005 && MakemkvVersion == null) MakemkvVersion = m.Params.Count > 0 ? m.Params[0] : m.Text;
        if (m.Code is 5036 or 5005 && m.Params.Count > 0 && Robot.Int(m.Params[0], out var saved)) Saved = saved;
        if (m.Code == 5038 && SpaceWarning == null)
        {
            // "The total size of all output files may reach as much as … while there are only … free": stop
            // before anything is written rather than fail when the disk fills up.
            SpaceWarning = m;
            StopReason ??= new BroMessage(MessageCode.SpaceMakemkvWarning, Severity.Error, ("text", JsonValue.Of(m.Text)));
        }
        if (m.Code is 5037 or 5004 && m.Params.Count > 1)
        {
            if (Robot.Int(m.Params[0], out var s)) Saved = s;
            if (Robot.Int(m.Params[1], out var f)) Failed = f;
        }
    }

    /// <summary>The path of a file:// URL (percent-decoded; <c>file:///C:/x</c> → <c>C:/x</c>), else null.</summary>
    internal static string? FilePath(string url)
    {
        if (!url.StartsWith("file://", StringComparison.OrdinalIgnoreCase)) return null;
        var rest = url.Substring(7);
        if (rest.StartsWith("localhost/", StringComparison.OrdinalIgnoreCase)) rest = rest.Substring(9);
        if (!rest.StartsWith("/")) return null;
        if (rest.Length >= 3 && char.IsAsciiLetter(rest[1]) && rest[2] == ':') rest = rest.Substring(1);
        var bytes = new List<byte>();
        for (int i = 0; i < rest.Length; i++)
        {
            if (rest[i] == '%' && i + 2 < rest.Length && Uri.IsHexDigit(rest[i + 1]) && Uri.IsHexDigit(rest[i + 2]))
            {
                bytes.Add(Convert.ToByte(rest.Substring(i + 1, 2), 16));
                i += 2;
            }
            else bytes.AddRange(Encoding.UTF8.GetBytes(rest[i].ToString()));
        }
        return Encoding.UTF8.GetString(bytes.ToArray());
    }

    private static bool EqualsIgnoringCase(string a, string b) => MessageCatalog.AsciiLower(a) == MessageCatalog.AsciiLower(b);
}
