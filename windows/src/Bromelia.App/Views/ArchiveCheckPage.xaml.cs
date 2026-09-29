using System.ComponentModel;
using Bromelia.App.Controls;
using Bromelia.Core.Engine;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;

namespace Bromelia.App.Views;

/// <summary>Reads archive folders again and compares every file with its SHA256SUMS (bit rot, bad copies).</summary>
public sealed partial class ArchiveCheckPage : Page
{
    readonly TextBlock _last = Form.Help("");
    readonly ProgressBar _progress = new() { Maximum = 1 };
    readonly TextBlock _progressText = new() { TextTrimming = TextTrimming.CharacterEllipsis };
    readonly TextBlock _resultsTitle = new() { TextWrapping = TextWrapping.Wrap };
    readonly StackPanel _results = new() { Spacing = 8 };
    readonly Button _stop, _checkFolder, _checkRoot;
    bool? _shownRunning;

    AppState State => App.State;

    public ArchiveCheckPage()
    {
        InitializeComponent();
        PageTitle.Text = "Archive check";
        _stop = PageHelpers.Button("Stop", () => State.CancelVerify());
        _checkFolder = PageHelpers.Button("Check a folder…", async () =>
        {
            if (await Pickers.PickFolderAsync() is { } folder) State.StartVerify(folder);
        });
        _checkRoot = PageHelpers.Button("Check output folder", () =>
        {
            if (!Directory.Exists(State.OutputRootPath)) State.LastError = $"The output folder {State.OutputRootPath} doesn't exist";
            else State.StartVerify(State.OutputRootPath);
        }, accent: true);
        HeaderButtons.Children.Add(_stop);
        HeaderButtons.Children.Add(_checkFolder);
        HeaderButtons.Children.Add(_checkRoot);
        _resultsTitle.Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"];

        var top = new StackPanel { Spacing = 8 };
        top.Children.Add(Form.Help("Reads every file listed in each archive folder's SHA256SUMS again and compares it with its checksum, to find files "
                                   + "damaged on the disk (bit rot) or by a bad copy. Files missing from the folder and files SHA256SUMS doesn't list are reported too."));
        top.Children.Add(_last);
        top.Children.Add(_progress);
        top.Children.Add(_progressText);
        top.Children.Add(_resultsTitle);
        var root = new Grid { RowSpacing = 10 };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.Children.Add(top);
        var scroll = new ScrollViewer { Content = _results };
        Grid.SetRow(scroll, 1);
        root.Children.Add(scroll);
        Body.Content = root;
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        State.PropertyChanged += OnStateChanged;
        _shownRunning = null;
        Refresh();
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e) => State.PropertyChanged -= OnStateChanged;

    void OnStateChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(AppState.Verify) or nameof(AppState.CheckRecords)) Refresh();
    }

    void Refresh()
    {
        var v = State.Verify;
        var root = State.OutputRootPath;
        _last.Text = (State.CheckRecords.TryGetValue(root, out var c)
                         ? $"Output folder {root}: last checked {c.CheckedAt:g} — {c.Summary}"
                         : $"Output folder {root}: never checked")
                     + (State.Config.ArchiveCheck.IntervalDays > 0 ? $". Checked every {State.Config.ArchiveCheck.IntervalDays} day(s) (Settings)." : "");
        var running = v.Running ? Visibility.Visible : Visibility.Collapsed;
        _progress.Visibility = _progressText.Visibility = _stop.Visibility = running;
        _checkFolder.IsEnabled = _checkRoot.IsEnabled = !v.Running;
        if (v.Running)
        {
            _progress.Value = v.Total > 0 ? (double)v.Done / v.Total : 0;
            _progressText.Text = $"{v.Folder ?? v.Path}{(v.File != null ? " — " + v.File : "")}   {FormatBytes(v.Done)} of {FormatBytes(v.Total)}";
        }
        if (_shownRunning == v.Running) return;
        _shownRunning = v.Running;
        _results.Opacity = v.Running ? 0.5 : 1;
        if (v.Running) return;
        _results.Children.Clear();
        foreach (var r in v.Results) _results.Children.Add(FolderRow(r));
        var damaged = v.Results.Count(r => !r.Ok);
        _resultsTitle.Text = v.FinishedAt is not { } finished ? ""
            : v.Results.Count == 0 ? $"{finished:g}: no archive folders (with a SHA256SUMS) found in {v.Path}"
            : $"{finished:g}{(v.Stopped ? " (stopped)" : "")}: {v.Results.Count} folder(s) checked, {(damaged > 0 ? "some are damaged" : "all OK")}";
    }

    static string FormatBytes(long n) => Bromelia.Core.Robot.TitleInfo.FormatBytes(n);

    static Expander FolderRow(ArchiveVerifier.FolderCheck r)
    {
        var red = new SolidColorBrush(Microsoft.UI.Colors.IndianRed);
        var header = new Grid { ColumnSpacing = 10 };
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.Children.Add(new FontIcon { Glyph = r.Ok ? "" : "", Foreground = r.Ok ? new SolidColorBrush(Microsoft.UI.Colors.SeaGreen) : red });
        var text = new StackPanel();
        text.Children.Add(new TextBlock { Text = System.IO.Path.GetFileName(r.Folder.TrimEnd('\\', '/')) });
        text.Children.Add(Form.Help(r.Folder));
        var summary = Form.Help(r.Summary);
        if (!r.Ok) summary.Foreground = red;
        text.Children.Add(summary);
        Grid.SetColumn(text, 1);
        header.Children.Add(text);
        var open = PageHelpers.Button("Open folder", () => Shell.Open(r.Folder));
        Grid.SetColumn(open, 2);
        header.Children.Add(open);

        var files = new StackPanel { Spacing = 2 };
        void Add(string what, List<string> list, bool problem)
        {
            foreach (var f in list.Take(50))
            {
                var row = new Grid();
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.Children.Add(new TextBlock { Text = f, TextTrimming = TextTrimming.CharacterEllipsis, IsTextSelectionEnabled = true });
                var label = new TextBlock { Text = what, Margin = new Thickness(12, 0, 0, 0) };
                if (problem) label.Foreground = red;
                Grid.SetColumn(label, 1);
                row.Children.Add(label);
                files.Children.Add(row);
            }
            if (list.Count > 50) files.Children.Add(Form.Help($"… and {list.Count - 50} more {what}"));
        }
        Add("changed", r.Changed, true);
        Add("unreadable", r.Unreadable, true);
        Add("missing", r.Missing, true);
        Add("not listed in SHA256SUMS", r.Extra, false);
        return new Expander
        {
            Header = header, Content = files, IsExpanded = !r.Ok,
            HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
        };
    }
}
