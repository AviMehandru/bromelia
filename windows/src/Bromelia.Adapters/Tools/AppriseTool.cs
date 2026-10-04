using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>The apprise command (plan §10.1): <c>apprise -t title -b body url</c>. notify.needsApprise when the
/// locator can't find it (naming the URL's scheme: URLs are secrets), notify.appriseFailed when it exits
/// non-zero.</summary>
public sealed class AppriseTool
{
    readonly IProcessLauncher _launcher;
    readonly IToolLocator _locator;

    public AppriseTool(IProcessLauncher launcher, IToolLocator locator)
    {
        _launcher = launcher;
        _locator = locator;
    }

    public async Task Send(string url, string title, string body, CancellationToken cancel)
    {
        var exe = _locator.Locate(ToolKind.Apprise).Path
                  ?? throw new BroFailure(new BroMessage(MessageCode.NotifyNeedsApprise, Severity.Warning, ("target", JsonValue.Of(Scheme(url)))).ToError());
        var process = _launcher.Start(new ProcessSpec(exe, new[] { "-t", title, "-b", body, url }, new Dictionary<string, string>(), null,
            StopPolicy.InterruptFirst, new Duration(120)));
        using var onCancel = cancel.OnCancel(() => process.Stop(StopReason.Cancelled));
        await foreach (var _ in process.Lines().ConfigureAwait(false)) { }
        var exit = await process.Wait().ConfigureAwait(false);
        if (exit.Cancelled) throw new BroFailure(new BroError(MessageCode.Wire(MessageCode.JobCancelled)));
        if (exit.Status != 0)
            throw new BroFailure(new BroMessage(MessageCode.NotifyAppriseFailed, Severity.Warning, ("status", JsonValue.Of(exit.Status))).ToError());
    }

    /// <summary>The URL's scheme ("mailto"), or the whole value when it has none (then it isn't a secret URL).</summary>
    internal static string Scheme(string url)
    {
        var i = url.IndexOf("://", System.StringComparison.Ordinal);
        return i > 0 ? url.Substring(0, i) : url;
    }
}
