using Bromelia.App.Controls;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;

namespace Bromelia.App.Views;

/// <summary>Application settings: tools, global MakeMKV settings, default drive, drives & presets, registration.</summary>
public sealed partial class SettingsPage : Page
{
    AppState State => App.State;

    public SettingsPage()
    {
        InitializeComponent();
        PageTitle.Text = "Settings";
        var pivot = new Pivot();
        pivot.Items.Add(Tab("General", General()));
        pivot.Items.Add(Tab("MakeMKV", SettingsCatalogPanel.Build(State.Config.GlobalSettings, State.Config.GlobalSettings, driveMode: false, State.ConfigChanged)));
        pivot.Items.Add(new PivotItem { Header = "Default drive", Content = DriveConfigEditor.Build(State.Config.DefaultDrive, true, null, State.ConfigChanged) });
        pivot.Items.Add(Tab("Drives & presets", Drives()));
        pivot.Items.Add(new PivotItem
        {
            Header = "Plugins",
            Content = new PostProcessEditor(State.Config.Plugins, "All drives", State.ConfigChanged,
                "Plugins are post-processing steps for every drive, usually limited to a movie or show (by name or disc label) and to formats such as DVD or 4Ke — for example a script that archives one series in a particular way. They run after the drive's own steps."),
        });
        pivot.Items.Add(Tab("Registration", Registration()));
        Body.Content = pivot;
    }

    static PivotItem Tab(string header, UIElement content) =>
        new() { Header = header, Content = new ScrollViewer { Content = content, Padding = new Thickness(0, 0, 16, 24) } };

    UIElement General()
    {
        var c = State.Config;
        var f = new Form();
        f.Changed += State.ConfigChanged;
        f.Section("Tools");
        f.PathPicker("makemkvcon", () => c.MakemkvconPath, v => c.MakemkvconPath = v, folder: false, placeholder: State.Makemkvcon ?? "Not found — install MakeMKV");
        f.Note(State.Makemkvcon is { } m ? $"Using {m}" : "makemkvcon not found. Install MakeMKV from makemkv.com.");
        f.PathPicker("mkvmerge", () => c.MkvmergePath, v => c.MkvmergePath = v, folder: false, placeholder: State.Mkvmerge ?? "Not found — optional");
        f.Note(State.Mkvmerge is { } mm ? $"Using {mm}" : "mkvmerge not found. Install MKVToolNix to choose individual tracks.");
        f.Section("Output");
        f.PathPicker("Default output folder", () => c.OutputRoot, v => c.OutputRoot = v, folder: true);
        f.Section("Drive detection", "Disc insertion and removal are also detected instantly. Polling runs makemkvcon to refresh drive and disc names; pausing it during rips avoids disturbing busy drives.");
        f.Number("Poll drives every (0 = only on media changes)", () => c.PollIntervalSeconds, v => c.PollIntervalSeconds = v, 0, 3600, suffix: "seconds");
        f.Toggle("Keep polling while jobs are running", () => c.PollWhileRipping, v => c.PollWhileRipping = v);
        f.Section("Jobs");
        f.Number("Maximum simultaneous jobs (0 = one per drive, no global limit)", () => c.MaxConcurrentJobs, v => c.MaxConcurrentJobs = v, 0, 64);
        f.Number("Keep history for", () => c.HistoryLimit, v => c.HistoryLimit = v, 10, 100000, suffix: "jobs");
        f.Number("Stop a stuck rip after (0 = never)", () => c.StallTimeoutMinutes, v => c.StallTimeoutMinutes = v, 0, 1440, suffix: "minutes without output");
        f.Toggle("Keep the computer awake while jobs run", () => c.PreventSleep, v => { c.PreventSleep = v; State.UpdateKeepAwake(); });
        f.Section("MakeMKV");
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        buttons.Children.Add(PageHelpers.Button("Import settings from MakeMKV", () => State.ImportSettings(MakeMKVEnvironment.InstalledSettings())));
        buttons.Children.Add(PageHelpers.Button("Open Bromelia data folder", () => { Directory.CreateDirectory(Paths.AppData); Shell.Open(Paths.AppData); }));
        f.Add(buttons);
        if (State.MakemkvVersion.Length > 0) f.Note("Detected: " + State.MakemkvVersion);
        foreach (var msg in State.ScanMessages) f.Note(msg.Text);
        return f.Root;
    }

