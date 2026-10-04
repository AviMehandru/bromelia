using System;
using System.Collections.Generic;
using System.Linq;
using System.Security;
using System.Text;
using System.Threading;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Adapters.Windows;

/// <summary>SettingsIsolation on Windows (plan §10.3). makemkvcon reads HKCU\Software\MakeMKV, so each launch writes
/// the run's values there, clearing the other catalogue keys, and gives the user's values back as soon as makemkvcon
/// has read them: on its first output line, when the lease is released (it exited), or after 15 s. Launches are
/// serialised: prepare waits for the previous lease to give the registry back. app_Key is touched only when the run
/// sets it. The values are also written to settings.conf.txt in the run's folder, for reference.</summary>
public sealed class RegistrySwapIsolation : ISettingsIsolation
{
    /// <summary>One launch at a time in this process: the registry is per user, not per job.</summary>
    static readonly SemaphoreSlim Launch = new(1, 1);

    readonly IFileSystem _fs;
    readonly IMakemkvRegistry _registry;
    readonly IReadOnlyCollection<string> _catalogKeys;
    readonly TimeSpan _restoreAfter;

    /// <param name="catalogKeys">Every key of shared/catalog/settings-catalog.json.</param>
    /// <param name="restoreAfter">When the values come back if makemkvcon says nothing (15 s).</param>
    public RegistrySwapIsolation(IFileSystem fs, IMakemkvRegistry registry, IReadOnlyCollection<string> catalogKeys, Duration? restoreAfter = null)
    {
        _fs = fs;
        _registry = registry;
        _catalogKeys = catalogKeys;
        _restoreAfter = TimeSpan.FromSeconds(restoreAfter?.Seconds ?? 15);
    }

    public IIsolationLease Prepare(MakemkvRunSettings settings)
    {
        Launch.Wait();
        var snapshot = new Dictionary<string, string?>(StringComparer.Ordinal);
        try
        {
            _fs.CreateDirectory(settings.WorkDirectory, parentsMustExist: false);
            string? profile = null;
            if (settings.ProfileXml is { } xml)
            {
                profile = settings.WorkDirectory + "\\profile.mmcp.xml";
                _fs.WriteAtomically(profile, Encoding.UTF8.GetBytes(xml), 0x1A4);
            }
            var shown = settings.Settings.Where(kv => kv.Key != "app_Key").ToDictionary(kv => kv.Key, kv => kv.Value);
            _fs.WriteAtomically(settings.WorkDirectory + "\\settings.conf.txt",
                Encoding.UTF8.GetBytes(SettingsConf.Render(shown, "Values applied to the registry for one makemkvcon run (for reference only)")), 0x1A4);

            var managed = new SortedSet<string>(_catalogKeys, StringComparer.Ordinal);
            managed.UnionWith(settings.Settings.Keys);
            if (!settings.Settings.ContainsKey("app_Key")) managed.Remove("app_Key");
            foreach (var name in managed)
            {
                var before = _registry.Get(name);
                snapshot[name] = before;
                if (settings.Settings.TryGetValue(name, out var value)) _registry.Set(name, value);
                else if (before != null) _registry.Remove(name);
            }
            return new Lease(this, snapshot, profile);
        }
        catch (Exception e) when (e is UnauthorizedAccessException or SecurityException or System.IO.IOException)
        {
            Restore(snapshot);
            Launch.Release();
            throw new BroFailure(new BroMessage(MessageCode.MakemkvRegistryFailed, Severity.Error, ("reason", JsonValue.Of(e.Message))).ToError());
        }
        catch
        {
            Restore(snapshot);
            Launch.Release();
            throw;
        }
    }

    void Restore(Dictionary<string, string?> snapshot)
    {
        foreach (var (name, value) in snapshot)
        {
            try
            {
                if (value == null) _registry.Remove(name);
                else _registry.Set(name, value);
            }
            catch (Exception e) when (e is UnauthorizedAccessException or SecurityException or System.IO.IOException) { }
        }
    }

    sealed class Lease : IIsolationLease
    {
        readonly RegistrySwapIsolation _owner;
        readonly Dictionary<string, string?> _snapshot;
        readonly string? _profile;
        readonly Timer _timer;
        int _restored;

        public Lease(RegistrySwapIsolation owner, Dictionary<string, string?> snapshot, string? profile)
        {
            _owner = owner;
            _snapshot = snapshot;
            _profile = profile;
            _timer = new Timer(_ => GiveBack(), null, owner._restoreAfter, Timeout.InfiniteTimeSpan);
        }

        public IReadOnlyDictionary<string, string> Environment() => new Dictionary<string, string>();

        public string? ProfilePath() => _profile;

        public void FirstOutput() => GiveBack();

        public void Release() => GiveBack();

        void GiveBack()
        {
            if (Interlocked.Exchange(ref _restored, 1) != 0) return;
            _timer.Dispose();
            _owner.Restore(_snapshot);
            Launch.Release();
        }
    }
}
