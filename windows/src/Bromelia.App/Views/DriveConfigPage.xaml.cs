using Bromelia.App.Controls;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;

namespace Bromelia.App.Views;

/// <summary>Edits one drive configuration (a copy; saved on “Save”).</summary>
public sealed partial class DriveConfigPage : Page
{
    DriveConfig? _original;
    DriveConfig? _draft;
    bool _dirty;

    public DriveConfigPage()
    {
        InitializeComponent();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        var id = e.Parameter is Guid g ? g : Guid.Empty;
        _original = App.State.Config.DriveConfigById(id);
        if (_original == null) { PageTitle.Text = "Configuration not found"; return; }
        _draft = _original.Clone();
        bool isDefault = _original.Id == App.State.Config.DefaultDrive.Id;
        PageTitle.Text = $"Configure “{_original.Name}”";

        HeaderButtons.Children.Add(PresetsButton());
        if (!isDefault)
            HeaderButtons.Children.Add(PageHelpers.Button("Delete…", async () =>
            {
                var dlg = new ContentDialog
                {
                    Title = $"Delete the configuration “{_original.Name}”?", PrimaryButtonText = "Delete", CloseButtonText = "Cancel", XamlRoot = XamlRoot,
                };
                if (await dlg.ShowAsync() != ContentDialogResult.Primary) return;
                App.State.RemoveDriveConfig(_original.Id);
                App.MainWindow.Navigate("queue");
            }));
        HeaderButtons.Children.Add(PageHelpers.Button("Cancel", GoBack));
        HeaderButtons.Children.Add(PageHelpers.Button("Save", () =>
        {
            App.State.UpdateDrive(_draft);
            _dirty = false;
            GoBack();
        }, accent: true));

        var preview = App.State.Sessions.Values.FirstOrDefault(s => s.ConfigId == _original.Id && s.Info != null)?.Info;
        Body.Content = DriveConfigEditor.Build(_draft, isDefault, preview, () => _dirty = true);
    }

    DropDownButton PresetsButton()
    {
        var menu = new MenuFlyout();
        var save = new MenuFlyoutItem { Text = "Save as preset…" };
        save.Click += async (_, _) =>
        {
            if (_draft == null) return;
            var box = new TextBox { Text = _draft.Name };
            var dlg = new ContentDialog
            {
                Title = "Save preset", Content = box, PrimaryButtonText = "Save", CloseButtonText = "Cancel", XamlRoot = XamlRoot,
            };
            if (await dlg.ShowAsync() == ContentDialogResult.Primary) App.State.SavePreset(box.Text.Length == 0 ? _draft.Name : box.Text, _draft);
        };
        menu.Items.Add(save);
        if (App.State.Config.Presets.Count > 0) menu.Items.Add(new MenuFlyoutSeparator());
        foreach (var p in App.State.Config.Presets)
        {
            var item = new MenuFlyoutItem { Text = $"Apply “{p.Name}”" };
            var preset = p;
            item.Click += (_, _) => Reload(_draft!.ApplyingBody(preset.Config));
            menu.Items.Add(item);
        }
        menu.Items.Add(new MenuFlyoutSeparator());
        var fromDefault = new MenuFlyoutItem { Text = "Copy settings from the default configuration" };
        fromDefault.Click += (_, _) => Reload(_draft!.ApplyingBody(App.State.Config.DefaultDrive));
        menu.Items.Add(fromDefault);
        return new DropDownButton { Content = "Presets", Flyout = menu };
    }

    void Reload(DriveConfig c)
    {
        _draft = c;
        _dirty = true;
        bool isDefault = _original!.Id == App.State.Config.DefaultDrive.Id;
        Body.Content = DriveConfigEditor.Build(_draft, isDefault, null, () => _dirty = true);
    }

    async void GoBack()
    {
        if (_dirty)
        {
            var dlg = new ContentDialog
            {
                Title = "Discard changes?", PrimaryButtonText = "Discard", CloseButtonText = "Keep editing", XamlRoot = XamlRoot,
            };
            if (await dlg.ShowAsync() != ContentDialogResult.Primary) return;
        }
        if (Frame.CanGoBack) Frame.GoBack(); else App.MainWindow.Navigate("queue");
    }
}
