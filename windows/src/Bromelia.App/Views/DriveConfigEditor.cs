using Bromelia.App.Controls;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Bromelia.Core.Logic;
using Bromelia.Core.Robot;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Bromelia.App.Views;

/// <summary>Builds the tabbed editor for a DriveConfig. Every change is written to the object and reported via onChanged.</summary>
public static class DriveConfigEditor
{
    public static Pivot Build(DriveConfig c, bool isDefaultTemplate, DiscInfo? preview, Action onChanged)
    {
        var pivot = new Pivot();
        pivot.Items.Add(Tab("General", General(c, isDefaultTemplate, onChanged)));
        pivot.Items.Add(Tab("Ripping", Ripping(c, preview, onChanged)));
        pivot.Items.Add(Tab("Output", Output(c, onChanged)));
        pivot.Items.Add(Tab("MakeMKV settings", SettingsCatalogPanel.Build(c.Settings, App.State.Config.GlobalSettings, driveMode: true, onChanged)));
        pivot.Items.Add(Tab("Profile", Profile(c.Profile, onChanged)));
        pivot.Items.Add(new PivotItem { Header = "Post-processing", Content = new PostProcessEditor(c.PostProcess, c.Name, onChanged) });
        return pivot;
    }

    static PivotItem Tab(string header, UIElement content) =>
        new() { Header = header, Content = new ScrollViewer { Content = content, Padding = new Thickness(0, 0, 16, 24) } };

    static UIElement General(DriveConfig c, bool isDefault, Action changed)
    {
        var f = new Form();
        f.Changed += changed;
        f.Section("Drive");
        f.Text("Name", () => c.Name, v => c.Name = v);
        if (!isDefault) f.Toggle("Enabled", () => c.Enabled, v => c.Enabled = v);
        if (isDefault)
            f.Note("The default configuration is used for drives that have not been set up and for disc images and folders. New drive configurations start as a copy of it.");
        else
        {
            f.Section("Drive identification", "The drive name reported by MakeMKV usually includes the serial number, so a configuration follows the drive even when drive letters change.");
            var name = f.Text("Drive name", () => c.Match.DriveName, v => c.Match.DriveName = v, "e.g. BD-RE HL-DT-ST BD-RE WH16NS60 1.02 KL…", width: 420);
            var dev = f.Text("Device", () => c.Match.DevicePath, v => c.Match.DevicePath = v, "E: (used when the drive name is empty)");
            var drives = App.State.ScannedDrives.Where(d => d.IsPresent).ToList();
            if (drives.Count > 0)
            {
                var pick = new ComboBox { PlaceholderText = "Use a connected drive", Width = 420 };
                foreach (var d in drives) pick.Items.Add($"{d.DriveName} — {d.DevicePath}");
                pick.SelectionChanged += (_, _) =>
                {
                    if (pick.SelectedIndex < 0) return;
                    name.Text = drives[pick.SelectedIndex].DriveName;
                    dev.Text = drives[pick.SelectedIndex].DevicePath;
                };
                f.Row("Connected drives", pick);
            }
        }
        f.Section("Automation");
        f.Toggle("Rip automatically when a disc is inserted", () => c.Automation.AutoRipOnInsert, v => c.Automation.AutoRipOnInsert = v);
        f.Number("Start automatic rips after", () => c.Automation.AutoRipDelaySeconds, v => c.Automation.AutoRipDelaySeconds = v, 0, 3600, suffix: "seconds");
        f.Toggle("Eject the disc when the job succeeds", () => c.Automation.EjectWhenDone, v => c.Automation.EjectWhenDone = v);
        f.Toggle("Eject the disc when the job fails", () => c.Automation.EjectOnFailure, v => c.Automation.EjectOnFailure = v);
        f.Toggle("Show a notification when the job finishes", () => c.Automation.Notify, v => c.Automation.Notify = v);
        f.Toggle("Play a sound", () => c.Automation.PlaySound, v => c.Automation.PlaySound = v);
        f.Number("Before an automatic rip, wait for the disc to be mounted for up to", () => c.Automation.WaitForMountSeconds, v => c.Automation.WaitForMountSeconds = v, 0, 600, suffix: "seconds");
        f.Choice("A disc archived before", new[]
            {
                (AlreadyArchived.Skip, "Skip it (eject when done)"), (AlreadyArchived.Ask, "Stop and ask (leave it in the drive)"),
                (AlreadyArchived.RipAgain, "Rip it again"),
            },
            () => c.Automation.AlreadyArchived, v => c.Automation.AlreadyArchived = v,
            "Automatic rips. A disc is recognised by its fingerprint, in the history or a bromelia.json under the output folder. Manual rips ask first.");
        f.Section("Other discs", "For discs without a DVD or Blu-ray structure, when ripped automatically or with Rip. Audio CDs are ripped by cyanrip or abcde (both look up the album in MusicBrainz and write FLAC). Data discs are copied sector by sector to an ISO image.");
        f.Toggle("Rip audio CDs", () => c.Other.RipAudioCDs, v => c.Other.RipAudioCDs = v);
        f.Text("Audio CD command", () => c.Other.AudioCommand, v => c.Other.AudioCommand = v, "Empty: cyanrip -d {device} -o flac, else abcde");
        f.Toggle("Save data discs as ISO images", () => c.Other.ImageDataDiscs, v => c.Other.ImageDataDiscs = v);
        return f.Root;
    }

