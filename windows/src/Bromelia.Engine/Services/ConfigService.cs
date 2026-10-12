using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>
/// The configuration (plan §11.1, §15.1, §15.7, §24), on the executor: config.json (mode 0600), its version (kv
/// config.version, so it survives a restart) and the last good document (kv config.lastGood), and the secrets it
/// refers to. The engine owns the secrets (owner, 2026-10-12, review L3-6): clients set them through
/// <see cref="SetSecret"/> and only ever see whether one is set. File, keystore and store work runs on workers.
/// shared/fixtures/engine/config-service.cases.json.
/// </summary>
public sealed class ConfigService : IConfigSource
{
    const string VersionKey = "config.version";
    const string LastGoodKey = "config.lastGood";
    const int FileMode = 0x180; // 0600
    const string EnvironmentPrefix = "BROMELIA_SECRET_";

    readonly EngineExecutor _executor;
    readonly EventBus _bus;
    readonly ProblemService _problems;
    readonly IFileSystem _fs;
    readonly IKeystore _keystore;
    readonly IKeyValueRepository _kv;
    readonly string _path;
    readonly IReadOnlyDictionary<string, string> _environment;
    Config _config = Defaults();
    long _version;
    bool _saving;

    /// <param name="path">config.json.</param>
    /// <param name="environment">The process's BROMELIA_SECRET_&lt;NAME&gt; variables, read once by the composition
    /// root (the engine never reads the environment itself).</param>
    public ConfigService(EngineExecutor executor, EventBus bus, ProblemService problems, IFileSystem fs, IKeystore keystore,
        IKeyValueRepository kv, string path, IReadOnlyDictionary<string, string> environment)
    {
        _executor = executor;
        _bus = bus;
        _problems = problems;
        _fs = fs;
        _keystore = keystore;
        _kv = kv;
        _path = path;
        _environment = environment;
    }

    /// <summary>At startup. A missing file gives the defaults; a file with errors (or not JSON) is left alone, the last
    /// good document is used (the defaults when there is none) and config.invalid{count} is raised.</summary>
    public async Task Load()
    {
        var (version, bytes, lastGood) = await _executor.Worker(async () =>
        {
            var v = await _kv.Get(VersionKey).ConfigureAwait(false);
            var b = _fs.Exists(_path) ? _fs.Read(_path) : null;
            var g = await _kv.Get(LastGoodKey).ConfigureAwait(false);
            return (v, b, g);
        });
        _version = version?.AsInteger ?? 0;
        if (bytes == null)
        {
            _config = Defaults();
            return;
        }
        Config? config = null;
        int errors;
        try
        {
            config = ConfigCodec.Decode(bytes);
            errors = ConfigValidator.Validate(config).Count(i => i.Severity == Severity.Error);
        }
        catch (BroFailure)
        {
            errors = 1;
        }
        if (config != null && errors == 0)
        {
            _config = config;
            await _executor.Worker(() => _kv.Set(LastGoodKey, config.Document));
            _problems.Clear(MessageCode.ConfigInvalid);
            return;
        }
        _config = Defaults();
        if (lastGood is JsonValue.Object)
        {
            try { _config = Decode(lastGood); }
            catch (BroFailure) { }
        }
        _problems.Raise(new BroMessage(MessageCode.ConfigInvalid, Severity.Error, ("count", JsonValue.Of(errors))), null,
            new[] { ProblemAction.OpenSettings });
    }

    /// <summary>The configuration in use (on the executor).</summary>
    public Config Current() => _config;

    /// <summary>The saved revision clients send back with an update.</summary>
    public long Version() => _version;

    /// <summary>The document's issues; nothing is saved.</summary>
    public IReadOnlyList<Issue> Validate(JsonValue doc) => ConfigValidator.Validate(Decode(doc));

