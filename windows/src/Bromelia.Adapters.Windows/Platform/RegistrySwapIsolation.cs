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
/// sets it. The values are also written to settings.conf.txt in the run's folder, for reference.
/// The user's values are written to a snapshot file (in an owner-only folder: it can hold app_Key) before anything
/// changes, and the file is removed once every value is back. A crash, a service stop or a power cut in between leaves
/// the file: <see cref="Recover"/> (at startup) or the next Prepare puts those values back first, and a run never
/// starts while they can't be (makemkv.registryNotRestored).</summary>
public sealed class RegistrySwapIsolation : ISettingsIsolation
{
    /// <summary>One launch at a time in this process: the registry is per user, not per job.</summary>
    static readonly SemaphoreSlim Launch = new(1, 1);

    readonly IFileSystem _fs;
    readonly IMakemkvRegistry _registry;
    readonly IReadOnlyCollection<string> _catalogKeys;
    readonly string _snapshotFile;
    readonly TimeSpan _restoreAfter;

    /// <param name="catalogKeys">Every key of shared/catalog/settings-catalog.json.</param>
    /// <param name="snapshotFile">Where the user's values wait while a run's are in place
    /// (&lt;data&gt;\run\makemkv-registry.json); its folder is made owner-only.</param>
    /// <param name="restoreAfter">When the values come back if makemkvcon says nothing (15 s).</param>
    public RegistrySwapIsolation(IFileSystem fs, IMakemkvRegistry registry, IReadOnlyCollection<string> catalogKeys, string snapshotFile,
        Duration? restoreAfter = null)
    {
        _fs = fs;
        _registry = registry;
        _catalogKeys = catalogKeys;
        _snapshotFile = snapshotFile;
        _restoreAfter = TimeSpan.FromSeconds(restoreAfter?.Seconds ?? 15);
    }

    /// <summary>Puts back the values of a snapshot a crash left behind; whether there was one. Fails with
    /// makemkv.registryNotRestored when they can't all be put back (the file stays for the next try), and with
    /// makemkv.registryFailed when the file isn't a snapshot.</summary>
    public bool Recover()
    {
        Launch.Wait();
        try { return RecoverLeftover(); }
        finally { Launch.Release(); }
    }

    public IIsolationLease Prepare(MakemkvRunSettings settings)
    {
        Launch.Wait();
        var snapshot = new Dictionary<string, string?>(StringComparer.Ordinal);
        var saved = false; // nothing changes before the snapshot is saved
        try
        {
            RecoverLeftover();
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
            foreach (var name in managed) snapshot[name] = _registry.Get(name);
            SaveSnapshot(snapshot);
            saved = true;
            foreach (var name in managed)
            {
                if (settings.Settings.TryGetValue(name, out var value)) _registry.Set(name, value);
                else if (snapshot[name] != null) _registry.Remove(name);
            }
            return new Lease(this, snapshot, profile);
        }
        catch (Exception e) when (e is UnauthorizedAccessException or SecurityException or System.IO.IOException)
        {
            if (saved) Restore(snapshot);
            Launch.Release();
            throw RegistryFailed(e.Message);
        }
        catch
        {
            if (saved) Restore(snapshot);
            Launch.Release();
            throw;
        }
    }

    /// <summary>With the launch held: the leftover snapshot's values back, then the file removed; false when there was
    /// none.</summary>
    bool RecoverLeftover()
    {
        if (!_fs.Exists(_snapshotFile)) return false;
        var json = JsonValue.Parse(_fs.Read(_snapshotFile));
        if (json?["values"]?.AsObject is not { } values) throw RegistryFailed(_snapshotFile + " isn't a registry snapshot");
        var snapshot = new Dictionary<string, string?>(StringComparer.Ordinal);
        foreach (var (name, value) in values) snapshot[name] = value.AsString;
        if (!Restore(snapshot))
            throw new BroFailure(new BroMessage(MessageCode.MakemkvRegistryNotRestored, Severity.Error, ("path", JsonValue.Of(_snapshotFile))).ToError());
        return true;
    }

    /// <summary>The user's values, saved before any of them changes: written atomically into an owner-only
    /// folder. They can hold app_Key, so a folder on a network share, or one where owner-only doesn't hold, is refused
    /// (fs.notPrivate) and the run doesn't start.</summary>
    void SaveSnapshot(Dictionary<string, string?> snapshot)
    {
        if (System.IO.Path.GetDirectoryName(_snapshotFile) is { Length: > 0 } folder) OwnerOnly.SecretsFolder(folder);
        var json = JsonValue.Of(("comment", JsonValue.Of("HKCU\\Software\\MakeMKV before a makemkvcon run; Bromelia puts these back")),
            ("values", JsonValue.Of(snapshot.Select(kv => (kv.Key, JsonValue.Of(kv.Value))).ToArray())));
        _fs.WriteAtomically(_snapshotFile, JsonValue.EncodeCanonical(json), 0x180);
    }

    /// <summary>Every value of the snapshot back (null: removed); then the snapshot file is removed. False, and the file
    /// kept for the next try, when any of them couldn't be.</summary>
    bool Restore(Dictionary<string, string?> snapshot)
    {
        var ok = true;
        foreach (var (name, value) in snapshot)
        {
            try
            {
                if (value == null) _registry.Remove(name);
                else _registry.Set(name, value);
            }
            catch (Exception e) when (e is UnauthorizedAccessException or SecurityException or System.IO.IOException) { ok = false; }
        }
        if (ok && _fs.Exists(_snapshotFile))
        {
            try { _fs.Remove(_snapshotFile); }
            catch (BroFailure) { ok = false; }
        }
        return ok;
    }

    static BroFailure RegistryFailed(string reason) =>
        new(new BroMessage(MessageCode.MakemkvRegistryFailed, Severity.Error, ("reason", JsonValue.Of(reason))).ToError());

    sealed class Lease : IIsolationLease
    {
        readonly RegistrySwapIsolation _owner;
        readonly Dictionary<string, string?> _snapshot;
        readonly string? _profile;
        readonly Timer _timer;
        int _restored;
        volatile bool _notRestored;

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

        public BroMessage? Release()
        {
            GiveBack();
            return _notRestored
                ? new BroMessage(MessageCode.MakemkvRegistryNotRestored, Severity.Warning, ("path", JsonValue.Of(_owner._snapshotFile)))
                : null;
        }

        void GiveBack()
        {
            if (Interlocked.Exchange(ref _restored, 1) != 0) return;
            _timer.Dispose();
            _notRestored = !_owner.Restore(_snapshot);
            Launch.Release();
        }
    }
}
