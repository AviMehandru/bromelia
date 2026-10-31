using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.TestKit;

/// <summary>Secrets in memory (the test root's keystore pair: the "system" store and the secrets folder). Thread-safe.
/// <see cref="Fails"/> makes every call about a name fail so before it acts.</summary>
public sealed class MemoryKeystore : IKeystore
{
    readonly object _lock = new();
    readonly SortedDictionary<string, string> _values = new(StringComparer.Ordinal);
    readonly Dictionary<string, BroError> _fails = new();
    readonly bool _systemStore;

    public MemoryKeystore(bool systemStore) { _systemStore = systemStore; }

    public bool IsSystemStore() => _systemStore;

    /// <summary>Calls about <paramref name="name"/> fail with <paramref name="error"/> (null: they work again).</summary>
    public void Fails(string name, BroError? error)
    {
        lock (_lock)
        {
            if (error == null) _fails.Remove(name);
            else _fails[name] = error;
        }
    }

    public IReadOnlyDictionary<string, string> Values()
    {
        lock (_lock) return new SortedDictionary<string, string>(_values, StringComparer.Ordinal);
    }

    public string? Get(string name)
    {
        lock (_lock)
        {
            Check(name);
            return _values.GetValueOrDefault(name);
        }
    }

    public void Set(string name, string value)
    {
        lock (_lock)
        {
            Check(name);
            _values[name] = value;
        }
    }

    public void Remove(string name)
    {
        lock (_lock)
        {
            Check(name);
            _values.Remove(name);
        }
    }

    void Check(string name)
    {
        if (_fails.TryGetValue(name, out var error)) throw new BroFailure(error);
    }
}
