using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Microsoft.Win32.SafeHandles;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Windows;

/// <summary>DriveControl on Windows (plan §10.3; shared/fixtures/adapters/drive-control.cases.json), today's
/// WindowsDeviceServices: IOCTL_STORAGE_EJECT_MEDIA and IOCTL_STORAGE_LOAD_MEDIA for the tray, the drive letter's
/// readiness for the mount, a mounted DVD / Blu-ray structure or the table of contents for the content, and raw reads of
/// \\.\E: (only a bare drive letter is a device: any other path is read as the file it is).</summary>
public sealed class PlatformDriveControl : IDriveControl
{
    const int Sector = 2048;
    readonly IClock _clock;

    public PlatformDriveControl(IClock clock) { _clock = clock; }

    /// <summary>\\.\E: for a bare drive letter (E:, E:\ or E:/) on Windows; anything else as it is.</summary>
    public static string DevicePath(string device, bool windows)
    {
        var d = device.Trim();
        bool driveLetter = d.Length >= 2 && char.IsLetter(d[0]) && d[1] == ':' && (d.Length == 2 || (d.Length == 3 && d[2] is '\\' or '/'));
        return windows && driveLetter ? $@"\\.\{char.ToUpperInvariant(d[0])}:" : d;
    }

    public Task Eject(string device) => Task.Run(() =>
    {
        if (!Control(device, IOCTL_STORAGE_EJECT_MEDIA))
            throw new BroFailure(new BroMessage(MessageCode.DriveEjectFailed, Severity.Error, ("device", JsonValue.Of(device))).ToError());
    });

    public Task CloseTray(string device) => Task.Run(() =>
    {
        if (!Control(device, IOCTL_STORAGE_LOAD_MEDIA))
            throw new BroFailure(new BroMessage(MessageCode.DriveCloseTrayFailed, Severity.Error, ("drive", JsonValue.Of(device))).ToError());
    });

    /// <summary>The drive's root once Windows has it ready (checked every half second); none for anything that isn't a
    /// drive letter, after the timeout, or when cancelled.</summary>
    public async Task<string?> WaitForMount(string device, Duration timeout, CancellationToken cancel)
    {
        if (Root(device) is not { } root) return null;
        var deadline = _clock.Monotonic().Seconds + timeout.Seconds;
        while (!cancel.IsCancelled)
        {
            if (Ready(root)) return root;
            if (_clock.Monotonic().Seconds >= deadline) return null;
            try { await _clock.Sleep(new Duration(0.5), cancel).ConfigureAwait(false); }
            catch (BroFailure) { return null; }
        }
        return null;
    }

    /// <summary>A mounted DVD / Blu-ray structure means video; otherwise the table of contents tells audio CDs (any audio
    /// track) from data discs.</summary>
    public DiscContent ProbeContent(string device)
    {
        if (Root(device) is not { } root) return DiscContent.Unknown;
        try
        {
            if (Ready(root) && Directory.EnumerateDirectories(root).Select(Path.GetFileName).Any(n => n?.ToUpperInvariant() is "BDMV" or "VIDEO_TS" or "HVDVD_TS"))
                return DiscContent.Video;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        using var handle = Open(device);
        if (handle is null) return DiscContent.Unknown;
        var toc = new byte[804];
        if (!DeviceIoControl(handle, IOCTL_CDROM_READ_TOC, null, 0, toc, toc.Length, out _, IntPtr.Zero)) return DiscContent.Unknown;
        int first = toc[2], last = toc[3];
        if (last < first) return DiscContent.Unknown;
        for (int i = 0; i <= last - first && i < 99; i++)
            if ((toc[4 + i * 8 + 1] & 0x04) == 0) return DiscContent.Audio; // control bit 2 clear: an audio track
        return DiscContent.Data;
    }

    public ISectorReader OpenRaw(string device)
    {
        var path = DevicePath(device, true);
        var isDevice = path != device.Trim();
        if (!isDevice && !File.Exists(path))
            throw new BroFailure(new BroMessage(MessageCode.FsNotFound, Severity.Error, ("path", JsonValue.Of(path))).ToError());
        try
        {
            var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 1, FileOptions.None);
            long bytes = isDevice ? DiskLength(stream.SafeFileHandle) : stream.Length;
            return new RawReader(stream, path, bytes / Sector);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            throw Failed("open", path, e.Message);
        }
    }

    static BroFailure Failed(string operation, string path, string reason) =>
        new(new BroMessage(MessageCode.FsFailed, Severity.Error, ("operation", JsonValue.Of(operation)), ("path", JsonValue.Of(path)),
            ("reason", JsonValue.Of(reason))).ToError());

    /// <summary>E:\ for a bare drive letter; none for anything else.</summary>
    static string? Root(string device)
    {
        var path = DevicePath(device, true);
        return path.StartsWith(@"\\.\", StringComparison.Ordinal) ? path[4..] + "\\" : null;
    }

    static bool Ready(string root)
    {
        try { return new DriveInfo(root).IsReady; }
        catch (Exception) { return false; }
    }

    static SafeFileHandle? Open(string device)
    {
        if (Root(device) is null) return null;
        var handle = CreateFileW(DevicePath(device, true), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (!handle.IsInvalid) return handle;
        handle.Dispose();
        return null;
    }

    static bool Control(string device, uint code)
    {
        using var handle = Open(device);
        return handle is not null && DeviceIoControl(handle, code, null, 0, null, 0, out _, IntPtr.Zero);
    }

    static long DiskLength(SafeFileHandle handle)
    {
        var length = new byte[8];
        if (!DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, null, 0, length, length.Length, out _, IntPtr.Zero))
            throw new IOException(new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()).Message);
        return BitConverter.ToInt64(length, 0);
    }

    /// <summary>Whole sectors from a file or a raw volume.</summary>
    sealed class RawReader : ISectorReader
    {
        readonly FileStream _stream;
        readonly string _path;
        readonly long _sectors;
        readonly object _gate = new();

        public RawReader(FileStream stream, string path, long sectors)
        {
            _stream = stream;
            _path = path;
            _sectors = sectors;
        }

        public byte[] Read(long sector, int count)
        {
            var n = (int)Math.Max(0, Math.Min(count, _sectors - sector));
            var buffer = new byte[n * Sector];
            lock (_gate)
            {
                try
                {
                    _stream.Seek(sector * Sector, SeekOrigin.Begin);
                    int total = 0, got;
                    while (total < buffer.Length && (got = _stream.Read(buffer, total, buffer.Length - total)) > 0) total += got;
                    return total == buffer.Length ? buffer : buffer[..total];
                }
                catch (IOException e) { throw Failed("read", _path, e.Message); }
            }
        }

        public long SectorCount() => _sectors;

        public void Close() => _stream.Dispose();
    }

    const uint GENERIC_READ = 0x80000000, FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, OPEN_EXISTING = 3;
    const uint IOCTL_STORAGE_EJECT_MEDIA = 0x2D4808, IOCTL_STORAGE_LOAD_MEDIA = 0x2D480C, IOCTL_CDROM_READ_TOC = 0x24000,
        IOCTL_DISK_GET_LENGTH_INFO = 0x7405C;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle device, uint code, byte[]? inBuffer, int inSize, byte[]? outBuffer, int outSize, out int returned,
        IntPtr overlapped);
}