    static string ModeHelp(RipMode m) => m switch
    {
        RipMode.Mkv => "Titles chosen by the rules below are saved as MKV files. Track selection follows the profile.",
        RipMode.Backup => "The whole disc is copied as is, still encrypted.",
        RipMode.BackupDecrypted => "The whole disc is copied and video files are decrypted, keeping menus and extras.",
        RipMode.BackupThenMkv => "A decrypted backup is made first (fast sequential read), then MKV files are made from the backup.",
        _ => "Only reads the disc and stores disc-info.json.",
    };

    static UIElement Ripping(DriveConfig c, DiscInfo? preview, Action changed)
    {
        var f = new Form();
        var previewPanel = new StackPanel { Spacing = 4 };
        void RefreshPreview()
        {
            previewPanel.Children.Clear();
            if (preview == null) return;
            var res = TitleSelector.Evaluate(preview.Titles, c.Rip.TitleSelection);
            if (res.Error != null) previewPanel.Children.Add(new TextBlock { Text = res.Error, Foreground = new SolidColorBrush(Microsoft.UI.Colors.IndianRed) });
            foreach (var d in res.Decisions)
            {
                var t = preview.Title(d.TitleIndex);
                previewPanel.Children.Add(new TextBlock
                {
                    Text = $"{(d.Selected ? "✔" : "○")}  Title {d.TitleIndex}   {t?.DurationText}   {t?.ChapterCount} ch   {t?.SourceFileName}   — {d.Reason}",
                    Opacity = d.Selected ? 1 : 0.65,
                });
            }
        }
        f.Changed += () => { RefreshPreview(); changed(); };

        f.Section("What to do with a disc");
        var help = Form.Help(ModeHelp(c.Rip.Mode));
        f.Choice("Mode", EnumLabels.VideoModes.Select(m => (m, m.Label())), () => c.Rip.Mode, v => { c.Rip.Mode = v; help.Text = ModeHelp(v); });
        f.Add(help);
        foreach (var (key, label) in new[] { ("dvd", "DVDs"), ("bluray", "Blu-rays"), ("uhd", "4K UHD discs") })
        {
            var options = new List<(RipMode?, string)> { (null, "Same as above") };
            options.AddRange(EnumLabels.VideoModes.Select(m => ((RipMode?)m, m.Label())));
            f.Choice(label, options, () => c.Rip.FormatModes.TryGetValue(key, out var fm) ? fm : null,
                v => { if (v is { } mode) c.Rip.FormatModes[key] = mode; else c.Rip.FormatModes.Remove(key); });
        }
        f.Note("Automatic and quick rips can use a different mode for each kind of disc, for example a backup of every Blu-ray and MKV files of DVDs.");
        f.Choice("Backup format", new[] { (BackupFormat.Folder, "Folder (BDMV / VIDEO_TS)"), (BackupFormat.Iso, "ISO image") }, () => c.Rip.BackupFormat, v => c.Rip.BackupFormat = v);
        f.Toggle("Keep the backup after the MKV files are made (backup + MKV mode)", () => c.Rip.KeepBackupAfterMkv, v => c.Rip.KeepBackupAfterMkv = v);

        f.Section("TV episodes", "DVDs often store several episodes in one title. Bromelia reads the disc's menu navigation to find where each episode starts and splits the MKV with mkvmerge, without re-encoding. Episode numbers are read from the menu screens with ffmpeg and tesseract when installed; otherwise enter the first episode number on the disc page, or episodes are numbered from 1.");
        f.Toggle("Split “play all” titles of TV shows into episodes", () => c.Episodes.SplitPlayAll, v => c.Episodes.SplitPlayAll = v);
        f.Toggle("Keep the unsplit title as well", () => c.Episodes.KeepPlayAll, v => c.Episodes.KeepPlayAll = v);
        f.Toggle("Read episode numbers from the disc menus", () => c.Episodes.ReadMenuNumbers, v => c.Episodes.ReadMenuNumbers = v);

        var r = c.Rip.TitleSelection;
        f.Section("Titles to rip");
        f.Choice("Choose", Enum.GetValues<TitleStrategy>().Select(s => (s, s.Label())), () => r.Strategy, v => r.Strategy = v);
        f.Number("Number of longest titles", () => r.LongestCount, v => r.LongestCount = v, 1, 99);
        f.Text("Index pattern", () => r.IndexPattern, v => r.IndexPattern = v, "0,2-4,7-  ·  last  ·  all");
        f.Choice("Index numbers refer to", new[] { (IndexBase.Makemkv, "MakeMKV title number (0-based)"), (IndexBase.Source, "Source title ID (playlist / VTS)") },
            () => r.IndexBase, v => r.IndexBase = v);

        f.Section("Filters", "0 means no limit. Patterns are regular expressions matched against the title name, comment, source file (e.g. 00800.mpls), output file name, segment map and “#<source id>”.");
        Duration(f, "Minimum duration", () => r.MinDurationSeconds, v => r.MinDurationSeconds = v);
        Duration(f, "Maximum duration", () => r.MaxDurationSeconds, v => r.MaxDurationSeconds = v);
        f.Number("Minimum chapters", () => r.MinChapters, v => r.MinChapters = v);
        f.Number("Maximum chapters", () => r.MaxChapters, v => r.MaxChapters = v);
        f.Number("Minimum size", () => r.MinSizeMB, v => r.MinSizeMB = v, suffix: "MB");
        f.Number("Maximum size", () => r.MaxSizeMB, v => r.MaxSizeMB = v, suffix: "MB");
        f.Text("Include if matching", () => r.IncludePattern, v => r.IncludePattern = v, @"e.g. ^(00800|00801)\.mpls", monospace: true);
        f.Text("Exclude if matching", () => r.ExcludePattern, v => r.ExcludePattern = v, "Regular expression", monospace: true);
        f.Toggle("Skip duplicate titles (same segments and length)", () => r.SkipDuplicates, v => r.SkipDuplicates = v);
        f.Toggle("Skip alternate angles", () => r.SkipAlternateAngles, v => r.SkipAlternateAngles = v);
        f.Number("At most (0 = no limit)", () => r.MaxTitles, v => r.MaxTitles = v, suffix: "titles");

        if (preview != null)
        {
            f.Section($"Preview on “{preview.Name}”");
            f.Add(previewPanel);
            RefreshPreview();
        }

        f.Section("makemkvcon options");
        f.OptionalNumber("Minimum title length (--minlength, seconds)", () => c.Rip.MinLengthSeconds, v => c.Rip.MinLengthSeconds = v, "Setting");
        f.OptionalNumber("Read cache (--cache, MB)", () => c.Rip.CacheMB, v => c.Rip.CacheMB = v);
        f.Choice("Direct disc access (--directio)", new (bool?, string)[] { (null, "MakeMKV default"), (true, "On"), (false, "Off") },
            () => c.Rip.DirectIO, v => c.Rip.DirectIO = v);
        f.Text("Extra switches", () => c.Rip.ExtraArguments, v => c.Rip.ExtraArguments = v, "Advanced: additional makemkvcon switches", monospace: true);
        f.Toggle("Save disc information (disc-info.json) next to the files", () => c.Rip.WriteDiscInfoJson, v => c.Rip.WriteDiscInfoJson = v);
        return f.Root;
    }

