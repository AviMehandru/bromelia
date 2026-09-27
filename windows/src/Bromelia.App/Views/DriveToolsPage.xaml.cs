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

/// <summary>MakeMKV's firmware utility (read-only commands).</summary>
public sealed partial class DriveToolsPage : Page
{
    readonly TextBox _output = new()
    {
        IsReadOnly = true, AcceptsReturn = true, FontFamily = new FontFamily("Cascadia Mono, Consolas"), FontSize = 12,
        TextWrapping = TextWrapping.NoWrap,
    };
    readonly ComboBox _drive = new() { IsEditable = true, Width = 320, PlaceholderText = "Drive (device or name from the list)" };

    public DriveToolsPage()
    {
        InitializeComponent();
        PageTitle.Text = "Drive tools";
        foreach (var e in App.State.ScannedDrives.Where(d => d.IsPresent)) _drive.Items.Add(e.DevicePath.Length > 0 ? e.DevicePath : e.DriveName);
        var bar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        bar.Children.Add(PageHelpers.Button("List drives", () => Run("f", "-l")));
        bar.Children.Add(_drive);
        bar.Children.Add(PageHelpers.Button("Drive commands", () =>
        {
            var d = _drive.Text?.Trim() ?? "";
            if (d.Length > 0) Run("f", "-d", d, "help");
        }));
        bar.Children.Add(PageHelpers.Button("SDF info", () => Run("f", "--info")));
        bar.Children.Add(PageHelpers.Button("Open folder as source…", async () =>
        {
            if (await Pickers.PickFolderAsync() is { } folder) App.MainWindow.OpenSource(folder);
        }));
        var root = new Grid { RowSpacing = 10 };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.Children.Add(Form.Help("Uses MakeMKV's firmware utility (makemkvcon f). Only read-only commands are offered; flashing firmware must be done from the command line."));
        Grid.SetRow(bar, 1);
        root.Children.Add(bar);
        Grid.SetRow(_output, 2);
        root.Children.Add(_output);
        Body.Content = root;
    }

    async void Run(params string[] args)
    {
        if (App.State.Makemkvcon is not { } exe) { _output.Text = "makemkvcon not found"; return; }
        _output.Text = "> makemkvcon " + string.Join(" ", args.Select(ArgumentSplitter.Quote)) + Environment.NewLine;
        var lines = new LineCollector();
        try
        {
            var r = await new ProcessRunner(exe, args).RunAsync(lines.Add, TimeSpan.FromMinutes(2));
            _output.Text += string.Join(Environment.NewLine, lines.All) + $"{Environment.NewLine}{Environment.NewLine}(exit status {r.ExitCode})";
        }
        catch (Exception e) { _output.Text += e.Message; }
    }
}