    UIElement Drives()
    {
        var root = new StackPanel();
        void Rebuild()
        {
            root.Children.Clear();
            var f = new Form();
            f.Section("Drive configurations");
            if (State.Config.Drives.Count == 0) f.Note("No drives configured yet. Select a drive and choose “Set up this drive…”.");
            foreach (var d in State.Config.Drives.ToList())
            {
                var row = new Grid { ColumnSpacing = 8 };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                var info = new StackPanel();
                info.Children.Add(new TextBlock { Text = d.Name });
                info.Children.Add(Form.Help((d.Match.DriveName.Length > 0 ? d.Match.DriveName : d.Match.DevicePath) + $" · {d.Rip.Mode.ShortLabel()} · {d.PostProcess.Count} step(s)"));
                row.Children.Add(info);
                var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
                var id = d.Id;
                buttons.Children.Add(PageHelpers.Button("Edit…", () => App.MainWindow.ShowDriveConfig(id)));
                buttons.Children.Add(PageHelpers.Button("Duplicate", () =>
                {
                    var copy = d.Clone();
                    copy.Id = Guid.NewGuid();
                    copy.Name = d.Name + " copy";
                    copy.Match = new DriveMatch();
                    foreach (var s in copy.PostProcess) s.Id = Guid.NewGuid();
                    State.Config.Drives.Add(copy);
                    State.ConfigChanged();
                    Rebuild();
                }));
                Grid.SetColumn(buttons, 1);
                row.Children.Add(buttons);
                f.Add(row);
            }
            f.Section("Presets", "Presets store everything except a drive's name and identification, so they can be applied to other drives from the configuration page.");
            if (State.Config.Presets.Count == 0) f.Note("No presets yet.");
            foreach (var p in State.Config.Presets.ToList())
            {
                var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
                var name = new TextBox { Text = p.Name, Width = 320 };
                name.TextChanged += (_, _) => { p.Name = name.Text; State.ConfigChanged(); };
                row.Children.Add(name);
                row.Children.Add(PageHelpers.Button("Delete", () => { State.Config.Presets.Remove(p); State.ConfigChanged(); Rebuild(); }));
                f.Add(row);
            }
            f.Section("Import / export", "Exports are plain JSON and work with the macOS and Linux versions of Bromelia.");
            var status = Form.Help("");
            var io = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            io.Children.Add(PageHelpers.Button("Export drives and presets…", async () =>
            {
                if (await Pickers.SaveFileAsync("bromelia-drives", "JSON", ".json") is not { } path) return;
                var bundle = new ConfigStore.ExportBundle { Drives = State.Config.Drives, Presets = State.Config.Presets };
                await File.WriteAllTextAsync(path, ConfigJson.Serialize(bundle));
                status.Text = $"Exported {bundle.Drives.Count} drive(s) and {bundle.Presets.Count} preset(s).";
            }));
            io.Children.Add(PageHelpers.Button("Import…", async () =>
            {
                if (await Pickers.PickFileAsync(".json") is not { } path) return;
                try
                {
                    var bundle = System.Text.Json.JsonSerializer.Deserialize<ConfigStore.ExportBundle>(await File.ReadAllTextAsync(path), ConfigJson.Options)!;
                    foreach (var d in bundle.Drives)
                    {
                        int i = State.Config.Drives.FindIndex(x => x.Id == d.Id);
                        if (i >= 0) State.Config.Drives[i] = d; else State.Config.Drives.Add(d);
                    }
                    foreach (var p in bundle.Presets.Where(p => State.Config.Presets.All(x => x.Id != p.Id))) State.Config.Presets.Add(p);
                    State.ConfigChanged();
                    Rebuild();
                }
                catch (Exception e) { status.Text = "Import failed: " + e.Message; }
            }));
            f.Add(io);
            f.Add(status);
            root.Children.Add(f.Root);
        }
        Rebuild();
        return root;
    }

    UIElement Registration()
    {
        var c = State.Config;
        var f = new Form();
        f.Section("MakeMKV registration");
        var key = new PasswordBox { Password = c.RegistrationKey, Width = 420, PlaceholderText = "Registration key" };
        key.PasswordChanged += (_, _) => { c.RegistrationKey = key.Password; State.ConfigChanged(); };
        f.Row("Registration key", key, "Empty: the key MakeMKV itself is registered with is used. Otherwise this key is passed to makemkvcon for every drive.");
        var result = Form.Help("");
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        buttons.Children.Add(PageHelpers.Button("Register key with MakeMKV", async () =>
        {
            if (c.RegistrationKey.Length == 0) return;
            result.Text = "Registering…";
            result.Text = await State.RegisterWithMakeMkvAsync(c.RegistrationKey);
        }));
        buttons.Children.Add(PageHelpers.Button("Get a key…", () => Shell.Open("https://www.makemkv.com/buy/")));
        buttons.Children.Add(PageHelpers.Button("Get the current beta key", async () =>
        {
            result.Text = "Getting the current beta key…";
            result.Text = await State.InstallBetaKeyAsync();
        }));
        buttons.Children.Add(PageHelpers.Button("Beta key forum page…", () => Shell.Open(BetaKey.PageUrl)));
        f.Add(buttons);
        f.Add(result);
        f.Note("“Register key with MakeMKV” runs makemkvcon reg, which stores the key in MakeMKV's own settings so the MakeMKV app uses it too. “Get the current beta key” reads the free beta key from MakeMKV's forum and registers it the same way.");
        return f.Root;
    }
}