    static void Duration(Form f, string label, Func<int> get, Action<int> set)
    {
        var tb = f.Text(label, () => get() == 0 ? "" : TitleInfo.FormatDuration(get()), v => set(TitleInfo.ParseDuration(v.Trim())), "h:mm:ss", width: 140);
        ToolTipService.SetToolTip(tb, "h:mm:ss, m:ss or seconds; empty = no limit");
    }

    static UIElement Output(DriveConfig c, Action changed)
    {
        var f = new Form();
        var preview = Form.Help("");
        var filePreview = Form.Help("");
        var tvPreview = Form.Help("");
        static Dictionary<string, string> Sample(DriveConfig c, bool tv)
        {
            var v = TemplateRenderer.DateValues();
            var label = tv ? "SHOW_NAME_S2_D3" : "MOVIE_TITLE";
            var id = new MediaIdentity(tv ? "Show Name" : "Movie Title", tv ? MediaKind.Tv : MediaKind.Movie, tv ? DiscFormat.Dvd : DiscFormat.Bluray,
                false, LabelParser.Parse(label), "");
            foreach (var kv in id.TemplateValues("Rip")) v[kv.Key] = kv.Value;
            v["disc"] = label; v["volume"] = label; v["type"] = tv ? "dvd" : "bd"; v["drive"] = c.Name; v["job"] = "1a2b3c4d";
            v["title"] = id.Name; v["index"] = "3"; v["n"] = "1"; v["source"] = tv ? "4" : "800"; v["duration"] = tv ? "0-23-40" : "1-58-02";
            v["chapters"] = "24"; v["original"] = label + "_t03"; v["comment"] = "";
            v["track"] = tv ? "Title 4" : "Playlist 00800";
            if (tv) { v["episode"] = "Episode 07"; v["episodeNumber"] = "7"; }
            return v;
        }
        void Refresh()
        {
            var v = Sample(c, false);
            var root = c.Output.RootOverride.Length > 0 ? c.Output.RootOverride : App.State.Config.OutputRoot;
            preview.Text = "Preview: " + Path.Combine(Paths.ExpandUser(root), TemplateRenderer.RenderPath(c.Output.FolderTemplate, v));
            filePreview.Text = c.Output.FileNameTemplate.Length == 0 ? "MakeMKV's file names are kept." : "Movie: " + TemplateRenderer.RenderPath(c.Output.FileNameTemplate, v) + ".mkv";
            tvPreview.Text = c.Output.FileNameTemplate.Length == 0 ? "" : "TV: " + TemplateRenderer.RenderPath(c.Output.FileNameTemplate, Sample(c, true)) + ".mkv";
        }
        f.Changed += () => { Refresh(); changed(); };
        f.Section("Location");
        f.Choice("Layout", new[] { (LibraryLayout.Templates, "Folder and file name templates"), (LibraryLayout.MediaServer, "Plex / Jellyfin / Emby library") },
            () => c.Output.Layout, v => c.Output.Layout = v,
            "Plex / Jellyfin / Emby: Movies\\Name (Year)\\Name (Year).mkv, TV Shows\\Name (Year)\\Season 02\\Name (Year) - S02E05.mkv; other titles go to Other and backups to Backup (hidden from the server). Turn on the online lookup for years; the templates below aren't used.");
        f.PathPicker("Output folder", () => c.Output.RootOverride, v => c.Output.RootOverride = v, folder: true, placeholder: $"Global default: {App.State.Config.OutputRoot}");
        var folderBox = f.Text("Folder name", () => c.Output.FolderTemplate, v => c.Output.FolderTemplate = v, OutputConfig.DefaultFolderTemplate);
        f.Add(preview);
        f.Choice("If the folder already exists", Enum.GetValues<ConflictPolicy>().Select(p => (p, p.Label())), () => c.Output.ConflictPolicy, v => c.Output.ConflictPolicy = v);
        f.Section("File names");
        var fileBox = f.Text("Name files and backups", () => c.Output.FileNameTemplate, v => c.Output.FileNameTemplate = v, "Empty keeps MakeMKV's names", width: 460);
        f.Add(filePreview);
        f.Add(tvPreview);
        var standard = new Button { Content = "Use the standard naming" };
        standard.Click += (_, _) =>
        {
            fileBox.Text = OutputConfig.DefaultFileNameTemplate;
            folderBox.Text = OutputConfig.DefaultFolderTemplate;
        };
        f.Add(standard);
        f.Note("Standard naming: {name} - {episode} - {discLabel} - {rip} - {track} - {format}, leaving out parts that don't apply. Format codes: DVD, BR (Blu-ray), 4K (Ultra HD Blu-ray); DVDe, BRe, 4Ke for backups that are not decrypted.");
        f.Text("Backup subfolder (backup + MKV mode)", () => c.Output.BackupSubfolder, v => c.Output.BackupSubfolder = v);
        f.Section("Tokens");
        foreach (var (token, help) in TemplateRenderer.FileTokens) f.Add(Form.Help($"{{{token}}}  —  {help}"));
        f.Add(Form.Help("{n:3}  —  zero-pad a number to 3 digits;   {token?text}  —  insert text only when token is not empty"));
        f.Section("Archiving", "Checksums of every file (including backup folders) are saved in the output folder in the standard format; check a copy later with “sha256sum -c SHA256SUMS” (Git Bash / WSL) or any SHA256SUMS tool. The archive record describes the disc, titles, episodes and files with their sizes and hashes. Checking compares each MKV's length and tracks with the disc listing (needs mkvmerge) and each backup's structure. Files only reach the output folder when the job succeeded; otherwise they are kept in a folder marked [INCOMPLETE] or [READ ERRORS].");
        f.Toggle("Write SHA-256 checksums (SHA256SUMS)", () => c.Archive.Checksums, v => c.Archive.Checksums = v);
        f.Toggle("Write an archive record (bromelia.json) and the job log", () => c.Archive.ArchiveRecord, v => c.Archive.ArchiveRecord = v);
        f.Toggle("Check every rip against the disc listing", () => c.Archive.VerifyRips, v => c.Archive.VerifyRips = v);
        Refresh();
        return f.Root;
    }

