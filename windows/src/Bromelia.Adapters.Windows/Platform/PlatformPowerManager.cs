using System.Runtime.InteropServices;
using System.Threading;
using Bromelia.Ports;

namespace Bromelia.Adapters.Windows;

/// <summary>PowerManager on Windows (plan §10.3): SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) while any
/// guard is held. The state belongs to the thread that set it, so a dedicated thread owns it; each guard counts once,
/// and releasing one leaves the others.</summary>
public sealed class PlatformPowerManager : IPowerManager
{
    const uint ES_CONTINUOUS = 0x80000000;
    const uint ES_SYSTEM_REQUIRED = 0x00000001;

    readonly object _gate = new();
    readonly AutoResetEvent _changed = new(false);
    Thread? _thread;
    int _held;
    uint _applied = ES_CONTINUOUS;

    public IPowerGuard Inhibit(string reason)
    {
        lock (_gate)
        {
            _held++;
            if (_thread == null)
            {
                _thread = new Thread(Loop) { IsBackground = true, Name = "Bromelia keep awake" };
                _thread.Start();
            }
        }
        _changed.Set();
        return new Guard(this);
    }

    /// <summary>What the dedicated thread has set (for the tests).</summary>
    internal bool Awake
    {
        get { lock (_gate) return _applied != ES_CONTINUOUS; }
    }

    void Loop()
    {
        while (true)
        {
            _changed.WaitOne();
            uint want;
            lock (_gate) want = _held > 0 ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS;
            SetThreadExecutionState(want);
            lock (_gate) _applied = want;
        }
    }

    void Release()
    {
        lock (_gate) _held--;
        _changed.Set();
    }

    [DllImport("kernel32.dll")]
    static extern uint SetThreadExecutionState(uint flags);

    sealed class Guard : IPowerGuard
    {
        readonly PlatformPowerManager _owner;
        int _released;
        public Guard(PlatformPowerManager owner) { _owner = owner; }

        public void Release()
        {
            if (Interlocked.Exchange(ref _released, 1) == 0) _owner.Release();
        }
    }
}
