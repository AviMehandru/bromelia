using System.Runtime.InteropServices;
using Bromelia.Core.Engine;
using Microsoft.UI.Dispatching;
using Microsoft.Windows.AppNotifications;
using Microsoft.Windows.AppNotifications.Builder;
using Microsoft.Win32.SafeHandles;

namespace Bromelia.App.Services;

/// <summary>Eject, trays and sleep (WindowsDeviceServices, in the engine) and notifications on Windows.</summary>
public sealed class WindowsPlatformServices : WindowsDeviceServices
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

    public override void Notify(string title, string body, bool sound)
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

    static string Signature() => WindowsDeviceServices.MediaSignature();
}

/// <summary>The logon task that runs bromelia-daemon.exe (Task Scheduler, task “Bromelia”).</summary>
public static class DaemonTask
{
    public static bool IsInstalled() => Run("schtasks.exe", "/Query", "/TN", "Bromelia") == 0;

    /// <summary>Runs a program without a window and returns its exit code (-1 when it can't start).</summary>
    public static int Run(string exe, params string[] args)
    {
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo(exe) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (var a in args) psi.ArgumentList.Add(a);
            using var p = System.Diagnostics.Process.Start(psi)!;
            p.StandardOutput.ReadToEnd();
            p.WaitForExit(30000);
            return p.HasExited ? p.ExitCode : -1;
        }
        catch (Exception) { return -1; }
    }
}