    static UIElement Profile(ProfileConfig p, Action changed)
    {
        var f = new Form();
        var xml = new TextBox { IsReadOnly = true, AcceptsReturn = true, Height = 240, FontFamily = new FontFamily("Cascadia Mono, Consolas"), FontSize = 12, TextWrapping = TextWrapping.NoWrap };
        void Refresh() => xml.Text = ProfileBuilder.Build(p.Generated);
        f.Changed += () => { Refresh(); changed(); };
        f.Section("Profile", "A MakeMKV profile controls which tracks are selected by default, MKV flags and audio conversion. It is passed to makemkvcon with --profile.");
        f.Choice("Profile", Enum.GetValues<ProfileMode>().Select(m => (m, m.Label())), () => p.Mode, v => p.Mode = v);
        f.PathPicker("Custom profile file", () => p.CustomPath, v => p.CustomPath = v, folder: false, placeholder: @"C:\path\profile.mmcp.xml");
        f.Section("Bromelia profile — track selection");
        f.Text("Profile name", () => p.Generated.Name, v => p.Generated.Name = v);
        SelectionRuleRow(f, "Selection rule", () => p.Generated.SelectionRule, v => p.Generated.SelectionRule = v);
        f.Section("MKV flags");
        f.Toggle("Mark the first audio track as default", () => p.Generated.SetFirstAudioTrackAsDefault, v => p.Generated.SetFirstAudioTrackAsDefault = v);
        f.Toggle("Mark the first subtitle track as default", () => p.Generated.SetFirstSubtitleTrackAsDefault, v => p.Generated.SetFirstSubtitleTrackAsDefault = v);
        f.Toggle("Mark the first forced subtitle track as default", () => p.Generated.SetFirstForcedSubtitleTrackAsDefault, v => p.Generated.SetFirstForcedSubtitleTrackAsDefault = v);
        f.Toggle("Ignore the disc's forced subtitle flag", () => p.Generated.IgnoreForcedSubtitlesFlag, v => p.Generated.IgnoreForcedSubtitlesFlag = v);
        f.Toggle("Use ISO 639-2/T language codes (deu instead of ger)", () => p.Generated.UseIso639Type2T, v => p.Generated.UseIso639Type2T = v);
        f.Toggle("Insert chapter 0 when missing", () => p.Generated.InsertFirstChapter00IfMissing, v => p.Generated.InsertFirstChapter00IfMissing = v);
        f.Section("Audio conversion");
        var outputs = Enum.GetValues<LpcmOutput>().Select(o => (o, o.Label())).ToList();
        f.Choice("Stereo / mono LPCM", outputs, () => p.Generated.LpcmStereo, v => p.Generated.LpcmStereo = v);
        f.Choice("Multichannel LPCM", outputs, () => p.Generated.LpcmMultichannel, v => p.Generated.LpcmMultichannel = v);
        f.Section("Generated profile XML");
        f.Add(xml);
        var export = new Button { Content = "Export profile…" };
        export.Click += async (_, _) =>
        {
            if (await Pickers.SaveFileAsync(p.Generated.Name + ".mmcp", "MakeMKV profile", ".xml") is { } path)
                await File.WriteAllTextAsync(path, ProfileBuilder.Build(p.Generated));
        };
        f.Add(export);
        Refresh();
        return f.Root;
    }

