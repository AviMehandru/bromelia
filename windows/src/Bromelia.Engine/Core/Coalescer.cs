using System;
using System.Collections.Generic;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>
/// At most one value per interval per key (plan §8 rule 5): a job's progress before it becomes an event or is stored. A
/// value goes out at once when nothing for its key went out in the last interval; otherwise it waits (replacing the one
/// waiting) until one interval after the last one went out. Used on the executor. Robot progress goes through it (a
/// value at most every 0.1 s); shared/fixtures/engine/coalescer.cases.json.
/// </summary>
public sealed class Coalescer
{
    readonly EngineClock _clock;
    readonly long _intervalMs;
    readonly Action<string, JsonValue> _emit;
    readonly Dictionary<string, Key> _keys = new();

    sealed class Key
    {
        public long? LastSentMs;
        public JsonValue? Waiting;
        public EngineTimer? Timer;
    }

    public Coalescer(EngineClock clock, Duration interval, Action<string, JsonValue> emit)
    {
        _clock = clock;
        _intervalMs = (long)Math.Round(interval.Seconds * 1000);
        _emit = emit;
    }

    long NowMs() => (long)Math.Round(_clock.Monotonic().Seconds * 1000);

    public void Offer(string key, JsonValue value)
    {
        if (!_keys.TryGetValue(key, out var k)) _keys[key] = k = new Key();
        var now = NowMs();
        if (k.Waiting == null && (k.LastSentMs is not { } last || now - last >= _intervalMs))
        {
            Send(key, k, value, now);
            return;
        }
        k.Waiting = value;
        if (k.Timer != null) return;
        var due = k.LastSentMs.GetValueOrDefault(now) + _intervalMs;
        k.Timer = _clock.Timer(new TimerSchedule.At(new Instant(_clock.Now().UnixMilliseconds + Math.Max(0, due - now))), () =>
        {
            k.Timer = null;
            if (k.Waiting is { } w) Send(key, k, w, NowMs());
        });
    }

    /// <summary>Sends a waiting value at once.</summary>
    public void Flush(string key)
    {
        if (_keys.TryGetValue(key, out var k) && k.Waiting is { } w) Send(key, k, w, NowMs());
    }

    /// <summary>Throws a waiting value away and forgets when the key last went out.</summary>
    public void Drop(string key)
    {
        if (!_keys.Remove(key, out var k)) return;
        k.Timer?.Cancel();
    }

    void Send(string key, Key k, JsonValue value, long now)
    {
        k.Timer?.Cancel();
        k.Timer = null;
        k.Waiting = null;
        k.LastSentMs = now;
        _emit(key, value);
    }
}
