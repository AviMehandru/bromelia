using System.Collections.Specialized;
using System.ComponentModel;
using Bromelia.App.Controls;
using Bromelia.App.Views;
using Bromelia.Core.Engine;
using Bromelia.Core.Robot;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;

namespace Bromelia.App;

public sealed partial class MainWindow : Window
{
    AppState State => App.State;
    string _selectedTag = "queue";
    bool _rebuilding;
    NavigationViewItem? _queueItem;

    public MainWindow()
    {
        InitializeComponent();
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "Bromelia.ico"));
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1280, 820));
        AppWindow.Closing += OnClosing;

        State.DrivesChanged += RebuildMenu;
        State.FileSessions.CollectionChanged += (_, _) => RebuildMenu();
        State.Jobs.CollectionChanged += OnJobsChanged;
        State.PropertyChanged += OnStateChanged;
        RebuildMenu();
        Navigate("queue");
    }

    // --- navigation ---------------------------------------------------------------------------

    public void RebuildMenu()
    {
        _rebuilding = true;
        Nav.MenuItems.Clear();
        Nav.MenuItems.Add(new NavigationViewItemHeader { Content = "Drives" });
        var drives = State.DriveItems;
        foreach (var d in drives)
        {
            var job = State.ActiveJob(d.LaneKey);
            Nav.MenuItems.Add(new NavigationViewItem
            {
                Content = DriveLabel(d, job),
                Icon = new FontIcon { Glyph = d.Entry?.State == DriveState.Inserted ? "" : "" },
                Tag = "drive:" + d.Id,
                Opacity = d.IsConnected ? 1 : 0.6,
            });
        }
        if (drives.Count == 0)
            Nav.MenuItems.Add(new NavigationViewItem { Content = State.IsScanning ? "Scanning…" : "No optical drives found", IsEnabled = false });

        if (State.FileSessions.Count > 0)
        {
            Nav.MenuItems.Add(new NavigationViewItemHeader { Content = "Images & Folders" });
            foreach (var s in State.FileSessions)
                Nav.MenuItems.Add(new NavigationViewItem
                {
                    Content = s.Source.DisplayName,
                    Icon = new SymbolIcon(s.Source is DiscSource.Iso ? Symbol.Page2 : Symbol.Folder),
                    Tag = "source:" + s.Id,
                });
        }

        Nav.MenuItems.Add(new NavigationViewItemHeader { Content = "Jobs" });
        _queueItem = new NavigationViewItem { Content = "Queue", Icon = new SymbolIcon(Symbol.List), Tag = "queue" };
        Nav.MenuItems.Add(_queueItem);
        Nav.MenuItems.Add(new NavigationViewItem { Content = "History", Icon = new FontIcon { Glyph = "" }, Tag = "history" });
        UpdateQueueBadge();

        Nav.FooterMenuItems.Clear();
        Nav.FooterMenuItems.Add(new NavigationViewItem { Content = "Open image or folder…", Icon = new SymbolIcon(Symbol.OpenFile), Tag = "action:open", SelectsOnInvoked = false });
        Nav.FooterMenuItems.Add(new NavigationViewItem { Content = "Rescan drives", Icon = new SymbolIcon(Symbol.Refresh), Tag = "action:rescan", SelectsOnInvoked = false });
        Nav.FooterMenuItems.Add(new NavigationViewItem { Content = "Drive tools", Icon = new SymbolIcon(Symbol.Repair), Tag = "tools" });

        var selected = Nav.MenuItems.Concat(Nav.FooterMenuItems).OfType<NavigationViewItem>().FirstOrDefault(i => (i.Tag as string) == _selectedTag);
        if (selected != null) Nav.SelectedItem = selected;
        _rebuilding = false;
        VersionText.Text = State.MakemkvVersion;
    }

    static string DriveLabel(DriveItem d, RipJob? job)
    {
        var name = d.DisplayName + (d.Config == null && d.IsConnected ? " (not set up)" : "") + (d.Config?.Automation.AutoRipOnInsert == true ? " ⚡" : "");
        string sub;
        if (job is { State: JobState.Running }) sub = $"{job.Phase} · {job.OverallProgress:P0}";
        else if (job is { State: JobState.Waiting }) sub = "Automatic rip starting";
        else if (d.Entry == null) sub = "Disconnected";
        else if (d.Entry.State == DriveState.Inserted) sub = d.Entry.DiscName.Length == 0 ? d.Entry.Flags.DiscTypeName() : $"{d.Entry.DiscName} · {d.Entry.Flags.DiscTypeName()}";
        else sub = d.Entry.State.DisplayName();
        return name + "\n" + sub;
    }

    void OnJobsChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.NewItems != null)
            foreach (RipJob j in e.NewItems) j.PropertyChanged += OnJobPropertyChanged;
        UpdateQueueBadge();
        RebuildMenu();
    }

    void OnJobPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(RipJob.State) or nameof(RipJob.Phase))
        {
            UpdateQueueBadge();
            RebuildMenu();
        }
    }

    void UpdateQueueBadge()
    {
        if (_queueItem == null) return;
        var active = State.Jobs.Count(j => !j.State.IsFinished());
        _queueItem.InfoBadge = active > 0 ? new InfoBadge { Value = active } : null;
    }

    void OnStateChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(AppState.LastError):
                ErrorBar.Message = State.LastError ?? "";
                ErrorBar.IsOpen = State.LastError != null;
                break;
            case nameof(AppState.MakemkvProblem):
                if (State.MakemkvProblem is { } p)
                {
                    ProblemBar.Severity = InfoBarSeverity.Error;
                    ProblemBar.Message = p.Explanation;
                    ProblemBar.IsOpen = true;
                }
                else ProblemBar.IsOpen = false;
                break;
            case nameof(AppState.IsScanning):
            case nameof(AppState.LastScan):
                ScanText.Text = State.IsScanning ? "Scanning drives…" : State.LastScan is { } t ? $"Scanned {t:T}" : "";
                break;
            case nameof(AppState.MakemkvVersion):
                VersionText.Text = State.MakemkvVersion;
                break;
        }
    }

    void Nav_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (_rebuilding) return;
        if (args.IsSettingsSelected) { Navigate("settings"); return; }
        if (args.SelectedItemContainer?.Tag is string tag && !tag.StartsWith("action:", StringComparison.Ordinal)) Navigate(tag);
    }

    async void Nav_ItemInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs args)
    {
        switch (args.InvokedItemContainer?.Tag as string)
        {
            case "action:open":
                await OpenSourcesAsync();
                break;
            case "action:rescan":
                await State.RefreshDrivesAsync(true);
                break;
        }
    }

    public void Navigate(string tag)
    {
        _selectedTag = tag;
        var sel = Nav.MenuItems.Concat(Nav.FooterMenuItems).OfType<NavigationViewItem>().FirstOrDefault(i => (i.Tag as string) == tag);
        if (sel != null && !ReferenceEquals(Nav.SelectedItem, sel))
        {
            _rebuilding = true;
            Nav.SelectedItem = sel;
            _rebuilding = false;
        }
        if (tag.StartsWith("drive:", StringComparison.Ordinal) || tag.StartsWith("source:", StringComparison.Ordinal))
            ContentFrame.Navigate(typeof(DrivePage), tag);
        else if (tag == "history") ContentFrame.Navigate(typeof(HistoryPage));
        else if (tag == "settings") ContentFrame.Navigate(typeof(SettingsPage));
        else if (tag == "tools") ContentFrame.Navigate(typeof(DriveToolsPage));
        else ContentFrame.Navigate(typeof(QueuePage));
    }

    public void ShowDriveConfig(Guid configId) => ContentFrame.Navigate(typeof(DriveConfigPage), configId);

    public void ShowError(string message)
    {
        ErrorBar.Message = message;
        ErrorBar.IsOpen = true;
    }

    void ErrorBar_Closed(InfoBar sender, InfoBarClosedEventArgs args) => State.LastError = null;

    void ProblemBar_Closed(InfoBar sender, InfoBarClosedEventArgs args) => State.MakemkvProblem = null;

    async void BetaKeyButton_Click(object sender, RoutedEventArgs e)
    {
        BetaKeyButton.IsEnabled = false;
        ProblemBar.Message = "Getting the current beta key…";
        var result = await State.InstallBetaKeyAsync();
        BetaKeyButton.IsEnabled = true;
        // Registering clears the problem (which closes the bar): reopen it with the result.
        if (State.MakemkvProblem == null) ProblemBar.Severity = InfoBarSeverity.Success;
        ProblemBar.Message = result;
        ProblemBar.IsOpen = true;
    }

    public async Task OpenSourcesAsync()
    {
        // Disc images, or any file on a disc (.IFO, .mpls, .m2ts, …: the disc it belongs to is opened); folders via
        // "Open folder" on the drive tools page or drag & drop.
        var files = await Pickers.PickFilesAsync();
        foreach (var f in files) OpenSource(f);
        if (files.Count == 0)
        {
            var folder = await Pickers.PickFolderAsync();
            if (folder != null) OpenSource(folder);
        }
    }

    public void OpenSource(string path)
    {
        var s = State.OpenFileSource(path);
        Navigate("source:" + s.Id);
    }

    void Root_DragOver(object sender, DragEventArgs e)
    {
        if (e.DataView.Contains(StandardDataFormats.StorageItems)) e.AcceptedOperation = DataPackageOperation.Link;
    }

    async void Root_Drop(object sender, DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.StorageItems)) return;
        foreach (var item in await e.DataView.GetStorageItemsAsync())
            if (item is IStorageItem si && !string.IsNullOrEmpty(si.Path)) OpenSource(si.Path);
    }

    async void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        var running = State.ActiveJobCount;
        if (running == 0) { State.SaveNow(); return; }
        args.Cancel = true;
        var dlg = new ContentDialog
        {
            Title = $"{running} job(s) are still running",
            Content = "Closing Bromelia cancels them. Partially written files are left in place.",
            PrimaryButtonText = "Cancel jobs and quit",
            CloseButtonText = "Keep running",
            DefaultButton = ContentDialogButton.Close,
            XamlRoot = Content.XamlRoot,
        };
        if (await dlg.ShowAsync() == ContentDialogResult.Primary)
        {
            foreach (var j in State.Jobs.Where(j => j.State == JobState.Running).ToList()) State.Cancel(j);
            State.SaveNow();
            await Task.Delay(1500);
            Application.Current.Exit();
        }
    }
}
