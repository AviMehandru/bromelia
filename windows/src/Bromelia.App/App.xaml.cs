using Bromelia.App.Services;
using Bromelia.Core.Engine;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace Bromelia.App;

public partial class App : Application
{
    public static AppState State { get; private set; } = null!;
    public static MainWindow MainWindow { get; private set; } = null!;
    public static DispatcherQueue UiQueue { get; private set; } = null!;

    DispatcherQueueTimer? _tick;
    DispatcherQueueTimer? _poll;
    MediaWatcher? _watcher;

    public App()
    {
        InitializeComponent();
        UnhandledException += (_, e) =>
        {
            State?.SaveNow();
            System.Diagnostics.Debug.WriteLine(e.Exception);
        };
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        UiQueue = DispatcherQueue.GetForCurrentThread();
        var platform = new WindowsPlatformServices();
        platform.RegisterNotifications();

        // Created on the UI thread: the engine captures this thread's SynchronizationContext.
        State = new AppState(platform);
        State.ImportFromMakeMkvIfFirstRun();
        State.StartServices();

        MainWindow = new MainWindow();
        MainWindow.Activate();

        // Queue pump / countdowns.
        _tick = UiQueue.CreateTimer();
        _tick.Interval = TimeSpan.FromSeconds(1);
        _tick.Tick += (_, _) => State.Pump();
        _tick.Start();

        // Periodic drive polling.
        _poll = UiQueue.CreateTimer();
        _poll.Interval = TimeSpan.FromSeconds(Math.Max(3, State.Config.PollIntervalSeconds));
        _poll.Tick += async (t, _) =>
        {
            t.Interval = TimeSpan.FromSeconds(Math.Max(3, State.Config.PollIntervalSeconds));
            if (State.Config.PollIntervalSeconds > 0) await State.RefreshDrivesAsync(false);
        };
        _poll.Start();

        // Instant media change detection (tray open / disc inserted).
        _watcher = new MediaWatcher(UiQueue, () => _ = State.RefreshDrivesAsync(true));
        _watcher.Start();

        _ = State.RefreshDrivesAsync(true);

        if (Snapshots.Folder is { } shots) _ = Snapshots.RunAsync(shots);
    }
}
