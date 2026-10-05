using System;

namespace Bromelia.Foundation;

/// <summary>What cancels a CancellationToken: the job (or whoever started the work) keeps the source and hands out
/// <see cref="Token"/>, so what receives the token can't cancel the caller's work. A linked source is cancelled with
/// its parent too, until it is closed: closing removes its registration on the parent, so a parent that lives as long
/// as the daemon doesn't keep one per job.</summary>
public sealed class CancellationSource : IDisposable
{
    private readonly System.Threading.CancellationTokenSource _source = new();
    private IDisposable? _parent;

    public CancellationSource() { Token = new CancellationToken(_source.Token); }

    /// <summary>A source that is cancelled when <paramref name="parent"/> is, or when it is cancelled itself.</summary>
    public static CancellationSource Linked(CancellationToken parent)
    {
        var source = new CancellationSource();
        source._parent = parent.OnCancel(source.Cancel);
        return source;
    }

    public CancellationToken Token { get; }

    /// <summary>Cancels the token and runs its handlers once.</summary>
    public void Cancel() => _source.Cancel();

    /// <summary>No longer follows the parent (a linked source); the token keeps its state.</summary>
    public void Close()
    {
        _parent?.Dispose();
        _parent = null;
    }

    public void Dispose() => Close();
}
