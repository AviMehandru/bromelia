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

/// <summary>All jobs: running, waiting, queued and recently finished.</summary>
public sealed partial class QueuePage : Page
{
    readonly StackPanel _list = new() { Spacing = 10 };
    readonly TextBlock _empty = new()
    {
        Text = "No jobs. Insert a disc and choose Rip, or open a disc to pick titles. Drives set to rip automatically start jobs on their own.",
        TextWrapping = TextWrapping.Wrap, Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
    };

    public QueuePage()
    {
        InitializeComponent();
        PageTitle.Text = "Queue";
        HeaderButtons.Children.Add(PageHelpers.Button("Clear finished", () => App.State.ClearFinishedJobs()));
        var sp = new StackPanel { Spacing = 10 };
        sp.Children.Add(_empty);
        sp.Children.Add(_list);
        Body.Content = new ScrollViewer { Content = sp };
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        App.State.Jobs.CollectionChanged += OnChanged;
        Rebuild();
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e) => App.State.Jobs.CollectionChanged -= OnChanged;

    void OnChanged(object? sender, NotifyCollectionChangedEventArgs e) => Rebuild();

    void Rebuild()
    {
        _list.Children.Clear();
        foreach (var j in App.State.Jobs) _list.Children.Add(new JobCard(j));
        _empty.Visibility = App.State.Jobs.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }
}
