using System;

namespace Bromelia.Foundation;

/// <summary>Cancellation that crosses layers (plan §6), wrapping .NET's token so adapters can pass it on.</summary>
public sealed class CancellationToken
{
    private readonly System.Threading.CancellationTokenSource _source;

    public CancellationToken() : this(new System.Threading.CancellationTokenSource()) { }

    private CancellationToken(System.Threading.CancellationTokenSource source) { _source = source; }

    /// <summary>A token that is cancelled when <paramref name="parent"/> is, or when it is cancelled itself.</summary>
    public static CancellationToken Child(CancellationToken parent) =>
        new(System.Threading.CancellationTokenSource.CreateLinkedTokenSource(parent.Token));

    public bool IsCancelled => _source.IsCancellationRequested;

    public void Cancel() => _source.Cancel();

    /// <summary>Runs <paramref name="handler"/> once on cancellation (straight away if already cancelled).</summary>
    public IDisposable OnCancel(Action handler) => _source.Token.Register(handler);

    /// <summary>The .NET token, for APIs that take one.</summary>
    public System.Threading.CancellationToken Token => _source.Token;
}