    /// <summary>Selection rule text box with presets and a syntax check.</summary>
    public static void SelectionRuleRow(Form f, string label, Func<string> get, Action<string> set)
    {
        var tb = f.Text(label, get, set, "Empty = MakeMKV default", width: 460, monospace: true);
        var presets = new MenuFlyout();
        foreach (var p in App.State.Catalog.SelectionPresets)
        {
            var item = new MenuFlyoutItem { Text = p.Name };
            var rule = p.Rule;
            item.Click += (_, _) => tb.Text = rule;
            presets.Items.Add(item);
        }
        var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        panel.Children.Add(new DropDownButton { Content = "Presets", Flyout = presets });
        var lint = Form.Help("");
        panel.Children.Add(lint);
        void Check() => lint.Text = SelectionLint(tb.Text) ?? "";
        tb.TextChanged += (_, _) => Check();
        Check();
        f.Add(panel);
        var tokens = string.Join(", ", App.State.Catalog.SelectionTokens.Select(t => t.GetValueOrDefault("token", "")));
        f.Note("Rules are comma separated ‘action:condition’ items applied in order: +sel / -sel select or deselect, +N / -N / =N change a track's weight. " +
               "Conditions combine tokens with | (or), & (and), ! (not) and parentheses. Tokens: " + tokens + ".");
    }

