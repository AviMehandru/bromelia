using System.Text.RegularExpressions;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Ports.Tests;

/// <summary>A keystore in memory, through the port.</summary>
internal sealed class MemoryKeystore : IKeystore
{
    private readonly Dictionary<string, string> _secrets = new();

    public string? Get(string name) => _secrets.TryGetValue(name, out var v) ? v : null;

    public void Set(string name, string value) => _secrets[name] = value;

    public void Remove(string name) => _secrets.Remove(name);
}

public class PortsTests
{
    /// <summary>The values a column's CHECK (column IN (…)) allows in shared/schema/db/0001_init.sql.</summary>
    internal static List<string> Allowed(string table, string column)
    {
        var sql = Text("../schema/db/0001_init.sql");
        var block = Regex.Match(sql, @"CREATE TABLE " + table + @" \((.*?)\n\) STRICT;", RegexOptions.Singleline).Groups[1].Value;
        var list = Regex.Match(block, @"CHECK \(" + column + @"(?: IS NULL OR " + column + @")? IN \(([^)]*)\)\)").Groups[1].Value;
        return Regex.Matches(list, "'([^']*)'").Select(x => x.Groups[1].Value).ToList();
    }

    private static List<string> Wire<T>() where T : struct, Enum => Enum.GetValues<T>().Select(v => EnumWire.Name(v)).ToList();

    [Fact]
    public void EnumsMatchTheDatabase()
    {
        Assert.Equal(Allowed("archive_units", "state"), Wire<UnitState>());
        Assert.Equal(Allowed("archive_units", "status"), Wire<UnitStatus>());
        Assert.Equal(Allowed("replicas", "state"), Wire<ReplicaState>());
        Assert.Equal(Allowed("jobs", "state"), Wire<JobState>());
        Assert.Equal(Allowed("jobs", "outcome"), Wire<Outcome>());
        Assert.Equal(Allowed("jobs", "queue"), Wire<Queue>());
        Assert.Equal(Allowed("job_steps", "state"), Wire<StepState>());
        Assert.Equal(Allowed("checks", "result"), Wire<CheckResult>());
        Assert.Equal(Allowed("works", "kind"), Wire<MediaKind>());
    }

    [Fact]
    public void APortCanBeImplemented()
    {
        IKeystore keystore = new MemoryKeystore();
        Assert.Null(keystore.Get("metadata.apiKey"));
        keystore.Set("metadata.apiKey", "k");
        Assert.Equal("k", keystore.Get("metadata.apiKey"));
        keystore.Remove("metadata.apiKey");
        Assert.Null(keystore.Get("metadata.apiKey"));
    }
}
