using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Storage.Pickers;

namespace Bromelia.App.Controls;

/// <summary>
/// Builds settings-style forms (sections of cards with labelled rows) in code, wiring each control to
/// getter / setter delegates. Used for drive configurations and the MakeMKV settings catalog.
/// </summary>
public sealed class Form
{
    public StackPanel Root { get; } = new() { Spacing = 0, MaxWidth = 900, HorizontalAlignment = HorizontalAlignment.Stretch };
    StackPanel? _card;

    /// <summary>Raised after any control changes a value.</summary>
    public event Action? Changed;

    void Raise() => Changed?.Invoke();

    public Form Section(string title, string? footer = null)
    {
        Root.Children.Add(new TextBlock { Text = title, Style = Res<Style>("SectionHeaderStyle") });
        _card = new StackPanel { Spacing = 10 };
        Root.Children.Add(new Border { Style = Res<Style>("CardStyle"), Child = _card });
        if (footer != null) Root.Children.Add(Help(footer, new Thickness(4, 4, 0, 0)));
        return this;
    }

    StackPanel Card => _card ?? Section("").Card;

    static T Res<T>(string key) => (T)Application.Current.Resources[key];

    public static TextBlock Help(string text, Thickness? margin = null) =>
        new() { Text = text, Style = Res<Style>("HelpTextStyle"), Margin = margin ?? new Thickness(0) };

    public void Add(UIElement e) => Card.Children.Add(e);

    public void Note(string text) => Card.Children.Add(Help(text));

