using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.TestKit;

/// <summary>An isolation that records what happens to its leases.</summary>
public sealed class RecordingIsolation : ISettingsIsolation
{
    public readonly List<string> Log = new();
    /// <summary>What release reports.</summary>
    public BroMessage? ReleaseProblem;
    /// <summary>The leases' warning.</summary>
    public BroMessage? Warning;

    public IIsolationLease Prepare(MakemkvRunSettings settings)
    {
        Log.Add("prepare");
        return new Lease(this, settings);
    }

    public IReadOnlyList<BroMessage> Recover(IReadOnlyList<string> workDirectories)
    {
        Log.Add("recover " + string.Join(" ", workDirectories));
        return Array.Empty<BroMessage>();
    }

    sealed class Lease : IIsolationLease
    {
        readonly RecordingIsolation _owner;
        readonly MakemkvRunSettings _settings;
        public Lease(RecordingIsolation owner, MakemkvRunSettings settings) { _owner = owner; _settings = settings; }
        public IReadOnlyDictionary<string, string> Environment() => new Dictionary<string, string> { ["HOME"] = _settings.WorkDirectory };
        public string? ProfilePath() => _settings.ProfileXml is null ? null : _settings.WorkDirectory + "/profile.mmcp.xml";
        public BroMessage? Warning() => _owner.Warning;
        public void FirstOutput() => _owner.Log.Add("firstOutput");
        public BroMessage? Release()
        {
            _owner.Log.Add("release");
            return _owner.ReleaseProblem;
        }
    }
}
