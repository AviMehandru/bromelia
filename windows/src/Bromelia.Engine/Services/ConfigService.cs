using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Engine;

/// <summary>
/// The configuration (plan §11.1, §15.1, §15.7, §24), on the executor: config.json (mode 0600), its version (kv
/// config.version, so it survives a restart) and the last good document (kv config.lastGood), and the secrets it refers
/// to. The engine owns the secrets: clients set them through <see cref="SetSecret"/> and only ever see whether one is
/// set. A secret comes from the system keystore, then BROMELIA_SECRET_&lt;NAME&gt;, then the secrets folder (plan §24);
/// when two of them disagree, config secrets.prefer / secrets.bySecret pick one, and the disagreement is a problem:
/// secret.conflict (an error) without a preference that settles it, secret.conflictPreferred (a warning) with one. A
/// source that fails is the problem secret.unreadable until it reads again. File, keystore and store work runs on
/// workers. shared/fixtures/engine/config-service.cases.json.
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
    readonly IKeystore _secretFiles;
    readonly IKeyValueRepository _kv;
    readonly string _path;
    readonly IReadOnlyDictionary<string, string> _environment;
    Config _config = Defaults();
    long _version;
    bool _saving;

    /// <param name="keystore">Where <see cref="SetSecret"/> stores secrets: the system store, or the secrets folder when
    /// there is none.</param>
    /// <param name="secretFiles">The secrets folder alone (the keystore adapter without its system store).</param>
    /// <param name="path">config.json.</param>
    /// <param name="environment">The process's BROMELIA_SECRET_&lt;NAME&gt; variables, read once by the composition
    /// root (the engine never reads the environment itself).</param>
    public ConfigService(EngineExecutor executor, EventBus bus, ProblemService problems, IFileSystem fs, IKeystore keystore,
        IKeystore secretFiles, IKeyValueRepository kv, string path, IReadOnlyDictionary<string, string> environment)
    {
        _executor = executor;
        _bus = bus;
        _problems = problems;
        _fs = fs;
        _keystore = keystore;
        _secretFiles = secretFiles;
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
            await CheckSecrets();
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
            await CheckSecrets();
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
        await CheckSecrets();
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
    /// Publishes configChanged{version}, then checks the secrets again (the preference may have changed).</summary>
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
        await CheckSecrets();
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
    /// it is set: in any of its sources; a source that fails counts as not having it.</summary>
    public async Task<ConfigEnvelope> Envelope()
    {
        var config = _config;
        var version = _version;
        var names = SecretNames(config.Document);
        var secrets = await _executor.Worker(() => names.Select(n => new SecretStatus(n, Resolve(n, config).Value != null)).ToList());
        return new ConfigEnvelope(version, config, ConfigValidator.Validate(config), secrets);
    }

    /// <summary>A secret's value, from the first source that has it: the system keystore, BROMELIA_SECRET_&lt;NAME&gt;
    /// (upper case, '.' → '_'), the secrets folder; a source <paramref name="config"/> prefers (secrets.bySecret, else
    /// secrets.prefer) comes first when it has one. Null when none has it; the first failure when a source failed and
    /// none has it (fs.notPrivate on a share). Sources that disagree raise (or update) the secret's problem on the
    /// executor. Any thread but the executor: the keystore may block.</summary>
    public string? Secret(string name, Config config)
    {
        var r = Resolve(name, config);
        _executor.Post(() => Report(name, r));
        if (r.Value == null && r.Failure != null) throw r.Failure;
        return r.Value;
    }

    /// <summary>Stores a secret in the keystore ('' removes it), publishes configChanged{version} (the version
    /// doesn't change) and checks the secrets again. The keystore's failure is returned.</summary>
    public async Task SetSecret(string name, string value)
    {
        await _executor.Worker(() =>
        {
            if (value.Length == 0) _keystore.Remove(name);
            else _keystore.Set(name, value);
            return true;
        });
        _bus.Publish(EngineEventType.ConfigChanged, JsonValue.Of(("version", JsonValue.Of(_version))));
        await CheckSecrets(name);
    }

    /// <summary>The export bundle: the configuration with its secret references, never their values.</summary>
    public byte[] Export() => ConfigCodec.ExportBundle(_config);

    /// <summary>What a secret's sources hold: the value used and where it came from, every source that has a value (in
    /// order), whether they disagree and whether the preference chose the one used.</summary>
    sealed record Resolution(string? Value, SecretSource? Using, IReadOnlyList<SecretSource> Sources, bool Conflict, bool Preferred,
        BroFailure? Failure);

    Resolution Resolve(string name, Config config)
    {
        var found = new List<(SecretSource Source, string Value)>();
        BroFailure? failure = null;
        void Read(SecretSource source, IKeystore store)
        {
            try
            {
                if (store.Get(name) is { } v) found.Add((source, v));
            }
            catch (BroFailure f)
            {
                failure ??= f;
            }
        }
        if (_keystore.IsSystemStore()) Read(SecretSource.Keystore, _keystore);
        if (FromEnvironment(name) is { } given) found.Add((SecretSource.Environment, given));
        Read(SecretSource.File, _secretFiles);
        var prefer = Preference(config, name);
        var chosen = found.FindIndex(f => f.Source == prefer);
        if (chosen < 0) chosen = found.Count > 0 ? 0 : -1;
        return new Resolution(chosen < 0 ? null : found[chosen].Value, chosen < 0 ? null : found[chosen].Source,
            found.Select(f => f.Source).ToList(), found.Select(f => f.Value).Distinct().Count() > 1,
            chosen >= 0 && found[chosen].Source == prefer, failure);
    }

    /// <summary>secrets.bySecret[name], else secrets.prefer; null for none.</summary>
    static SecretSource? Preference(Config config, string name)
    {
        var secrets = config.Document["secrets"];
        return EnumWire.Parse<SecretSource>(secrets?["bySecret"]?[name]?.AsString ?? secrets?["prefer"]?.AsString);
    }

    static BroMessage SourceLabel(SecretSource source) => new(source switch
    {
        SecretSource.Keystore => MessageCode.SecretSourceKeystore,
        SecretSource.Environment => MessageCode.SecretSourceEnvironment,
        _ => MessageCode.SecretSourceFile,
    });

    /// <summary>Raises, updates or clears a secret's problems (on the executor): secret.unreadable{name, reason} while a
    /// source fails (a warning when another source has the secret, an error when none has), then its conflict.</summary>
    void Report(string name, Resolution r)
    {
        var scope = new ProblemScope(ProblemScopeKind.Secret, name);
        if (r.Failure is { } failure)
            _problems.Raise(new BroMessage(MessageCode.SecretUnreadable, r.Value != null ? Severity.Warning : Severity.Error, ("name", JsonValue.Of(name)),
                ("reason", JsonValue.Of(failure.Error.Params["reason"]?.AsString ?? failure.Error.Code))), scope, new[] { ProblemAction.OpenSettings });
        else _problems.Clear(MessageCode.SecretUnreadable, scope);
        if (!r.Conflict)
        {
            _problems.Clear(MessageCode.SecretConflict, scope);
            _problems.Clear(MessageCode.SecretConflictPreferred, scope);
            return;
        }
        var parameters = new (string, JsonValue)[]
        {
            ("name", JsonValue.Of(name)), ("using", SourceLabel(r.Using!.Value).ToJson()),
            ("sources", JsonValue.Of(r.Sources.Select(s => SourceLabel(s).ToJson()))),
        };
        var actions = new[] { ProblemAction.OpenSettings };
        if (r.Preferred)
        {
            _problems.Clear(MessageCode.SecretConflict, scope);
            _problems.Raise(new BroMessage(MessageCode.SecretConflictPreferred, Severity.Warning, parameters), scope, actions);
        }
        else
        {
            _problems.Clear(MessageCode.SecretConflictPreferred, scope);
            _problems.Raise(new BroMessage(MessageCode.SecretConflict, Severity.Error, parameters), scope, actions);
        }
    }

    /// <summary>Checks every secret the configuration refers to (and <paramref name="extra"/>), and clears the problems
    /// of secrets it no longer refers to.</summary>
    async Task CheckSecrets(string? extra = null)
    {
        var config = _config;
        var names = SecretNames(config.Document);
        if (extra != null && !names.Contains(extra)) names.Add(extra);
        var results = await _executor.Worker(() => names.Select(n => (Name: n, Result: Resolve(n, config))).ToList());
        foreach (var (name, result) in results) Report(name, result);
        foreach (var p in _problems.Problems())
            if (p.Scope is { Kind: ProblemScopeKind.Secret } scope && !names.Contains(scope.Id))
                _problems.Clear(p.Message.Code, scope);
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
