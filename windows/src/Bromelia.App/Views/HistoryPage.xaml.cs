using System.Collections.Specialized;
using Bromelia.App.Controls;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Bromelia.Core.Logic;
using Bromelia.Core.Robot;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;

namespace Bromelia.App.Views;

/// <summary>Finished jobs with their logs and output.</summary>
public sealed partial class HistoryPage : Page
{
    readonly ListView _list = new() { SelectionMode = ListViewSelectionMode.Single };
    readonly TextBox _log = new()
    {
        IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.NoWrap,
        FontFamily = new FontFamily("Cascadia Mono, Consolas"), FontSize = 12,
    };
    readonly StackPanel _details = new() { Spacing = 6 };

    public HistoryPage()
    {
        InitializeComponent();
        PageTitle.Text = "History";
        HeaderButtons.Children.Add(PageHelpers.Button("Clear history…", async () =>
        {
            var dlg = new ContentDialog
            {
                Title = "Clear the job history?", Content = "Stored job logs are deleted as well.",
                PrimaryButtonText = "Clear", CloseButtonText = "Cancel", XamlRoot = XamlRoot,
            };
            if (await dlg.ShowAsync() == ContentDialogResult.Primary) { App.State.ClearHistory(); Rebuild(); }
        }));
        var grid = new Grid { ColumnSpacing = 12, RowSpacing = 12 };
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(280) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(2, GridUnitType.Star) });
        grid.Children.Add(_list);
        Grid.SetColumnSpan(_list, 2);
        var detailsScroll = new ScrollViewer { Content = _details };
        Grid.SetRow(detailsScroll, 1);
        grid.Children.Add(detailsScroll);
        Grid.SetRow(_log, 1);
        Grid.SetColumn(_log, 1);
        grid.Children.Add(_log);
        Body.Content = grid;
        _list.SelectionChanged += (_, _) => ShowSelected();
        Rebuild();
    }

    void Rebuild()
    {
        _list.Items.Clear();
        foreach (var r in App.State.History)
        {
            var dur = r.StartedAt is { } s && r.FinishedAt is { } f ? JobRunner.FormatElapsed(f - s) : "";
            var g = new Grid { ColumnSpacing = 12, Tag = r.Id };
            foreach (var w in new[] { 150, 0, 160, 110, 100, 60, 70 })
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = w == 0 ? new GridLength(1, GridUnitType.Star) : new GridLength(w) });
            string[] cols = { r.FinishedAt?.ToString("g") ?? "—", r.DiscName, r.DriveName, r.Mode.ShortLabel(), r.State.Label(), r.Files.Count.ToString(), dur };
            for (int i = 0; i < cols.Length; i++)
            {
                var tb = new TextBlock { Text = cols[i], TextTrimming = TextTrimming.CharacterEllipsis };
                if (i == 4 && r.State != JobState.Succeeded)
                    tb.Foreground = new SolidColorBrush(r.State == JobState.CompletedWithErrors ? Microsoft.UI.Colors.DarkOrange : Microsoft.UI.Colors.IndianRed);
                Grid.SetColumn(tb, i);
                g.Children.Add(tb);
            }
            _list.Items.Add(g);
        }
    }

    void ShowSelected()
    {
        _details.Children.Clear();
        if (_list.SelectedItem is not FrameworkElement { Tag: Guid id } || App.State.History.FirstOrDefault(h => h.Id == id) is not { } r) return;
        _details.Children.Add(new TextBlock { Text = r.Title, Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"], TextWrapping = TextWrapping.Wrap });
        if (r.ErrorMessage != null) _details.Children.Add(new TextBlock { Text = r.ErrorMessage, Foreground = new SolidColorBrush(Microsoft.UI.Colors.IndianRed), TextWrapping = TextWrapping.Wrap });
        if (r.OutputDirectory is { } dir) _details.Children.Add(PageHelpers.Button("Open output folder", () => Shell.Open(dir)));
        _details.Children.Add(PageHelpers.Button("Open log file", () => Shell.Open(r.LogPath)));
        foreach (var f in r.Files) _details.Children.Add(Form.Help(f));
        try { _log.Text = File.Exists(r.LogPath) ? File.ReadAllText(r.LogPath) : "Log not available"; }
        catch (IOException e) { _log.Text = e.Message; }
    }
}
