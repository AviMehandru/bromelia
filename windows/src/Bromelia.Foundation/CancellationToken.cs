using System;

namespace Bromelia.Foundation;

/// <summary>Cancellation that crosses layers (plan §6): the side that is told. It can't cancel anything itself; its
/// CancellationSource does.</summary>
public sealed class CancellationToken
{
    private readonly System.Threading.CancellationToken _token;

    internal CancellationToken(System.Threading.CancellationToken token) { _token = token; }

    public bool IsCancelled => _token.IsCancellationRequested;

    /// <summary>Runs <paramref name="handler"/> once on cancellation (straight away if already cancelled).</summary>
    public IDisposable OnCancel(Action handler) => _token.Register(handler);

    /// <summary>The .NET token, for APIs that take one.</summary>
    public System.Threading.CancellationToken Token => _token;
}
