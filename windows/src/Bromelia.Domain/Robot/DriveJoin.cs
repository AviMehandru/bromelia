using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace Bromelia.Domain;

/// <summary>MakeMKV's drives ⇄ the OS's drives and the configuration's entries.</summary>
public static class DriveJoin
{
    /// <summary>One entry per present MakeMKV drive, with the OS drive of the same device; then the OS drives
    /// MakeMKV didn't report (their id comes from the device until MakeMKV names them).</summary>
    public static List<JoinedDrive> Join(IReadOnlyList<MakemkvDrive> makemkvDrives, IReadOnlyList<OsDrive> osDrives)
    {
        var joined = new List<JoinedDrive>();
        var used = new HashSet<OsDrive>(ReferenceEqualityComparer.Instance);
        foreach (var m in makemkvDrives)
        {
            if (!MakemkvDrive.IsPresent(m)) continue;
            var os = m.Device.Length == 0 ? null : osDrives.FirstOrDefault(o => !used.Contains(o) && DeviceKey(o.Device) == DeviceKey(m.Device));
            if (os != null) used.Add(os);
            joined.Add(new JoinedDrive(DriveId(m.Identification, m.Device), m, os));
        }
        foreach (var o in osDrives)
            if (!used.Contains(o)) joined.Add(new JoinedDrive(DriveId("", o.Device), null, o));
        return joined;
    }

    /// <summary><c>drv-</c> and the first 16 hex digits of the SHA-256 of the normalised identification (lower
    /// case, runs of spaces collapsed), or of <c>dev:&lt;device&gt;</c> when it's empty.</summary>
    public static string DriveId(string identification, string device)
    {
        var name = Normalize(identification);
        var key = name.Length > 0 ? name : "dev:" + device;
        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(key));
        return "drv-" + Convert.ToHexString(hash, 0, 8).ToLowerInvariant();
    }

    /// <summary>driveName compared case- and space-insensitively with the identification; devicePath (ignoring
    /// case) when driveName is empty.</summary>
    public static bool Matches(DriveMatch match, MakemkvDrive drive)
    {
        var name = match.DriveName.Trim(' ', '\t');
        if (name.Length > 0) return Normalize(name) == Normalize(drive.Identification);
        return match.DevicePath.Length > 0 && MessageCatalog.AsciiLower(match.DevicePath) == MessageCatalog.AsciiLower(drive.Device);
    }

    /// <summary>The first enabled entry that matches, else the first that matches.</summary>
    public static DriveEntry? EntryFor(IReadOnlyList<DriveEntry> drives, MakemkvDrive drive) =>
        drives.FirstOrDefault(d => d.Enabled && Matches(d.Match, drive)) ?? drives.FirstOrDefault(d => Matches(d.Match, drive));

    /// <summary>The model part of an identification: <c>BD-RE NEW DRIVE 3.00 SN</c> → <c>NEW DRIVE 3.00</c>.</summary>
    public static string ShortModel(string identification)
    {
        var parts = identification.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return parts.Length > 2 ? string.Join(" ", parts.Skip(1).Take(3)) : (identification.Length == 0 ? "Drive" : identification);
    }

    private static string Normalize(string s) =>
        MessageCatalog.AsciiLower(string.Join(' ', s.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries)));

    // /dev/rdisk4 (MakeMKV on macOS) and /dev/disk4 (DiskArbitration) are one drive; E: and E:\ too.
    private static string DeviceKey(string device)
    {
        var d = MessageCatalog.AsciiLower(device).TrimEnd('\\', '/');
        return d.StartsWith("/dev/rdisk", StringComparison.Ordinal) ? "/dev/disk" + d.Substring(10) : d;
    }
}
