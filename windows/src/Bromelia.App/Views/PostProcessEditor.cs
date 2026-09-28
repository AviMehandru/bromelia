using Bromelia.App.Controls;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Bromelia.Core.Logic;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Bromelia.App.Views;

/// <summary>List of post-processing steps with an editor for the selected step.</summary>
public sealed class PostProcessEditor : UserControl
{
    readonly List<PostProcessStep> _steps;
    readonly string _driveName;
    readonly Action _changed;
    readonly string _emptyText;
    readonly ListView _list = new() { SelectionMode = ListViewSelectionMode.Single };
    readonly ScrollViewer _editorHost = new() { Padding = new Thickness(0, 0, 16, 24) };

    public PostProcessEditor(List<PostProcessStep> steps, string driveName, Action changed,
        string emptyText = "Post-processing steps run after each job, in order. Use them to move, rename, encode or catalogue files, or to call any script. Add a step or pick an example.")
    {
        _steps = steps;
        _driveName = driveName;
        _changed = changed;
        _emptyText = emptyText;

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        buttons.Children.Add(IconButton("\uE710", "Add step", () => Add(new PostProcessStep { Name = $"Step {_steps.Count + 1}" })));
        buttons.Children.Add(IconButton("\uE738", "Remove step", Remove));
        buttons.Children.Add(IconButton("\uE70E", "Move up", () => Move(-1)));
        buttons.Children.Add(IconButton("\uE70D", "Move down", () => Move(1)));
        var examples = new MenuFlyout();
        AddExample(examples, "Move files to a library folder", new PostProcessStep
        {
            Name = "Move to library", Executable = "powershell.exe",
            Arguments = "-NoProfile -Command \"Move-Item -LiteralPath $env:BROMELIA_OUTPUT_DIR -Destination 'D:\\Library'\"",
            FailJobOnError = true,
        });
        AddExample(examples, "Encode with HandBrakeCLI", new PostProcessStep
        {
            Name = "Encode with HandBrake", Executable = @"C:\Program Files\HandBrake\HandBrakeCLI.exe",
            Arguments = "-i {file} -o \"{outputDir}\\{stem}.mp4\" --preset \"Fast 1080p30\"", PerFile = true,
        });
        AddExample(examples, "Append to a log file", new PostProcessStep
        {
            Name = "Append to rip log", Executable = "cmd.exe", RunOn = RunCondition.Always,
            Arguments = "/c \"echo %DATE% %TIME% %BROMELIA_STATUS% %BROMELIA_DISC_NAME% %BROMELIA_OUTPUT_DIR% >> %USERPROFILE%\\rips.log\"",
        });
        AddExample(examples, "Run a PowerShell script", new PostProcessStep
        {
            Name = "PowerShell script", Executable = @"C:\Scripts\after-rip.ps1", Arguments = "{outputDir} {files}",
        });
        buttons.Children.Add(new DropDownButton { Content = "Examples", Flyout = examples });

        var left = new Grid { RowSpacing = 8, Width = 260 };
        left.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        left.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        left.Children.Add(_list);
        Grid.SetRow(buttons, 1);
        left.Children.Add(buttons);

        var root = new Grid { ColumnSpacing = 16 };
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        root.Children.Add(left);
        Grid.SetColumn(_editorHost, 1);
        root.Children.Add(_editorHost);
        Content = root;

        _list.SelectionChanged += (_, _) => ShowEditor();
        RebuildList(0);
    }

    static Button IconButton(string glyph, string tip, Action onClick)
    {
        var b = new Button { Content = new FontIcon { Glyph = glyph, FontSize = 14 } };
        ToolTipService.SetToolTip(b, tip);
        b.Click += (_, _) => onClick();
        return b;
    }

    void AddExample(MenuFlyout menu, string text, PostProcessStep template)
    {
        var item = new MenuFlyoutItem { Text = text };
        item.Click += (_, _) =>
        {
            var copy = System.Text.Json.JsonSerializer.Deserialize<PostProcessStep>(ConfigJson.Serialize(template), ConfigJson.Options)!;
            copy.Id = Guid.NewGuid();
            Add(copy);
        };
        menu.Items.Add(item);
    }

    void Add(PostProcessStep s)
    {
        _steps.Add(s);
        _changed();
        RebuildList(_steps.Count - 1);
    }

    void Remove()
    {
        int i = _list.SelectedIndex;
        if (i < 0 || i >= _steps.Count) return;
        _steps.RemoveAt(i);
        _changed();
        RebuildList(Math.Min(i, _steps.Count - 1));
    }

    void Move(int offset)
    {
        int i = _list.SelectedIndex, k = i + offset;
        if (i < 0 || k < 0 || k >= _steps.Count) return;
        (_steps[i], _steps[k]) = (_steps[k], _steps[i]);
        _changed();
        RebuildList(k);
    }