    public static string? SelectionLint(string rule)
    {
        var r = rule.Trim();
        if (r.Length == 0) return null;
        int depth = 0;
        foreach (var ch in r)
        {
            if (ch == '(') depth++;
            if (ch == ')' && --depth < 0) return "Unbalanced parentheses";
        }
        if (depth != 0) return "Unbalanced parentheses";
        foreach (var item in r.Split(','))
        {
            var s = item.Trim();
            int colon = s.IndexOf(':');
            if (colon < 0) return $"“{s}” is missing ':' (expected e.g. +sel:all)";
            var action = s[..colon];
            if (action is not ("+sel" or "-sel") && (action.Length == 0 || "+-=".IndexOf(action[0]) < 0)) return $"“{action}” is not a valid action";
        }
        return null;
    }
}

/// <summary>Catalog-driven editor for MakeMKV settings (global or per-drive overrides).</summary>
public static class SettingsCatalogPanel
{
    public static UIElement Build(Dictionary<string, string> settings, Dictionary<string, string> global, bool driveMode, Action changed)
    {
        var f = new Form();
        f.Changed += changed;
        var catalog = App.State.Catalog;
        f.Section("MakeMKV settings");
        f.Note(driveMode
            ? "Checked settings override the global MakeMKV settings for this drive only. Values are applied only while this drive's makemkvcon starts, so drives never share them."
            : "These settings apply to every drive unless a drive overrides them. Bromelia applies them for each job; MakeMKV's own preferences are restored afterwards.");
        foreach (var section in catalog.Sections)
        {
            var items = section.Settings.Where(s => s.AppliesTo("windows")).ToList();
            if (items.Count == 0) continue;
            f.Section(section.Title);
            foreach (var s in items) AddSetting(f, s, settings, global, driveMode, changed);
        }

        f.Section("Additional settings", "Any other MakeMKV setting key, written as is.");
        var known = catalog.AllSettings.Select(s => s.Key).ToHashSet();
        var extra = new StackPanel { Spacing = 6 };
        void Rebuild()
        {
            extra.Children.Clear();
            foreach (var key in settings.Keys.Where(k => !known.Contains(k) && k != "app_Key").OrderBy(k => k).ToList())
            {
                var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
                row.Children.Add(new TextBlock { Text = key, Width = 220, VerticalAlignment = VerticalAlignment.Center });
                var val = new TextBox { Text = settings[key], Width = 300 };
                var k = key;
                val.TextChanged += (_, _) => { settings[k] = val.Text; changed(); };
                row.Children.Add(val);
                var del = new Button { Content = new SymbolIcon(Symbol.Delete) };
                del.Click += (_, _) => { settings.Remove(k); changed(); Rebuild(); };
                row.Children.Add(del);
                extra.Children.Add(row);
            }
            var add = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            var nk = new TextBox { PlaceholderText = "setting_Key", Width = 220 };
            var nv = new TextBox { PlaceholderText = "value", Width = 300 };
            var btn = new Button { Content = new SymbolIcon(Symbol.Add) };
            btn.Click += (_, _) =>
            {
                var key = nk.Text.Trim();
                if (key.Length == 0) return;
                settings[key] = nv.Text;
                changed();
                Rebuild();
            };
            add.Children.Add(nk);
            add.Children.Add(nv);
            add.Children.Add(btn);
            extra.Children.Add(add);
        }
        Rebuild();
        f.Add(extra);
        return f.Root;
    }