    /// <summary>Label on the left, control on the right, optional help text underneath.</summary>
    public Grid Row(string label, FrameworkElement control, string? help = null)
    {
        var g = new Grid { ColumnSpacing = 16 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var left = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        left.Children.Add(new TextBlock { Text = label, TextWrapping = TextWrapping.Wrap });
        if (help != null) left.Children.Add(Help(help));
        g.Children.Add(left);
        control.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(control, 1);
        g.Children.Add(control);
        Card.Children.Add(g);
        return g;
    }

    public TextBox Text(string label, Func<string> get, Action<string> set, string? placeholder = null, string? help = null,
        double width = 360, bool monospace = false)
    {
        var tb = new TextBox { Text = get(), PlaceholderText = placeholder ?? "", Width = width };
        if (monospace) tb.FontFamily = new FontFamily("Cascadia Mono, Consolas");
        tb.TextChanged += (_, _) => { if (tb.Text == get()) return; set(tb.Text); Raise(); };
        Row(label, tb, help);
        return tb;
    }

    public ToggleSwitch Toggle(string label, Func<bool> get, Action<bool> set, string? help = null)
    {
        var ts = new ToggleSwitch { IsOn = get(), OnContent = "", OffContent = "", MinWidth = 0 };
        ts.Toggled += (_, _) => { set(ts.IsOn); Raise(); };
        Row(label, ts, help);
        return ts;
    }

    public NumberBox Number(string label, Func<int> get, Action<int> set, int min = 0, int max = int.MaxValue, string? help = null, string? suffix = null)
    {
        var nb = new NumberBox
        {
            Value = get(), Minimum = min, Maximum = max, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact,
            Width = 140, SmallChange = 1, LargeChange = 10,
        };
        nb.ValueChanged += (_, a) =>
        {
            if (double.IsNaN(a.NewValue)) { nb.Value = get(); return; }
            set((int)Math.Round(a.NewValue));
            Raise();
        };
        Row(suffix == null ? label : $"{label} ({suffix})", nb, help);
        return nb;
    }

    /// <summary>Number box where empty means null ("use default").</summary>
    public NumberBox OptionalNumber(string label, Func<int?> get, Action<int?> set, string placeholder = "Default", string? help = null)
    {
        var nb = new NumberBox { PlaceholderText = placeholder, Width = 140, Minimum = 0, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Hidden };
        nb.Value = get() is { } v ? v : double.NaN;
        nb.ValueChanged += (_, a) => { set(double.IsNaN(a.NewValue) ? null : (int)Math.Round(a.NewValue)); Raise(); };
        Row(label, nb, help);
        return nb;
    }

    public ComboBox Choice<T>(string label, IEnumerable<(T Value, string Label)> options, Func<T> get, Action<T> set, string? help = null, double width = 280)
    {
        var list = options.ToList();
        var cb = new ComboBox { Width = width };
        foreach (var o in list) cb.Items.Add(o.Label);
        var current = get();
        cb.SelectedIndex = Math.Max(0, list.FindIndex(o => EqualityComparer<T>.Default.Equals(o.Value, current)));
        cb.SelectionChanged += (_, _) =>
        {
            if (cb.SelectedIndex >= 0) { set(list[cb.SelectedIndex].Value); Raise(); }
        };
        Row(label, cb, help);
        return cb;
    }

    public TextBox PathPicker(string label, Func<string> get, Action<string> set, bool folder, string? placeholder = null, string? help = null)
    {
        var tb = new TextBox { Text = get(), PlaceholderText = placeholder ?? "", Width = 300 };
        tb.TextChanged += (_, _) => { if (tb.Text == get()) return; set(tb.Text); Raise(); };
        var btn = new Button { Content = "Browse…" };
        btn.Click += async (_, _) =>
        {
            var path = folder ? await Pickers.PickFolderAsync() : await Pickers.PickFileAsync();
            if (path != null) tb.Text = path;
        };
        var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        panel.Children.Add(tb);
        panel.Children.Add(btn);
        Row(label, panel, help);
        return tb;
    }

    /// <summary>Row with a checkbox ("override") in front of the control.</summary>
    public (CheckBox Check, Grid Row) OverrideRow(string label, FrameworkElement control, bool overridden, Action<bool> onToggle, string? help, string inherited)
    {
        var check = new CheckBox { IsChecked = overridden, MinWidth = 0, Content = label, VerticalAlignment = VerticalAlignment.Center };
        var g = new Grid { ColumnSpacing = 16 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var left = new StackPanel();
        left.Children.Add(check);
        if (help != null) left.Children.Add(Help(help, new Thickness(28, 0, 0, 0)));
        var inheritedText = Help("Inherited: " + inherited, new Thickness(28, 0, 0, 0));
        inheritedText.Visibility = overridden ? Visibility.Collapsed : Visibility.Visible;
        left.Children.Add(inheritedText);
        g.Children.Add(left);
        control.VerticalAlignment = VerticalAlignment.Center;
        SetEnabled(control, overridden);
        Grid.SetColumn(control, 1);
        g.Children.Add(control);
        check.Checked += (_, _) => { SetEnabled(control, true); inheritedText.Visibility = Visibility.Collapsed; onToggle(true); Raise(); };
        check.Unchecked += (_, _) => { SetEnabled(control, false); inheritedText.Visibility = Visibility.Visible; onToggle(false); Raise(); };
        Card.Children.Add(g);
        return (check, g);
    }

    static void SetEnabled(FrameworkElement e, bool enabled)
    {
        if (e is Control c) c.IsEnabled = enabled;
        else if (e is Panel p) foreach (var child in p.Children.OfType<Control>()) child.IsEnabled = enabled;
    }
}

/// <summary>File / folder pickers initialised for the main window (required for unpackaged WinUI apps).</summary>
public static class Pickers
{
    static IntPtr Hwnd => WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow);

    public static async Task<string?> PickFolderAsync()
    {
        var p = new FolderPicker();
        p.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(p, Hwnd);
        var f = await p.PickSingleFolderAsync();
        return f?.Path;
    }

    public static async Task<string?> PickFileAsync(params string[] extensions)
    {
        var p = new FileOpenPicker();
        if (extensions.Length == 0) p.FileTypeFilter.Add("*");
        foreach (var e in extensions) p.FileTypeFilter.Add(e);
        WinRT.Interop.InitializeWithWindow.Initialize(p, Hwnd);
        var f = await p.PickSingleFileAsync();
        return f?.Path;
    }

    public static async Task<IReadOnlyList<string>> PickFilesAsync(params string[] extensions)
    {
        var p = new FileOpenPicker();
        foreach (var e in extensions) p.FileTypeFilter.Add(e);
        if (extensions.Length == 0) p.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(p, Hwnd);
        var files = await p.PickMultipleFilesAsync();
        return files.Select(f => f.Path).ToList();
    }

    public static async Task<string?> SaveFileAsync(string suggestedName, string typeLabel, string extension)
    {
        var p = new FileSavePicker { SuggestedFileName = suggestedName };
        p.FileTypeChoices.Add(typeLabel, new List<string> { extension });
        WinRT.Interop.InitializeWithWindow.Initialize(p, Hwnd);
        var f = await p.PickSaveFileAsync();
        return f?.Path;
    }
}
