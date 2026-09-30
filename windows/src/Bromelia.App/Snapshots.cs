using System.Runtime.InteropServices.WindowsRuntime;
using Bromelia.Core.Config;
using Bromelia.Core.Engine;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;

namespace Bromelia.App;

/// <summary>Screenshot mode, for looking at screens without a Windows machine at hand: with BROMELIA_SNAPSHOT=&lt;folder&gt;
/// the app shows every page (each settings and drive configuration tab, scrolled through; every drive) in the light and
/// the dark theme, saves each as a PNG and quits. CI runs it with a stand-in for makemkvcon. It sets up a drive and
/// adds background steps, so it only runs with its own data folder (BROMELIA_DATA_DIR).</summary>
static class Snapshots
{
    public static string? Folder => Environment.GetEnvironmentVariable("BROMELIA_SNAPSHOT") is { Length: > 0 } f ? f : null;

    static int _count;

    /// <summary>Records an unhandled exception in screenshot mode (CI shows errors.txt).</summary>
    public static void Failed(Exception e)
    {
        if (Folder is { } f)
            try { Directory.CreateDirectory(f); File.AppendAllText(Path.Combine(f, "errors.txt"), e + Environment.NewLine); } catch (IOException) { }
    }

    public static async Task RunAsync(string folder)
    {
        Directory.CreateDirectory(folder);
        try
        {
            if (Paths.DataOverride is not { Length: > 0 })
                throw new InvalidOperationException("BROMELIA_SNAPSHOT needs BROMELIA_DATA_DIR: screenshot mode changes the configuration.");
            await Task.Delay(1500);
            await App.State.RefreshDrivesAsync(true);

            // A configured drive (for its configuration page) and background steps (for the Queue page's list).
            if (App.State.DriveItems.FirstOrDefault(d => d.Entry != null && d.Config == null) is { Entry: { } entry })
                App.State.Configure(entry);
            var cmd = Environment.GetEnvironmentVariable("ComSpec") ?? @"C:\Windows\System32\cmd.exe";
            foreach (var title in new[] { "Snapshot Movie (2024)", "Snapshot Show (2023) - Season 2" })
                App.State.Background.Enqueue(new BackgroundWork(Guid.NewGuid(), title,
                    new() { new PostProcessStep { Name = "Encode", Executable = cmd, Arguments = "/c ping -n 60 127.0.0.1 >nul", Background = true } },
                    new PostProcessor.Context(JobState.Succeeded, new Dictionary<string, string>(), null, Array.Empty<string>(), new Dictionary<string, string>()),
                    Path.Combine(folder, "background-" + _count++ + ".txt")));
            _count = 0;

            var root = (Grid)App.MainWindow.Content;
            foreach (var (theme, background) in new[] { (ElementTheme.Light, Windows.UI.Color.FromArgb(255, 243, 243, 243)), (ElementTheme.Dark, Windows.UI.Color.FromArgb(255, 32, 32, 32)) })
            {
                // Mica isn't drawn into a RenderTargetBitmap: paint its fallback colour behind the pages instead.
                root.RequestedTheme = theme;
                root.Background = new SolidColorBrush(background);
                var t = theme == ElementTheme.Dark ? "dark" : "light";
                foreach (var (tag, name) in new[] { ("queue", "queue"), ("history", "history"), ("verify", "archive-check"), ("tools", "drive-tools"), ("settings", "settings") })
                {
                    App.MainWindow.Navigate(tag);
                    await Page(root, $"{t}-{name}");
                }
                foreach (var d in App.State.DriveItems.Where(d => d.Entry != null).ToList())
                {
                    App.MainWindow.Navigate("drive:" + d.Id);
                    await Page(root, $"{t}-drive{d.Entry!.Index}-{d.Entry.State}");
                }
                if (App.State.Config.Drives.FirstOrDefault() is { } configured)
                {
                    App.MainWindow.ShowDriveConfig(configured.Id);
                    await Page(root, $"{t}-drive-config");
                }
            }
        }
        catch (Exception e)
        {
            Failed(e);
        }
        Application.Current.Exit();

        async Task Page(Grid root, string name)
        {
            await Task.Delay(1200);
            var frame = Descendants<Frame>(root).First();
            if (Descendants<Pivot>(frame).FirstOrDefault() is { } pivot) await Tabs(root, pivot, name);
            else await Scrolled(root, frame, name);
        }

        async Task Tabs(Grid root, Pivot pivot, string name)
        {
            for (var i = 0; i < pivot.Items.Count; i++)
            {
                pivot.SelectedIndex = i;
                await Task.Delay(900);
                var item = (PivotItem)pivot.Items[i];
                var label = $"{name}-{Slug(item.Header?.ToString() ?? i.ToString())}";
                if (Descendants<Pivot>(item).FirstOrDefault() is { } inner) await Tabs(root, inner, label);
                else await Scrolled(root, item, label);
            }
            pivot.SelectedIndex = 0;
        }

        async Task Scrolled(Grid root, DependencyObject within, string name)
        {
            var sv = Descendants<ScrollViewer>(within).Where(s => s.ActualHeight > 0).OrderByDescending(s => s.ScrollableHeight).FirstOrDefault();
            if (sv == null || sv.ScrollableHeight <= 1)
            {
                await Save(root, folder, name);
                return;
            }
            var part = 1;
            for (double y = 0; ; y += sv.ViewportHeight * 0.9, part++)
            {
                sv.ChangeView(null, y, null, true);
                await Task.Delay(400);
                await Save(root, folder, $"{name}-{part}");
                if (y >= sv.ScrollableHeight || part >= 12) break;
            }
            sv.ChangeView(null, 0, null, true);
        }
    }

    static async Task Save(UIElement element, string folder, string name)
    {
        var bitmap = new RenderTargetBitmap();
        await bitmap.RenderAsync(element);
        var pixels = await bitmap.GetPixelsAsync();
        using var stream = new InMemoryRandomAccessStream();
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, (uint)bitmap.PixelWidth, (uint)bitmap.PixelHeight, 96, 96, pixels.ToArray());
        await encoder.FlushAsync();
        stream.Seek(0);
        using var file = File.Create(Path.Combine(folder, $"{++_count:000}-{name}.png"));
        await stream.AsStreamForRead().CopyToAsync(file);
    }

    static string Slug(string text) =>
        string.Join("-", new string(text.ToLowerInvariant().Select(ch => char.IsLetterOrDigit(ch) ? ch : ' ').ToArray()).Split(' ', StringSplitOptions.RemoveEmptyEntries));

    static IEnumerable<T> Descendants<T>(DependencyObject parent) where T : DependencyObject
    {
        for (var i = 0; i < VisualTreeHelper.GetChildrenCount(parent); i++)
        {
            var child = VisualTreeHelper.GetChild(parent, i);
            if (child is T t) yield return t;
            foreach (var d in Descendants<T>(child)) yield return d;
        }
    }
}
