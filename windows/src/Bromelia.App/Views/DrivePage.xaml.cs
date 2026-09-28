using System.Collections.Specialized;
using System.ComponentModel;
using Bromelia.App.Controls;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Bromelia.Core.Logic;
using Bromelia.Core.Robot;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;

namespace Bromelia.App.Views;

/// <summary>A drive (or an opened ISO / folder): header, active job, title browser and rip actions.</summary>
public sealed partial class DrivePage : Page
{
    AppState State => App.State;
    string _tag = "";
    DriveItem? _item;
    DiscSession? _session;
    DiscInfo? _shownInfo;
    bool _syncing;
    readonly Dictionary<int, CheckBox> _titleChecks = new();
    readonly Dictionary<int, StackPanel> _trackPanels = new();

    public DrivePage()
    {
        InitializeComponent();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        _tag = e.Parameter as string ?? "";
        State.DrivesChanged += OnDrivesChanged;
        State.Jobs.CollectionChanged += OnJobsChanged;
        Resolve();
        RefreshAll();
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        State.DrivesChanged -= OnDrivesChanged;
        State.Jobs.CollectionChanged -= OnJobsChanged;
        Attach(null);
    }

    bool IsSource => _tag.StartsWith("source:", StringComparison.Ordinal);

    void Resolve()
    {
        if (IsSource)
        {
            _item = null;
            Attach(State.Sessions.GetValueOrDefault(_tag["source:".Length..]));
        }
        else
        {
            _item = State.DriveItemById(_tag["drive:".Length..]);
            Attach(_item != null ? State.SessionFor(_item) : null);
        }
    }

    void Attach(DiscSession? s)
    {
        if (ReferenceEquals(s, _session)) return;
        if (_session != null)
        {
            _session.PropertyChanged -= OnSessionChanged;
            _session.SelectionChanged -= OnSelectionChanged;
        }
        _session = s;
        _shownInfo = null;
        if (_session != null)
        {
            _session.PropertyChanged += OnSessionChanged;
            _session.SelectionChanged += OnSelectionChanged;
        }
    }

    void OnDrivesChanged()
    {
        Resolve();
        RefreshHeader();
        RefreshDiscArea();
    }

    void OnJobsChanged(object? sender, NotifyCollectionChangedEventArgs e) => RefreshJob();
    void OnSessionChanged(object? sender, PropertyChangedEventArgs e) => RefreshDiscArea();
    void OnSelectionChanged() => SyncChecks();

    DriveConfig CurrentConfig => _session != null ? State.ConfigForSession(_session) : _item?.Config ?? State.Config.DefaultDrive;
    string Lane => _session?.Id ?? _item?.LaneKey ?? "";
    RipJob? ActiveJob => State.ActiveJob(Lane);
    bool DriveBusy => ActiveJob?.State == JobState.Running;

    void RefreshAll()
    {
        RefreshHeader();
        RefreshJob();
        RefreshDiscArea();
    }

    // --- header -------------------------------------------------------------------------------

