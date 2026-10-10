using System;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>A timer of <see cref="EngineClock"/>. Used on the executor only.</summary>
public sealed class EngineTimer
{
    readonly Action _handler;
    readonly bool _once;
    bool _ended;

    internal ITimerHandle? Handle;

    internal EngineTimer(Action handler, bool once)
    {
        _handler = handler;
        _once = once;
    }

    /// <summary>Stops the timer: a handler the clock already posted doesn't run either. Does nothing the second time,
    /// or after a one-off timer fired.</summary>
    public void Cancel()
    {
        if (_ended) return;
        _ended = true;
        Handle?.Cancel();
    }

    /// <summary>Posted by the clock's handler; runs on the executor.</summary>
    internal void Fire()
    {
        if (_ended) return;
        if (_once) _ended = true;
        _handler();
    }
}
