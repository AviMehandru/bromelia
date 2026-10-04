using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Microsoft.Win32.SafeHandles;

namespace Bromelia.Adapters.Windows;

/// <summary>DeviceMonitor on Windows (plan §10.3): today's media signature, polled. The CD/DVD/BD drive letters
/// (DriveInfo), whether each is ready (media) and its root when it is (mounted), and the drive's vendor and product from
/// IOCTL_STORAGE_QUERY_PROPERTY; DrivePoller turns changes into events.</summary>
public sealed class PlatformDeviceMonitor : IDeviceMonitor
{
    readonly DrivePoller _poller;

    /// <param name="interval">How often to look (2 s when omitted).</param>
    public PlatformDeviceMonitor(IClock clock, Duration? interval = null)
    {
        _poller = new DrivePoller(Snapshot, clock, interval ?? new Duration(2));
    }

    public void Start(Action<DeviceEvent> sink) => _poller.Start(sink);

    public void Stop() => _poller.Stop();

    public IReadOnlyList<OsDrive> CurrentDrives() => _poller.CurrentDrives();

    /// <summary>The optical drives now: device "E:", identification "vendor product", media when ready, mounted at the
    /// root when ready.</summary>
    public IReadOnlyList<OsDriveState> Snapshot()
    {
        var states = new List<OsDriveState>();
        DriveInfo[] drives;
        try { drives = DriveInfo.GetDrives(); }
        catch (IOException) { return states; }
        foreach (var d in drives.Where(d => d.DriveType == DriveType.CDRom).OrderBy(d => d.Name, StringComparer.Ordinal))
        {
            var device = d.Name.TrimEnd('\\');
            bool ready;
            try { ready = d.IsReady; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { ready = false; }
            states.Add(new OsDriveState(new OsDrive(device, Identification(device), ready ? d.Name : null), ready));
        }
        return states;
    }

    /// <summary>"vendor product" from the storage device descriptor; empty when the drive can't be asked.</summary>
    static string Identification(string device)
    {
        using var handle = CreateFileW(@"\\.\" + device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (handle.IsInvalid) return "";
        var query = new byte[12]; // STORAGE_PROPERTY_QUERY: StorageDeviceProperty, PropertyStandardQuery
        var buffer = new byte[1024];
        if (!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, query, query.Length, buffer, buffer.Length, out var got, IntPtr.Zero) || got < 20)
            return "";
        string Field(int offsetAt)
        {
            var offset = BitConverter.ToInt32(buffer, offsetAt);
            if (offset <= 0 || offset >= got) return "";
            var end = Array.IndexOf(buffer, (byte)0, offset);
            return Encoding.ASCII.GetString(buffer, offset, (end < 0 ? got : end) - offset).Trim();
        }
        return string.Join(" ", new[] { Field(12), Field(16) }.Where(s => s.Length > 0)); // VendorIdOffset, ProductIdOffset
    }

    const uint IOCTL_STORAGE_QUERY_PROPERTY = 0x2D1400;
    const uint FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, OPEN_EXISTING = 3;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle device, uint code, byte[] inBuffer, int inSize, byte[] outBuffer, int outSize, out int returned,
        IntPtr overlapped);
}
