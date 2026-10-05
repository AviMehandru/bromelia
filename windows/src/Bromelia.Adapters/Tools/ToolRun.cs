using System;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>How every tool adapter runs its process: hands each output line to <c>onLine</c>, which may return a reason
/// to stop it (acted on once), stops it when the token is cancelled, and always leaves with the process ended. When
/// <c>onLine</c> or <c>started</c> throws, the process is stopped (StopReason.Policy) and waited for before the
/// exception goes on, so a tool never keeps the drive or writes behind the caller's back.</summary>
internal static class ToolRun
{
    /// <param name="started">Called once the process has started (a timer that stops it, for instance).</param>
    public static async Task<ProcessExit> Run(IProcessLauncher launcher, ProcessSpec spec, CancellationToken cancel,
        Func<OutputLine, StopReason?> onLine, Action<IRunningProcess>? started = null)
    {
        var process = launcher.Start(spec);
        using var onCancel = cancel.OnCancel(() => process.Stop(StopReason.Cancelled));
        try
        {
            started?.Invoke(process);
            var stopped = false;
            await foreach (var line in process.Lines().ConfigureAwait(false))
            {
                if (onLine(line) is { } reason && !stopped)
                {
                    stopped = true;
                    process.Stop(reason);
                }
            }
        }
        catch
        {
            process.Stop(StopReason.Policy);
            await process.Wait().ConfigureAwait(false);
            throw;
        }
        return await process.Wait().ConfigureAwait(false);
    }
}