    void RebuildList(int select)
    {
        _list.Items.Clear();
        foreach (var s in _steps)
        {
            var sp = new StackPanel { Padding = new Thickness(0, 4, 0, 4), Opacity = s.Enabled ? 1 : 0.6 };
            sp.Children.Add(new TextBlock { Text = s.Name });
            sp.Children.Add(Form.Help(Summary(s)));
            _list.Items.Add(sp);
        }
        if (select >= 0 && select < _steps.Count) _list.SelectedIndex = select;
        else ShowEditor();
    }

    static string Summary(PostProcessStep s)
    {
        var parts = new List<string> { s.RunOn.Label() + (s.PerFile ? " · per file" : "") };
        if (s.MatchName.Trim().Length > 0) parts.Add($"“{s.MatchName}”");
        if (s.MatchFormats.Count > 0) parts.Add(string.Join(", ", s.MatchFormats));
        return string.Join(" · ", parts);
    }

    void UpdateSummary(int i)
    {
        if (i >= 0 && i < _list.Items.Count && _list.Items[i] is StackPanel sp && sp.Children.Count > 1 && sp.Children[1] is TextBlock tb) tb.Text = Summary(_steps[i]);
    }

    void ShowEditor()
    {
        int i = _list.SelectedIndex;
        if (i < 0 || i >= _steps.Count)
        {
            _editorHost.Content = Form.Help(_emptyText);
            return;
        }
        var s = _steps[i];
        var f = new Form();
        f.Changed += () =>
        {
            _changed();
            if (_list.Items[i] is StackPanel sp && sp.Children[0] is TextBlock tb) tb.Text = s.Name;
            UpdateSummary(i);
        };
        f.Section("Step");
        f.Text("Name", () => s.Name, v => s.Name = v);
        f.Toggle("Enabled", () => s.Enabled, v => s.Enabled = v);
        f.Choice("Run", Enum.GetValues<RunCondition>().Select(r => (r, r.Label())), () => s.RunOn, v => s.RunOn = v);
        f.Toggle("Run once for every produced file", () => s.PerFile, v => s.PerFile = v);
        f.Section("Applies to", "Leave both empty to run for every disc. The name is the movie or show name used for file names; the disc label (e.g. ONE_PIECE_S2_P7_D2) lets a step target one specific disc. No format ticked = all formats; the e codes are backups that are not decrypted.");
        var nameError = Form.Help(PluginMatcher.Validate(s.MatchName) ?? "");
        var nameBox = f.Text("Movie / show name or disc label matches", () => s.MatchName, v => s.MatchName = v, "Any — regular expression, e.g. ^One Piece$", width: 320);
        nameBox.TextChanged += (_, _) => nameError.Text = PluginMatcher.Validate(nameBox.Text) ?? "";
        f.Add(nameError);
        var formats = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        foreach (var code in DiscFormatExtensions.AllCodes.Where(c => !c.StartsWith("HDDVD", StringComparison.Ordinal)))
        {
            var cb = new CheckBox { Content = code, IsChecked = s.MatchFormats.Contains(code), MinWidth = 70 };
            cb.Click += (_, _) =>
            {
                s.MatchFormats.Remove(code);
                if (cb.IsChecked == true) s.MatchFormats.Add(code);
                _changed();
                UpdateSummary(i);
            };
            formats.Children.Add(cb);
        }
        f.Row("Formats", formats);
        f.Section("Command", "Arguments are split like a command line and then {tokens} are filled in, so values with spaces stay a single argument. A lone {files} expands to one argument per file. .ps1, .bat, .cmd and .py scripts are started with the matching interpreter.");
        f.PathPicker("Program or script", () => s.Executable, v => s.Executable = v, folder: false, placeholder: @"C:\Scripts\after-rip.ps1");
        f.Text("Interpreter", () => s.Interpreter, v => s.Interpreter = v, "Optional, e.g. python.exe");
        f.Text("Arguments", () => s.Arguments, v => s.Arguments = v, "{outputDir}", width: 460, monospace: true);
        f.Text("Working folder", () => s.WorkingDirectory, v => s.WorkingDirectory = v, "Default: the job's output folder");
        f.Number("Time limit (0 = none)", () => s.TimeoutSeconds, v => s.TimeoutSeconds = v, 0, 86400, suffix: "seconds");
        f.Toggle("Mark the job as failed if this step fails", () => s.FailJobOnError, v => s.FailJobOnError = v);

        f.Section("Environment variables", "Always set: BROMELIA_JOB_ID, BROMELIA_STATUS, BROMELIA_MODE, BROMELIA_DRIVE_NAME, BROMELIA_DRIVE_ID, BROMELIA_DEVICE, BROMELIA_DISC_NAME, BROMELIA_DISC_TYPE, BROMELIA_OUTPUT_DIR, BROMELIA_FILES (newline separated), BROMELIA_FILE_COUNT, BROMELIA_FILE (per-file steps), BROMELIA_MANIFEST (JSON), BROMELIA_LOG, BROMELIA_SOURCE, BROMELIA_ERROR, BROMELIA_NAME, BROMELIA_KIND (movie / tv), BROMELIA_FORMAT (DVD, BRe, 4K, …), BROMELIA_ENCRYPTED (1 / 0), BROMELIA_SEASON, BROMELIA_DISC_NUMBER, BROMELIA_DISC_SET, BROMELIA_CHECKSUMS (SHA256SUMS path).");
        var env = new StackPanel { Spacing = 6 };
        void RebuildEnv()
        {
            env.Children.Clear();
            foreach (var key in s.Environment.Keys.OrderBy(k => k).ToList())
            {
                var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
                row.Children.Add(new TextBlock { Text = key, Width = 200, VerticalAlignment = VerticalAlignment.Center });
                var val = new TextBox { Text = s.Environment[key], Width = 280 };
                var k = key;
                val.TextChanged += (_, _) => { s.Environment[k] = val.Text; _changed(); };
                row.Children.Add(val);
                var del = new Button { Content = new SymbolIcon(Symbol.Delete) };
                del.Click += (_, _) => { s.Environment.Remove(k); _changed(); RebuildEnv(); };
                row.Children.Add(del);
                env.Children.Add(row);
            }
            var add = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            var nk = new TextBox { PlaceholderText = "VARIABLE", Width = 200 };
            var nv = new TextBox { PlaceholderText = "value ({tokens} allowed)", Width = 280 };
            var btn = new Button { Content = new SymbolIcon(Symbol.Add) };
            btn.Click += (_, _) =>
            {
                if (nk.Text.Trim().Length == 0) return;
                s.Environment[nk.Text.Trim()] = nv.Text;
                _changed();
                RebuildEnv();
            };
            add.Children.Add(nk);
            add.Children.Add(nv);
            add.Children.Add(btn);
            env.Children.Add(add);
        }
        RebuildEnv();
        f.Add(env);

        f.Section("Tokens");
        foreach (var (token, help) in TemplateRenderer.ScriptTokens) f.Add(Form.Help($"{{{token}}}  —  {help}"));

        f.Section("Test");
        var output = new TextBox { IsReadOnly = true, AcceptsReturn = true, Height = 140, FontFamily = new FontFamily("Cascadia Mono, Consolas"), FontSize = 12 };
        var run = new Button { Content = "Run with sample values" };
        run.Click += async (_, _) =>
        {
            run.IsEnabled = false;
            output.Text = "";
            var tmp = Path.Combine(Path.GetTempPath(), "bromelia-test");
            Directory.CreateDirectory(tmp);
            var values = TemplateRenderer.DateValues();
            values["disc"] = "SAMPLE_DISC"; values["volume"] = "SAMPLE_DISC"; values["type"] = "bd"; values["drive"] = _driveName;
            values["job"] = "test0000"; values["outputDir"] = tmp; values["status"] = "success"; values["manifest"] = "";
            values["file"] = ""; values["files"] = ""; values["device"] = ""; values["checksums"] = "";
            var sample = new MediaIdentity("Sample Disc", MediaKind.Movie, DiscFormat.Bluray, false, new LabelInfo(), "");
            foreach (var kv in sample.TemplateValues("Rip")) values[kv.Key] = kv.Value;
            var envVars = new Dictionary<string, string>
            {
                ["BROMELIA_STATUS"] = "success", ["BROMELIA_DISC_NAME"] = "SAMPLE_DISC", ["BROMELIA_OUTPUT_DIR"] = tmp,
                ["BROMELIA_DRIVE_NAME"] = _driveName, ["BROMELIA_FILES"] = "", ["BROMELIA_FILE_COUNT"] = "0", ["BROMELIA_MODE"] = "mkv",
                ["BROMELIA_NAME"] = sample.Name, ["BROMELIA_KIND"] = "movie", ["BROMELIA_FORMAT"] = sample.FormatCode, ["BROMELIA_ENCRYPTED"] = "0",
            };
            var copy = System.Text.Json.JsonSerializer.Deserialize<PostProcessStep>(ConfigJson.Serialize(s), ConfigJson.Options)!;
            copy.Enabled = true;
            copy.RunOn = RunCondition.Always;
            copy.PerFile = false;
            var lines = new LineCollector();
            await PostProcessor.RunAsync(new[] { copy }, new PostProcessor.Context(JobState.Succeeded, values, tmp, Array.Empty<string>(), envVars),
                _ => { }, (text, _) => lines.Add(text));
            output.Text = string.Join(Environment.NewLine, lines.All);
            run.IsEnabled = true;
        };
        f.Add(run);
        f.Add(output);
        _editorHost.Content = f.Root;
    }
}