    static void AddSetting(Form f, SettingsCatalog.Setting s, Dictionary<string, string> settings, Dictionary<string, string> global, bool driveMode, Action changed)
    {
        string Inherited()
        {
            var v = global.GetValueOrDefault(s.Key) ?? "";
            if (v.Length == 0) return "MakeMKV default";
            if (s.Type == "bool") return v == "1" ? "On" : "Off";
            return s.Choices?.FirstOrDefault(c => c.Value == v)?.Label ?? v;
        }
        string Current() => settings.TryGetValue(s.Key, out var v) ? v : driveMode ? global.GetValueOrDefault(s.Key) ?? s.Default ?? "" : "";
        void Set(string v) { settings[s.Key] = v; changed(); }

        FrameworkElement control;
        switch (s.Type)
        {
            case "bool":
                var ts = new ToggleSwitch { IsOn = Current() is "1" || (Current().Length == 0 && s.Default == "1"), OnContent = "", OffContent = "", MinWidth = 0 };
                ts.Toggled += (_, _) => Set(ts.IsOn ? "1" : "0");
                control = ts;
                break;
            case "choice":
                var cb = new ComboBox { Width = 220 };
                var choices = s.Choices ?? new();
                foreach (var ch in choices) cb.Items.Add(ch.Label);
                cb.SelectedIndex = Math.Max(0, choices.FindIndex(ch => ch.Value == Current()));
                cb.SelectionChanged += (_, _) => { if (cb.SelectedIndex >= 0) Set(choices[cb.SelectedIndex].Value); };
                control = cb;
                break;
            case "file":
            case "directory":
                var tb = new TextBox { Text = Current(), Width = 300 };
                tb.TextChanged += (_, _) => { if (tb.Text != Current()) Set(tb.Text); };
                var browse = new Button { Content = "Browse…" };
                var folder = s.Type == "directory";
                browse.Click += async (_, _) =>
                {
                    var p = folder ? await Pickers.PickFolderAsync() : await Pickers.PickFileAsync();
                    if (p != null) tb.Text = p;
                };
                var sp = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
                sp.Children.Add(tb);
                sp.Children.Add(browse);
                control = sp;
                break;
            default:
                var text = new TextBox
                {
                    Text = Current(), Width = s.Type == "selection" ? 420 : 220,
                    PlaceholderText = s.Type == "language" ? "e.g. eng" : string.IsNullOrEmpty(s.Default) ? "Default" : s.Default,
                };
                if (s.Type == "selection") text.FontFamily = new FontFamily("Cascadia Mono, Consolas");
                text.TextChanged += (_, _) => { if (text.Text != Current()) Set(text.Text); };
                control = text;
                break;
        }

        var label = s.Label + (s.IsAdvanced ? " (advanced)" : "");
        if (driveMode)
            f.OverrideRow(label, control, settings.ContainsKey(s.Key), on =>
            {
                if (on) settings[s.Key] = global.GetValueOrDefault(s.Key) ?? s.Default ?? "";
                else settings.Remove(s.Key);
            }, s.Help, Inherited());
        else
            f.Row(label, control, s.Help);
    }
}
