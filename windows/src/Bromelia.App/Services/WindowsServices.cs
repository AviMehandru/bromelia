using System.Runtime.InteropServices;
using Bromelia.Core.Engine;
using Microsoft.UI.Dispatching;
using Microsoft.Windows.AppNotifications;
using Microsoft.Windows.AppNotifications.Builder;
using Microsoft.Win32.SafeHandles;

namespace Bromelia.App.Services;

/// <summary>Eject and notifications on Windows.</summary>
public sealed class WindowsPlatformServices : IPlatformServices
{
    bool _notificationsRegistered;

    public void RegisterNotifications()
    {
        try
        {
            AppNotificationManager.Default.Register();
            _notificationsRegistered = true;
        }
        catch (Exception)
        {
            _notificationsRegistered = false;
        }
    }

    public void Notify(string title, string body, bool sound)
    {
        if (!_notificationsRegistered) return;
        try
        {
            var builder = new AppNotificationBuilder().AddText(title).AddText(body);
            if (!sound) builder.MuteAudio();
            AppNotificationManager.Default.Show(builder.BuildNotification());
        }
        catch (Exception)
        {
            // Notifications are best effort.
        }
    }

    public Task<bool> EjectAsync(string devicePath) => Task.Run(() => Eject(devicePath));

    /// <summary>Keeps the system (not the display) awake while jobs run. Called on the UI thread, which lives as long as the app.</summary>
    public void KeepAwake(bool on) =>
        SetThreadExecutionState(on ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);

    const uint ES_CONTINUOUS = 0x80000000, ES_SYSTEM_REQUIRED = 0x00000001;

    [DllImport("kernel32.dll")]
    static extern uint SetThreadExecutionState(uint flags);