    void RefreshHeader()
    {
        var cfg = CurrentConfig;
        Chips.Children.Clear();
        if (IsSource)
        {
            var s = _session;
            TitleText.Text = s?.Info?.Name is { Length: > 0 } n ? n : s?.Source.DisplayName ?? "Source";
            SubtitleText.Text = s?.Source.InfoArgument ?? "";
            StatusText.Text = "";
            HeaderIcon.Glyph = s?.Source is DiscSource.Iso ? "" : "";
            OpenDiscButton.Content = "Reload";
            EjectButton.Visibility = Visibility.Collapsed;
            RipButton.Visibility = Visibility.Collapsed;
            CloseSourceButton.Visibility = Visibility.Visible;
            ConfigureButton.Visibility = Visibility.Collapsed;
            ConfigPicker.Visibility = Visibility.Visible;
            _syncing = true;
            ConfigPicker.Items.Clear();
            ConfigPicker.Items.Add(State.Config.DefaultDrive.Name);
            foreach (var d in State.Config.Drives) ConfigPicker.Items.Add(d.Name);
            var idx = s == null ? 0 : s.ConfigId == State.Config.DefaultDrive.Id ? 0 : State.Config.Drives.FindIndex(d => d.Id == s.ConfigId) + 1;
            ConfigPicker.SelectedIndex = Math.Max(0, idx);
            _syncing = false;
            BackupButton.Visibility = Visibility.Collapsed;
        }
        else
        {
            var e = _item?.Entry;
            TitleText.Text = _item?.DisplayName ?? "Drive";
            SubtitleText.Text = e?.DriveName ?? $"Waiting for a drive matching “{_item?.Config?.Match.DriveName}”";
            StatusText.Text = e == null ? "Disconnected"
                : $"{(e.DevicePath.Length == 0 ? $"disc:{e.Index}" : e.DevicePath)} · {e.State.DisplayName()}" +
                  (e.State == DriveState.Inserted ? $" · {(e.DiscName.Length == 0 ? e.Flags.DiscTypeName() : $"{e.DiscName} ({e.Flags.DiscTypeName()})")}" : "");
            HeaderIcon.Glyph = e?.State == DriveState.Inserted ? "" : "";
            bool inserted = e?.State == DriveState.Inserted;
            OpenDiscButton.IsEnabled = inserted && !DriveBusy && _session?.IsLoading != true;
            RipButton.IsEnabled = inserted && ActiveJob == null;
            EjectButton.IsEnabled = e != null && !DriveBusy;
            ConfigureButton.Content = _item?.Config == null && e != null ? "Set up this drive…" : "Configure…";
            BackupButton.Visibility = Visibility.Visible;

            RipMenu.Items.Clear();
            foreach (var mode in Enum.GetValues<RipMode>())
            {
                var mi = new MenuFlyoutItem { Text = mode.Label() };
                var m = mode;
                mi.Click += (_, _) => { if (_item != null) State.QuickRip(_item, m); };
                RipMenu.Items.Add(mi);
            }
            if (_item?.Config == null && e != null) AddChip("Default configuration", Colors.DarkOrange);
        }

        AddChip(cfg.Rip.Mode.ShortLabel(), Colors.SteelBlue);
        var r = cfg.Rip.TitleSelection;
        AddChip(r.Strategy switch
        {
            TitleStrategy.All => "All titles",
            TitleStrategy.Longest => r.LongestCount == 1 ? "Main feature" : $"Longest {r.LongestCount}",
            TitleStrategy.Indices => $"Titles {r.IndexPattern}",
            _ => "Manual titles",
        }, Colors.MediumPurple);
        AddChip(cfg.Profile.Mode switch
        {
            ProfileMode.MakemkvDefault => "Default profile",
            ProfileMode.Generated => $"Profile: {cfg.Profile.Generated.Name}",
            _ => "Custom profile",
        }, Colors.Teal);
        if (cfg.Settings.Count > 0) AddChip($"{cfg.Settings.Count} setting override(s)", Colors.SlateBlue);
        var steps = cfg.PostProcess.Count(p => p.Enabled);
        if (steps > 0) AddChip($"{steps} post-process step(s)", Colors.PaleVioletRed);
        if (cfg.Automation.AutoRipOnInsert) AddChip("Auto-rip", Colors.DarkOrange);
    }

    void AddChip(string text, Windows.UI.Color color)
    {
        Chips.Children.Add(new Border
        {
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(8, 2, 8, 2),
            Background = new SolidColorBrush(Windows.UI.Color.FromArgb(40, color.R, color.G, color.B)),
            Child = new TextBlock { Text = text, FontSize = 12, Foreground = new SolidColorBrush(color) },
        });
    }

    // --- job ----------------------------------------------------------------------------------

    RipJob? _shownJob;

