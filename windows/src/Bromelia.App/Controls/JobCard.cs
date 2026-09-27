using System.Collections.Specialized;
using System.ComponentModel;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Bromelia.Core.Robot;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Windows.ApplicationModel.DataTransfer;

namespace Bromelia.App.Controls;

/// <summary>Progress card for one job.</summary>
public sealed class JobCard : UserControl
{
    readonly RipJob _job;
    readonly TextBlock _title = new() { Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"], TextTrimming = TextTrimming.CharacterEllipsis };
    readonly TextBlock _state = new() { Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"] };
    readonly TextBlock _phase = new();
    readonly TextBlock _timing = new() { Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"], HorizontalAlignment = HorizontalAlignment.Right };
    readonly ProgressBar _overall = new() { Maximum = 1, Height = 6 };
    readonly TextBlock _overallText = new() { Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"] };
    readonly ProgressBar _current = new() { Maximum = 1, Height = 4, Opacity = 0.7 };
    readonly TextBlock _currentText = new() { Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"] };
    readonly TextBlock _error = new() { Foreground = new SolidColorBrush(Colors.IndianRed), TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true };
    readonly TextBlock _summary = new() { Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"] };
    readonly StackPanel _progressPanel = new() { Spacing = 4 };
    readonly StackPanel _buttons = new() { Orientation = Orientation.Horizontal, Spacing = 8 };
    readonly LogList _log;
    readonly ToggleButton _logToggle = new() { Content = "Log" };

    public JobCard(RipJob job)
    {
        _job = job;
        _log = new LogList(job.Log) { Height = 220, Visibility = Visibility.Collapsed };
        _logToggle.Checked += (_, _) => _log.Visibility = Visibility.Visible;
        _logToggle.Unchecked += (_, _) => _log.Visibility = Visibility.Collapsed;

        var header = new Grid { ColumnSpacing = 8 };
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.Children.Add(_title);
        Grid.SetColumn(_state, 1);
        header.Children.Add(_state);

        var phaseRow = new Grid();
        phaseRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        phaseRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        phaseRow.Children.Add(_phase);
        Grid.SetColumn(_timing, 1);
        phaseRow.Children.Add(_timing);

        _progressPanel.Children.Add(phaseRow);
        _progressPanel.Children.Add(_overall);
        _progressPanel.Children.Add(_overallText);
        _progressPanel.Children.Add(_current);
        _progressPanel.Children.Add(_currentText);

        var root = new StackPanel { Spacing = 8 };
        root.Children.Add(header);
        root.Children.Add(_progressPanel);
        root.Children.Add(_error);
        root.Children.Add(_summary);
        root.Children.Add(_buttons);
        root.Children.Add(_log);
        Content = new Border { Style = (Style)Application.Current.Resources["CardStyle"], Child = root };

        _job.PropertyChanged += OnJobChanged;
        Unloaded += (_, _) => _job.PropertyChanged -= OnJobChanged;
        Loaded += (_, _) => { _job.PropertyChanged -= OnJobChanged; _job.PropertyChanged += OnJobChanged; Update(); };
        Update();
    }

    void OnJobChanged(object? sender, PropertyChangedEventArgs e) => Update(e.PropertyName);

    string? _lastButtonsState;

    void Update(string? property = null)
    {
        var j = _job;
        _title.Text = $"{j.Title}   ·   {j.Mode.ShortLabel()}{(j.IsAutomatic ? " · automatic" : "")}";
        _state.Text = j.StateLabel;
        bool live = j.State is JobState.Running or JobState.Waiting or JobState.Queued;
        _progressPanel.Visibility = live ? Visibility.Visible : Visibility.Collapsed;
        if (live)
        {
            _phase.Text = j.State == JobState.Waiting && j.StartAt is { } t
                ? $"Starting in {Math.Max(0, (int)(t - DateTime.Now).TotalSeconds)} s"
                : j.Phase;
            _overall.Value = j.OverallProgress;
            _overall.IsIndeterminate = j.State == JobState.Running && j.OverallProgress <= 0;
            _overallText.Text = $"{(j.TotalOperation.Length == 0 ? "Overall" : j.TotalOperation)} — {j.OverallProgress:P0}";
            _current.Value = Math.Clamp(j.CurrentProgress, 0, 1);
            _currentText.Text = j.CurrentOperation;
            var timing = $"Elapsed {JobRunner.FormatElapsed(j.Elapsed)}";
            if (j.EstimatedRemaining is { } r) timing += $" · {JobRunner.FormatElapsed(r)} left";
            _timing.Text = j.State == JobState.Running ? timing : "";
        }
        _error.Text = j.State is JobState.Failed or JobState.Cancelled ? j.ErrorMessage ?? "" : "";
        _error.Visibility = _error.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        _summary.Text = j.IsFinished ? $"{j.ProducedFiles.Count} item(s) · {JobRunner.FormatElapsed(j.Elapsed)} · {j.WarningCount} warning(s) · {j.ErrorCount} error(s)" : "";
        _summary.Visibility = j.IsFinished ? Visibility.Visible : Visibility.Collapsed;

        var buttonsState = $"{j.State}|{j.OutputDirectory != null}";
        if (buttonsState != _lastButtonsState)
        {
            _lastButtonsState = buttonsState;
            BuildButtons();
        }
    }

    void BuildButtons()
    {
        _buttons.Children.Clear();
        var state = App.State;
        switch (_job.State)
        {
            case JobState.Running:
                _buttons.Children.Add(MakeButton("Cancel", () => state.Cancel(_job)));
                break;
            case JobState.Waiting:
                _buttons.Children.Add(MakeButton("Start now", () => state.StartNow(_job)));
                _buttons.Children.Add(MakeButton("Cancel", () => state.Cancel(_job)));
                break;
            case JobState.Queued:
                _buttons.Children.Add(MakeButton("Remove", () => state.Cancel(_job)));
                _buttons.Children.Add(MakeButton("Move up", () => state.MoveJob(_job, -1)));
                _buttons.Children.Add(MakeButton("Move down", () => state.MoveJob(_job, 1)));
                break;
            case JobState.Failed:
            case JobState.Cancelled:
                _buttons.Children.Add(MakeButton("Retry", () => state.Retry(_job)));
                break;
        }
        if (_job.OutputDirectory is { } dir)
            _buttons.Children.Add(MakeButton("Open folder", () => Shell.Open(dir)));
        _buttons.Children.Add(_logToggle);
        _buttons.Children.Add(MakeButton("Copy commands", () =>
        {
            var dp = new DataPackage();
            dp.SetText(string.Join(Environment.NewLine, _job.Commands));
            Clipboard.SetContent(dp);
        }));
        _buttons.Children.Add(MakeButton("Open log file", () => Shell.Open(_job.LogFile)));
    }

    static Button MakeButton(string text, Action onClick)
    {
        var b = new Button { Content = text };
        b.Click += (_, _) => onClick();
        return b;
    }
}

/// <summary>Scrolling, colour-coded log list bound to an observable collection.</summary>
public sealed class LogList : UserControl
{
    readonly ListView _list = new() { SelectionMode = ListViewSelectionMode.None, IsItemClickEnabled = false };
    readonly CheckBox _onlyProblems = new() { Content = "Warnings and errors only" };
    readonly System.Collections.ObjectModel.ObservableCollection<LogEntry> _source;

    public LogList(System.Collections.ObjectModel.ObservableCollection<LogEntry> source)
    {
        _source = source;
        var root = new Grid { RowSpacing = 4 };
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.Children.Add(_list);
        Grid.SetRow(_onlyProblems, 1);
        root.Children.Add(_onlyProblems);
        Content = root;
        _onlyProblems.Checked += (_, _) => Refresh();
        _onlyProblems.Unchecked += (_, _) => Refresh();
        _source.CollectionChanged += OnChanged;
        Unloaded += (_, _) => _source.CollectionChanged -= OnChanged;
        Loaded += (_, _) => { _source.CollectionChanged -= OnChanged; _source.CollectionChanged += OnChanged; Refresh(); };
        Refresh();
    }

    void Refresh()
    {
        _list.Items.Clear();
        foreach (var e in _source.Where(Include)) _list.Items.Add(Render(e));
        ScrollToEnd();
    }

    bool Include(LogEntry e) => _onlyProblems.IsChecked != true || e.Severity is Severity.Warning or Severity.Error;

    void OnChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.Action == NotifyCollectionChangedAction.Add && e.NewItems != null)
        {
            foreach (LogEntry item in e.NewItems) if (Include(item)) _list.Items.Add(Render(item));
            while (_list.Items.Count > 5000) _list.Items.RemoveAt(0);
            ScrollToEnd();
        }
        else Refresh();
    }

    void ScrollToEnd()
    {
        if (_list.Items.Count > 0) _list.ScrollIntoView(_list.Items[^1]);
    }

    static TextBlock Render(LogEntry e) => new()
    {
        Text = $"{e.TimeText}  {e.Text}",
        FontFamily = new FontFamily("Cascadia Mono, Consolas"),
        FontSize = 12,
        TextWrapping = TextWrapping.Wrap,
        IsTextSelectionEnabled = true,
        Foreground = e.Severity switch
        {
            Severity.Error => new SolidColorBrush(Colors.IndianRed),
            Severity.Warning => new SolidColorBrush(Colors.DarkOrange),
            Severity.Debug => (Brush)Application.Current.Resources["TextFillColorTertiaryBrush"],
            _ => (Brush)Application.Current.Resources["TextFillColorPrimaryBrush"],
        },
    };
}

public static class Shell
{
    public static void Open(string path)
    {
        try
        {
            if (File.Exists(path) && !path.EndsWith(".txt", StringComparison.OrdinalIgnoreCase) && !path.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = true });
            else
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true });
        }
        catch (Exception) { }
    }
}