    /// <summary>Maps MakeMKV's device name ("E:", "E:\", "\Device\CdRom0") to a Win32 device path.</summary>
    public static string? Win32DevicePath(string device)
    {
        var d = device.Trim();
        // Only a bare drive letter (E: or E:\); a longer path is a file or folder, never a device.
        if (d.Length >= 2 && char.IsLetter(d[0]) && d[1] == ':')
            return d.Length == 2 || (d.Length == 3 && d[2] is '\\' or '/') ? $@"\\.\{char.ToUpperInvariant(d[0])}:" : null;
        if (d.StartsWith(@"\Device\", StringComparison.OrdinalIgnoreCase)) return @"\\.\" + d[8..];
        if (d.StartsWith(@"\\.\", StringComparison.Ordinal)) return d;
        return null;
    }

    public Task<bool> CloseTrayAsync(string devicePath) => Task.Run(() => DeviceControl(devicePath, NativeMethods.IOCTL_STORAGE_LOAD_MEDIA));

    public bool IsDiscMounted(string devicePath)
    {
        var d = devicePath.Trim();
        if (d.Length < 2 || !char.IsLetter(d[0]) || d[1] != ':') return true;
        try { return new DriveInfo(d[..2] + "\\").IsReady; } catch (Exception) { return false; }
    }

    /// <summary>A mounted DVD / Blu-ray structure means video; otherwise the table of contents tells audio CDs (any audio
    /// track) from data discs.</summary>
    public DiscContent ProbeDisc(string devicePath)
    {
        var d = devicePath.Trim();
        if (d.Length >= 2 && char.IsLetter(d[0]) && d[1] == ':')
        {
            try
            {
                var root = d[..2] + "\\";
                if (new DriveInfo(root).IsReady && DiscContentRules.HasVideoStructure(root)) return DiscContent.Video;
            }
            catch (Exception) { }
        }
        var path = Win32DevicePath(devicePath);
        if (path == null) return DiscContent.Unknown;
        using var handle = NativeMethods.CreateFile(path, NativeMethods.GENERIC_READ, NativeMethods.FILE_SHARE_READ | NativeMethods.FILE_SHARE_WRITE,
            IntPtr.Zero, NativeMethods.OPEN_EXISTING, 0, IntPtr.Zero);
        if (handle.IsInvalid) return DiscContent.Unknown;
        var buffer = Marshal.AllocHGlobal(804);
        try
        {
            if (!NativeMethods.DeviceIoControl(handle, NativeMethods.IOCTL_CDROM_READ_TOC, IntPtr.Zero, 0, buffer, 804, out _, IntPtr.Zero))
                return DiscContent.Unknown;
            var toc = new byte[804];
            Marshal.Copy(buffer, toc, 0, toc.Length);
            int first = toc[2], last = toc[3];
            if (last < first) return DiscContent.Unknown;
            for (int i = 0; i <= last - first && i < 99; i++)
                if ((toc[4 + i * 8 + 1] & 0x04) == 0) return DiscContent.Audio;   // control bit 2 clear: audio track
            return DiscContent.Data;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    static bool DeviceControl(string device, uint code)
    {
        var path = Win32DevicePath(device);
        if (path == null) return false;
        using var handle = NativeMethods.CreateFile(path, NativeMethods.GENERIC_READ, NativeMethods.FILE_SHARE_READ | NativeMethods.FILE_SHARE_WRITE,
            IntPtr.Zero, NativeMethods.OPEN_EXISTING, 0, IntPtr.Zero);
        return !handle.IsInvalid && NativeMethods.DeviceIoControl(handle, code, IntPtr.Zero, 0, IntPtr.Zero, 0, out _, IntPtr.Zero);
    }

    static bool Eject(string device)
    {
        var path = Win32DevicePath(device);
        if (path == null) return false;
        using var handle = NativeMethods.CreateFile(path, NativeMethods.GENERIC_READ, NativeMethods.FILE_SHARE_READ | NativeMethods.FILE_SHARE_WRITE,
            IntPtr.Zero, NativeMethods.OPEN_EXISTING, 0, IntPtr.Zero);
        if (handle.IsInvalid) return false;
        return NativeMethods.DeviceIoControl(handle, NativeMethods.IOCTL_STORAGE_EJECT_MEDIA, IntPtr.Zero, 0, IntPtr.Zero, 0, out _, IntPtr.Zero);
    }

    static class NativeMethods
    {
        public const uint GENERIC_READ = 0x80000000;
        public const uint FILE_SHARE_READ = 0x1;
        public const uint FILE_SHARE_WRITE = 0x2;
        public const uint OPEN_EXISTING = 3;
        public const uint IOCTL_STORAGE_EJECT_MEDIA = 0x2D4808;
        public const uint IOCTL_STORAGE_LOAD_MEDIA = 0x2D480C;
        public const uint IOCTL_CDROM_READ_TOC = 0x24000;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool DeviceIoControl(SafeFileHandle device, uint code, IntPtr inBuffer, uint inSize, IntPtr outBuffer, uint outSize,
            out uint returned, IntPtr overlapped);
    }
}

/// <summary>Detects media changes in optical drives cheaply (no makemkvcon) and triggers a rescan.</summary>
public sealed class MediaWatcher
{
    readonly DispatcherQueueTimer _timer;
    readonly Action _changed;
    string _signature = "";

    public MediaWatcher(DispatcherQueue queue, Action changed)
    {
        _changed = changed;
        _timer = queue.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(2);
        _timer.Tick += (_, _) => Check();
    }

    public void Start()
    {
        _signature = Signature();
        _timer.Start();
    }

    void Check()
    {
        var sig = Signature();
        if (sig == _signature) return;
        _signature = sig;
        _changed();
    }

    static string Signature()
    {
        try
        {
            return string.Join("|", DriveInfo.GetDrives().Where(d => d.DriveType == DriveType.CDRom)
                .Select(d => d.Name + ":" + (d.IsReady ? SafeLabel(d) : "-")));
        }
        catch (Exception) { return ""; }
    }

    static string SafeLabel(DriveInfo d)
    {
        try { return d.VolumeLabel; } catch (Exception) { return "?"; }
    }
}