    void RefreshJob()
    {
        var job = ActiveJob ?? State.Jobs.LastOrDefault(j => j.LaneKey == Lane && j.State.IsFinished() && j.FinishedAt > DateTime.Now.AddMinutes(-10));
        if (!ReferenceEquals(job, _shownJob))
        {
            _shownJob = job;
            JobHost.Content = job != null ? new JobCard(job) : null;
            if (job != null) job.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(RipJob.State)) RefreshHeader(); };
        }
        RefreshHeader();
    }

    // --- disc area ----------------------------------------------------------------------------

    void Show(UIElement panel)
    {
        foreach (var p in new UIElement[] { EmptyPanel, LoadingPanel, ErrorPanel, BrowserPanel })
            p.Visibility = ReferenceEquals(p, panel) ? Visibility.Visible : Visibility.Collapsed;
        ActionBar.Visibility = ReferenceEquals(panel, BrowserPanel) ? Visibility.Visible : Visibility.Collapsed;
    }

    void RefreshDiscArea()
    {
        var s = _session;
        if (s == null)
        {
            Show(EmptyPanel);
            EmptyTitle.Text = "Drive not connected";
            EmptyText.Text = "This configuration will be used as soon as a matching drive is connected.";
            EmptyOpenButton.Visibility = Visibility.Collapsed;
            return;
        }
        if (s.IsLoading)
        {
            Show(LoadingPanel);
            LoadingText.Text = s.Operation.Length == 0 ? "Reading disc…" : s.Operation;
            LoadingProgress.Value = s.Progress;
            if (LoadingLogHost.Content == null) LoadingLogHost.Content = new LogList(s.Log);
            return;
        }
        LoadingLogHost.Content = null;
        if (s.Info is { } info)
        {
            Show(BrowserPanel);
            if (!ReferenceEquals(info, _shownInfo)) { BuildTitles(info); BuildIdentityBar(info); }
            RefreshHeader();
            return;
        }
        if (s.LoadError is { } err)
        {
            Show(ErrorPanel);
            LoadErrorBar.Message = err;
            ErrorLogHost.Content = new LogList(s.Log);
            return;
        }
        Show(EmptyPanel);
        EmptyTitle.Text = "No disc opened";
        EmptyText.Text = IsSource ? "Reload to read the image." : "Open the disc to browse titles and tracks, or use Rip to apply this drive's rules directly.";
        EmptyOpenButton.Visibility = _item?.Entry?.State == DriveState.Inserted || IsSource ? Visibility.Visible : Visibility.Collapsed;
    }

    void BuildTitles(DiscInfo info)
    {
        _shownInfo = info;
        _titleChecks.Clear();
        _trackPanels.Clear();
        TitleList.Items.Clear();
        var decisions = TitleSelector.Evaluate(info.Titles, CurrentConfig.Rip.TitleSelection).Decisions.ToDictionary(d => d.TitleIndex);
        var longest = info.Titles.OrderByDescending(t => t.DurationSeconds).FirstOrDefault()?.Index;
        foreach (var t in info.Titles) TitleList.Items.Add(BuildTitleRow(t, decisions.GetValueOrDefault(t.Index), t.Index == longest));
        SyncChecks();
        ShowInfo(-1, null);
    }

    FrameworkElement BuildTitleRow(TitleInfo t, TitleDecision? decision, bool longest)
    {
        var row = new StackPanel { Tag = t.Index, Padding = new Thickness(0, 4, 0, 4) };
        var header = new Grid { ColumnSpacing = 8 };
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var tracks = new StackPanel { Visibility = Visibility.Collapsed, Margin = new Thickness(64, 4, 0, 0), Spacing = 2 };
        _trackPanels[t.Index] = tracks;
        var expand = new Button { Content = new FontIcon { Glyph = "", FontSize = 12 }, Background = new SolidColorBrush(Colors.Transparent), BorderThickness = new Thickness(0), Padding = new Thickness(6) };
        expand.Click += (_, _) =>
        {
            var open = tracks.Visibility == Visibility.Collapsed;
            tracks.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
            expand.Content = new FontIcon { Glyph = open ? "" : "", FontSize = 12 };
        };
        header.Children.Add(expand);

        var check = new CheckBox { MinWidth = 0, Padding = new Thickness(0) };
        check.Checked += (_, _) => { if (!_syncing) _session?.SetTitleSelected(t.Index, true); };
        check.Unchecked += (_, _) => { if (!_syncing) _session?.SetTitleSelected(t.Index, false); };
        _titleChecks[t.Index] = check;
        Grid.SetColumn(check, 1);
        header.Children.Add(check);

        var texts = new StackPanel();
        var line1 = $"Title {t.Index}    {t.DurationText}    {t.ChapterCount} ch    {t.SizeText}" + (longest ? "    ★ longest" : "") +
                    (t.Angle is > 0 ? $"    angle {t.Angle}" : "");
        texts.Children.Add(new TextBlock { Text = line1, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
        var parts = new List<string>();
        if (t.Name.Length > 0) parts.Add(t.Name);
        if (t.SourceTitleId is { } src) parts.Add($"source #{src}");
        if (t.SourceFileName.Length > 0) parts.Add(t.SourceFileName);
        if (t.OutputFileName.Length > 0) parts.Add("→ " + t.OutputFileName);
        if (t.SegmentMap.Length > 0) parts.Add("segments " + t.SegmentMap);
        texts.Children.Add(new TextBlock { Text = string.Join(" · ", parts), Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"], Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"], TextTrimming = TextTrimming.CharacterEllipsis });
        Grid.SetColumn(texts, 2);
        header.Children.Add(texts);

        if (decision != null)
        {
            var icon = new FontIcon { Glyph = decision.Selected ? "" : "", FontSize = 14, Foreground = decision.Selected ? new SolidColorBrush(Colors.SeaGreen) : (Brush)Application.Current.Resources["TextFillColorTertiaryBrush"] };
            ToolTipService.SetToolTip(icon, "Drive rules: " + decision.Reason);
            Grid.SetColumn(icon, 3);
            header.Children.Add(icon);
        }

        var menu = new MenuFlyout();
        var customize = new MenuFlyoutItem { Text = "Choose tracks manually" };
        customize.Click += (_, _) =>
        {
            if (State.Mkvmerge == null) { App.MainWindow.ShowError("Choosing individual tracks requires mkvmerge (MKVToolNix)."); return; }
            _session?.CustomizeTracks(t.Index);
            BuildTracks(t);
            tracks.Visibility = Visibility.Visible;
        };
        var reset = new MenuFlyoutItem { Text = "Use the profile's track selection" };
        reset.Click += (_, _) => { _session?.ResetTracks(t.Index); BuildTracks(t); };
        menu.Items.Add(customize);
        menu.Items.Add(reset);
        row.ContextFlyout = menu;

        row.Children.Add(header);
        row.Children.Add(tracks);
        BuildTracks(t);
        return row;
    }

    void BuildTracks(TitleInfo t)
    {
        if (!_trackPanels.TryGetValue(t.Index, out var panel) || _session == null) return;
        panel.Children.Clear();
        bool custom = _session.HasCustomTracks(t.Index);
        if (!custom)
            panel.Children.Add(Form.Help("Tracks are chosen by the profile's selection rule. Right-click the title to choose tracks manually."));
        foreach (var tr in t.Tracks)
        {
            var g = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            if (custom)
            {
                var cb = new CheckBox { IsChecked = _session.IsTrackSelected(t.Index, tr.Index), MinWidth = 0 };
                var track = tr.Index;
                cb.Checked += (_, _) => _session?.SetTrack(t.Index, track, true);
                cb.Unchecked += (_, _) => _session?.SetTrack(t.Index, track, false);
                g.Children.Add(cb);
            }
            g.Children.Add(new FontIcon
            {
                FontSize = 14,
                Glyph = tr.Kind switch { TrackKind.Video => "", TrackKind.Audio => "", TrackKind.Subtitle => "", _ => "" },
            });
            var text = tr.Summary.Length == 0 ? tr.Kind.ToString() : tr.Summary;
            var flags = tr.Flags.Descriptions();
            if (flags.Count > 0) text += "  [" + string.Join(", ", flags) + "]";
            if (tr.IsDefault) text += "  (default)";
            var tb = new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center };
            g.Children.Add(tb);
            var trackIndex = tr.Index;
            g.Tapped += (_, _) => ShowInfo(t.Index, trackIndex);
            panel.Children.Add(g);
        }
    }

    void SyncChecks()
    {
        if (_session == null) return;
        _syncing = true;
        foreach (var (i, cb) in _titleChecks) cb.IsChecked = _session.SelectedTitles.Contains(i);
        _syncing = false;
        var info = _session.Info;
        SelectionSummary.Text = info == null ? "" :
            $"{_session.SelectedTitles.Count} of {info.Titles.Count} titles · {TitleInfo.FormatBytes(_session.SelectedSizeBytes)}" +
            (_session.TrackSelections.Keys.Any(_session.SelectedTitles.Contains) ? " · custom tracks" : "");
        MakeMkvButton.IsEnabled = _session.SelectedTitles.Count > 0 && !DriveBusy;
        BackupButton.IsEnabled = !DriveBusy;
        SelectRuleItem.Text = $"Apply “{CurrentConfig.Name}” title rules";
        OutputPreview.Text = "→ " + OutputPreviewText();
        if (_sampleName != null) _sampleName.Text = SampleFileName();
    }

    // --- identity (name, movie / TV, first episode) ------------------------------------------

    TextBlock? _sampleName;
    TextBlock? _formatBadge;

    MediaIdentity Identity(bool overrides = true)
    {
        var s = _session!;
        return MediaIdentity.Resolve(s.Info, s.Info?.Name ?? "", false, s.DiscFlags, null,
            overrides ? s.MediaName : "", overrides ? s.MediaKind : null);
    }

    void BuildIdentityBar(DiscInfo info)
    {
        IdentityBar.Children.Clear();
        if (_session == null) return;
        var s = _session;
        var auto = Identity(false);
        var name = new TextBox { Text = s.MediaName, PlaceholderText = auto.Name, Width = 240 };
        ToolTipService.SetToolTip(name, $"Movie or show name used for folder and file names. Empty = “{auto.Name}”, read from the disc.");
        var kind = new ComboBox { Width = 170 };
        kind.Items.Add($"Auto ({auto.Kind.Label()})");
        kind.Items.Add(MediaKind.Movie.Label());
        kind.Items.Add(MediaKind.Tv.Label());
        kind.SelectedIndex = s.MediaKind switch { MediaKind.Movie => 1, MediaKind.Tv => 2, _ => 0 };
        ToolTipService.SetToolTip(kind, $"Detected: {auto.Reason}");
        var first = new NumberBox { PlaceholderText = "First episode", Width = 130, Minimum = 0, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Hidden };
        first.Value = s.FirstEpisode is { } fe ? fe : double.NaN;
        ToolTipService.SetToolTip(first, "Number of the first episode on this disc. Empty = read from the disc menus, or 1.");
        _formatBadge = new TextBlock { VerticalAlignment = VerticalAlignment.Center, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };
        _sampleName = new TextBlock { VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis, MaxWidth = 640,
                                      Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"] };
        void Update()
        {
            var id = Identity();
            first.Visibility = id.Kind == MediaKind.Tv ? Visibility.Visible : Visibility.Collapsed;
            _formatBadge.Text = id.FormatCode;
            ToolTipService.SetToolTip(_formatBadge, id.Format.Label());
            _sampleName.Text = SampleFileName();
            OutputPreview.Text = "→ " + OutputPreviewText();
        }
        name.TextChanged += (_, _) => { s.MediaName = name.Text; Update(); };
        kind.SelectionChanged += (_, _) =>
        {
            s.MediaKind = kind.SelectedIndex switch { 1 => MediaKind.Movie, 2 => MediaKind.Tv, _ => null };
            Update();
        };
        first.ValueChanged += (_, a) => { s.FirstEpisode = double.IsNaN(a.NewValue) ? null : (int)Math.Round(a.NewValue); Update(); };
        IdentityBar.Children.Add(name);
        IdentityBar.Children.Add(kind);
        IdentityBar.Children.Add(first);
        IdentityBar.Children.Add(_formatBadge);
        IdentityBar.Children.Add(_sampleName);
        Update();
    }

    string SampleFileName()
    {
        if (_session is not { Info: { } info } session) return "";
        var cfg = CurrentConfig;
        var template = cfg.Output.FileNameTemplate.Trim();
        if (template.Length == 0) return "MakeMKV's file names";
        var id = Identity();
        var v = TemplateRenderer.DateValues();
        foreach (var kv in id.TemplateValues("Rip")) v[kv.Key] = kv.Value;
        v["disc"] = info.Name; v["volume"] = info.VolumeName; v["type"] = info.TypeToken; v["drive"] = cfg.Name; v["job"] = "xxxxxxxx";
        var title = info.Titles.FirstOrDefault(t => session.SelectedTitles.Contains(t.Index)) ?? info.Titles.FirstOrDefault();
        if (title != null)
        {
            foreach (var kv in JobRunner.TitleValues(title, 1, info, null)) v[kv.Key] = kv.Value;
            v["track"] = MediaIdentity.TrackLabel(title);
        }
        if (id.Kind == MediaKind.Tv)
        {
            v["episode"] = MediaIdentity.EpisodeLabel(session.FirstEpisode ?? 1, 2);
            v["episodeNumber"] = (session.FirstEpisode ?? 1).ToString(System.Globalization.CultureInfo.InvariantCulture);
        }
        return "e.g. " + TemplateRenderer.RenderPath(template, v) + ".mkv";
    }

    string OutputPreviewText()
    {
        if (_session is { OutputFolderOverride.Length: > 0 } s) return s.OutputFolderOverride + "  (one-off folder)";
        var cfg = CurrentConfig;
        var v = TemplateRenderer.DateValues();
        v["disc"] = _session?.Info?.Name is { Length: > 0 } n ? n : "Disc";
        v["volume"] = _session?.Info?.VolumeName ?? "";
        v["type"] = _session?.Info?.TypeToken ?? "disc";
        v["drive"] = cfg.Name;
        v["job"] = "xxxxxxxx";
        if (_session?.Info != null) foreach (var kv in Identity().TemplateValues("Rip")) v[kv.Key] = kv.Value;
        var root = Paths.ExpandUser(State.Config.OutputRootFor(cfg));
        var rel = TemplateRenderer.RenderPath(cfg.Output.FolderTemplate, v);
        return rel.Length == 0 ? root : Path.Combine(root, rel);
    }

    void TitleList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (TitleList.SelectedItem is FrameworkElement { Tag: int index }) ShowInfo(index, null);
    }

    void ShowInfo(int title, int? track)
    {
        InfoPanel.Children.Clear();
        var info = _session?.Info;
        if (info == null) return;
        Dictionary<int, string> attrs;
        string heading;
        if (title < 0) { attrs = info.Attributes; heading = "Disc information"; }
        else if (track is { } tr) { attrs = info.Title(title)?.Tracks.FirstOrDefault(x => x.Index == tr)?.Attributes ?? new(); heading = $"Title {title} · Track {tr}"; }
        else { attrs = info.Title(title)?.Attributes ?? new(); heading = $"Title {title}"; }
        InfoPanel.Children.Add(new TextBlock { Text = heading, Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"] });

        if (title >= 0 && track == null && _session != null && info.Title(title) is { } t)
        {
            var box = new TextBox
            {
                Header = "Output file name",
                PlaceholderText = t.OutputFileName.Length == 0 ? "MakeMKV default" : t.OutputFileName,
                Text = _session.TitleNameOverrides.GetValueOrDefault(title) ?? "",
            };
            box.TextChanged += (_, _) =>
            {
                if (box.Text.Length == 0) _session.TitleNameOverrides.Remove(title); else _session.TitleNameOverrides[title] = box.Text;
            };
            InfoPanel.Children.Add(box);
            InfoPanel.Children.Add(Form.Help("Overrides the configuration's file name template. {tokens} are allowed."));
        }

        foreach (var key in attrs.Keys.OrderBy(k => k))
        {
            var value = attrs[key];
            if (value.Length == 0) continue;
            var id = (AttributeId)key;
            if (Enum.IsDefined(typeof(AttributeId), key) && !id.IsUserVisible()) continue;
            if (id == AttributeId.StreamFlags && int.TryParse(value, out var f) && ((StreamFlags)f).Descriptions() is { Count: > 0 } d) value = string.Join(", ", d);
            var sp = new StackPanel();
            sp.Children.Add(new TextBlock { Text = Enum.IsDefined(typeof(AttributeId), key) ? id.DisplayName() : $"Attribute {key}", Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"], Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"] });
            sp.Children.Add(new TextBlock { Text = value, IsTextSelectionEnabled = true, TextWrapping = TextWrapping.Wrap });
            InfoPanel.Children.Add(sp);
        }
    }

    // --- actions ------------------------------------------------------------------------------

    async void OpenDisc_Click(object sender, RoutedEventArgs e)
    {
        if (_session != null) await State.LoadDiscAsync(_session);
    }

    void CancelLoad_Click(object sender, RoutedEventArgs e) => _session?.Runner?.Cancel();

    void Rip_Click(SplitButton sender, SplitButtonClickEventArgs args)
    {
        if (_item != null) State.QuickRip(_item);
    }

    async void Eject_Click(object sender, RoutedEventArgs e) => await State.EjectAsync(Lane);

    void CloseSource_Click(object sender, RoutedEventArgs e)
    {
        if (_session != null) State.CloseFileSource(_session);
        App.MainWindow.Navigate("queue");
    }

    void Configure_Click(object sender, RoutedEventArgs e)
    {
        if (_item?.Config is { } c) App.MainWindow.ShowDriveConfig(c.Id);
        else if (_item?.Entry is { } entry) App.MainWindow.ShowDriveConfig(State.Configure(entry).Id);
    }

    void ConfigPicker_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing || _session == null || ConfigPicker.SelectedIndex < 0) return;
        _session.ConfigId = ConfigPicker.SelectedIndex == 0 ? State.Config.DefaultDrive.Id : State.Config.Drives[ConfigPicker.SelectedIndex - 1].Id;
        _session.ApplyRule(CurrentConfig.Rip.TitleSelection);
        _shownInfo = null;
        RefreshAll();
    }

    void SelectAll_Click(object sender, RoutedEventArgs e)
    {
        if (_session?.Info is not { } info) return;
        foreach (var t in info.Titles) _session.SelectedTitles.Add(t.Index);
        _session.NotifySelectionChanged();
    }

    void SelectNone_Click(object sender, RoutedEventArgs e)
    {
        if (_session == null) return;
        _session.SelectedTitles.Clear();
        _session.NotifySelectionChanged();
    }

    void SelectRule_Click(object sender, RoutedEventArgs e) => _session?.ApplyRule(CurrentConfig.Rip.TitleSelection);

    async void OutputFolder_Click(object sender, RoutedEventArgs e)
    {
        if (_session == null) return;
        if (_session.OutputFolderOverride.Length > 0)
        {
            _session.OutputFolderOverride = "";
        }
        else if (await Pickers.PickFolderAsync() is { } folder)
        {
            _session.OutputFolderOverride = folder;
        }
        ToolTipService.SetToolTip(OutputFolderButton, _session.OutputFolderOverride.Length > 0
            ? $"Output folder: {_session.OutputFolderOverride} (click to reset)"
            : "Choose a different output folder for this disc");
        SyncChecks();
    }

    void Rip(RipMode mode)
    {
        if (_session == null) return;
        State.RipSession(_session, mode);
        RefreshJob();
    }

    void MakeMkv_Click(object sender, RoutedEventArgs e) => Rip(RipMode.Mkv);
    void BackupEncrypted_Click(object sender, RoutedEventArgs e) => Rip(RipMode.Backup);
    void BackupDecrypted_Click(object sender, RoutedEventArgs e) => Rip(RipMode.BackupDecrypted);
    void BackupThenMkv_Click(object sender, RoutedEventArgs e) => Rip(RipMode.BackupThenMkv);
}