    /// <summary>Saves a document: config.versionConflict{current} unless <paramref name="expectedVersion"/> is the
    /// current version (or while another update saves); config.rejected{count (errors), issues} when it has errors.
    /// The version and the last good document are stored before config.json, so a crash or a failed write can only
    /// make a client reload; a failure is config.saveFailed{reason} and leaves the configuration in use as it was.
    /// Publishes configChanged{version}.</summary>
    public async Task<ConfigEnvelope> Update(JsonValue doc, long expectedVersion)
    {
        if (_saving || expectedVersion != _version)
            throw new BroFailure(new BroMessage(MessageCode.ConfigVersionConflict, Severity.Error, ("current", JsonValue.Of(_version))).ToError());
        var config = Decode(doc);
        var issues = ConfigValidator.Validate(config);
        var errors = issues.Count(i => i.Severity == Severity.Error);
        if (errors > 0)
            throw new BroFailure(new BroMessage(MessageCode.ConfigRejected, Severity.Error, ("count", JsonValue.Of(errors)),
                ("issues", JsonValue.Of(issues.Select(i => i.ToJson())))).ToError());
        _saving = true;
        try
        {
            var next = _version + 1;
            await Saving(() => _kv.Set(VersionKey, JsonValue.Of(next)));
            _version = next;
            await Saving(async () =>
            {
                await _kv.Set(LastGoodKey, config.Document).ConfigureAwait(false);
                _fs.WriteAtomically(_path, ConfigCodec.Encode(config), FileMode);
            });
        }
        finally
        {
            _saving = false;
        }
        _config = config;
        _problems.Clear(MessageCode.ConfigInvalid);
        _bus.Publish(EngineEventType.ConfigChanged, JsonValue.Of(("version", JsonValue.Of(_version))));
        return await Envelope();
    }

    async Task Saving(System.Func<Task> work)
    {
        try
        {
            await _executor.Worker(work);
        }
        catch (BroFailure f)
        {
            throw new BroFailure(new BroMessage(MessageCode.ConfigSaveFailed, Severity.Error, ("reason", JsonValue.Of(f.Error.Code))).ToError(f.Error));
        }
    }

    /// <summary>The version, the document, its issues and every secret it refers to (in document order) with whether
    /// it is set: in the keystore, or given as BROMELIA_SECRET_&lt;NAME&gt;; a keystore failure counts as not set.</summary>
    public async Task<ConfigEnvelope> Envelope()
    {
        var config = _config;
        var version = _version;
        var names = SecretNames(config.Document);
        var secrets = await _executor.Worker(() => names.Select(n => new SecretStatus(n, IsSet(n))).ToList());
        return new ConfigEnvelope(version, config, ConfigValidator.Validate(config), secrets);
    }

    /// <summary>A secret's value: the keystore's, else BROMELIA_SECRET_&lt;NAME&gt; (upper case, '.' → '_'), else
    /// null; the keystore's failure when it fails and the environment has nothing (fs.notPrivate on a share, L3-8).
    /// Any thread but the executor: the keystore may block.</summary>
    public string? Secret(string name)
    {
        BroFailure? failure = null;
        try
        {
            if (_keystore.Get(name) is { } value) return value;
        }
        catch (BroFailure f)
        {
            failure = f;
        }
        if (FromEnvironment(name) is { } given) return given;
        if (failure != null) throw failure;
        return null;
    }

    /// <summary>Stores a secret in the keystore ('' removes it) and publishes configChanged{version} (the version
    /// doesn't change). The keystore's failure is returned.</summary>
    public async Task SetSecret(string name, string value)
    {
        await _executor.Worker(() =>
        {
            if (value.Length == 0) _keystore.Remove(name);
            else _keystore.Set(name, value);
            return true;
        });
        _bus.Publish(EngineEventType.ConfigChanged, JsonValue.Of(("version", JsonValue.Of(_version))));
    }

    /// <summary>The export bundle: the configuration with its secret references, never their values.</summary>
    public byte[] Export() => ConfigCodec.ExportBundle(_config);

    bool IsSet(string name)
    {
        try
        {
            if (_keystore.Get(name) != null) return true;
        }
        catch (BroFailure)
        {
        }
        return FromEnvironment(name) != null;
    }

    string? FromEnvironment(string name) =>
        _environment.TryGetValue(EnvironmentPrefix + name.ToUpperInvariant().Replace('.', '_'), out var v) && v.Length > 0 ? v : null;

    static Config Decode(JsonValue doc) => ConfigCodec.Decode(JsonValue.EncodeCanonical(doc));

    static Config Defaults() => Decode(JsonValue.Of(("version", JsonValue.Of(3))));

    /// <summary>The names of the document's SecretRefs ({"secret": name}), in document order, each once.</summary>
    static List<string> SecretNames(JsonValue doc)
    {
        var names = new List<string>();
        void Walk(JsonValue v)
        {
            switch (v)
            {
                case JsonValue.Object { Members: [{ Key: "secret", Value: JsonValue.String s }] }:
                    if (!names.Contains(s.Value)) names.Add(s.Value);
                    break;
                case JsonValue.Object o:
                    foreach (var m in o.Members) Walk(m.Value);
                    break;
                case JsonValue.Array a:
                    foreach (var item in a.Items) Walk(item);
                    break;
            }
        }
        Walk(doc);
        return names;
    }
}
