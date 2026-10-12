import BroDomain
import BroFoundation
import BroPorts

/// The configuration (plan §11.1, §15.1, §15.7, §24), on the executor: config.json (mode 0600), its version (kv
/// config.version, so it survives a restart) and the last good document (kv config.lastGood), and the secrets it
/// refers to. The engine owns the secrets (owner, 2026-10-12, review L3-6): clients set them through `setSecret` and
/// only ever see whether one is set. A secret comes from the system keystore, then BROMELIA_SECRET_<NAME>, then the
/// secrets folder (plan §24); when two of them disagree, config secrets.prefer / secrets.bySecret pick one, and the
/// disagreement is a problem: secret.conflict (an error) without a preference that settles it, secret.conflictPreferred
/// (a warning) with one (owner, 2026-10-12). File, keystore and store work runs on workers.
/// shared/fixtures/engine/config-service.cases.json.
@EngineActor
public final class ConfigService: ConfigSource {
    private nonisolated static let versionKey = "config.version"
    private nonisolated static let lastGoodKey = "config.lastGood"
    private nonisolated static let fileMode = 0o600
    private nonisolated static let environmentPrefix = "BROMELIA_SECRET_"

    private nonisolated let executor: EngineExecutor
    private let bus: EventBus
    private let problems: ProblemService
    private let fs: any FileSystem
    private nonisolated let keystore: any Keystore
    private nonisolated let secretFiles: any Keystore
    private let kv: any KeyValueRepository
    private let path: String
    private nonisolated let environment: [String: String]
    private var config = ConfigService.defaults()
    private var savedVersion: Int64 = 0
    private var saving = false

    /// `keystore`: where `setSecret` stores secrets (the system store, or the secrets folder when there is none).
    /// `secretFiles`: the secrets folder alone (the keystore adapter without its system store). `path`: config.json.
    /// `environment`: the process's BROMELIA_SECRET_<NAME> variables, read once by the composition root (the engine
    /// never reads the environment itself).
    public init(executor: EngineExecutor, bus: EventBus, problems: ProblemService, fs: any FileSystem, keystore: any Keystore,
                secretFiles: any Keystore, kv: any KeyValueRepository, path: String, environment: [String: String]) {
        self.executor = executor
        self.bus = bus
        self.problems = problems
        self.fs = fs
        self.keystore = keystore
        self.secretFiles = secretFiles
        self.kv = kv
        self.path = path
        self.environment = environment
    }

    /// At startup. A missing file gives the defaults; a file with errors (or not JSON) is left alone, the last good
    /// document is used (the defaults when there is none) and config.invalid{count} is raised.
    public func load() async throws(BroError) {
        let (kv, fs, path) = (self.kv, self.fs, self.path)
        let (version, bytes, lastGood) = try await executor.worker { () async throws(BroError) -> (JsonValue?, [UInt8]?, JsonValue?) in
            let v = try await kv.get(Self.versionKey)
            let b = fs.exists(path) ? try fs.read(path) : nil
            let g = try await kv.get(Self.lastGoodKey)
            return (v, b, g)
        }
        savedVersion = version?.int ?? 0
        guard let bytes else {
            config = Self.defaults()
            try await checkSecrets()
            return
        }
        var decoded: Config?
        var errors: Int
        do {
            let c = try ConfigCodec.decode(bytes)
            decoded = c
            errors = ConfigValidator.validate(c).filter { $0.severity == .error }.count
        } catch {
            errors = 1
        }
        if let decoded, errors == 0 {
            config = decoded
            let document = decoded.document
            try await executor.worker { () async throws(BroError) in try await kv.set(Self.lastGoodKey, value: document) }
            problems.clear(.configInvalid)
            try await checkSecrets()
            return
        }
        config = Self.defaults()
        if let lastGood, lastGood.members != nil, let c = try? Self.decode(lastGood) { config = c }
        problems.raise(BroMessage(.configInvalid, [("count", .integer(Int64(errors)))], severity: .error), actions: [.openSettings])
        try await checkSecrets()
    }

    /// The configuration in use (on the executor).
    public func current() -> Config { config }

    /// The saved revision clients send back with an update.
    public func version() -> Int64 { savedVersion }

    /// The document's issues; nothing is saved.
    public func validate(_ doc: JsonValue) throws(BroError) -> [Issue] { ConfigValidator.validate(try Self.decode(doc)) }

    /// Saves a document: config.versionConflict{current} unless `expectedVersion` is the current version (or while
    /// another update saves); config.rejected{count (errors), issues} when it has errors. The version and the last
    /// good document are stored before config.json, so a crash or a failed write can only make a client reload; a
    /// failure is config.saveFailed{reason} and leaves the configuration in use as it was. Publishes
    /// configChanged{version}, then checks the secrets again (the preference may have changed).
    public func update(_ doc: JsonValue, expectedVersion: Int64) async throws(BroError) -> ConfigEnvelope {
        if saving || expectedVersion != savedVersion {
            throw BroMessage(.configVersionConflict, [("current", .integer(savedVersion))], severity: .error).toError()
        }
        let next = try Self.decode(doc)
        let issues = ConfigValidator.validate(next)
        let errors = issues.filter { $0.severity == .error }.count
        if errors > 0 {
            throw BroMessage(.configRejected, [("count", .integer(Int64(errors))), ("issues", .array(issues.map { $0.toJson() }))],
                             severity: .error).toError()
        }
        saving = true
        defer { saving = false }
        let (kv, fs, path) = (self.kv, self.fs, self.path)
        let version = savedVersion + 1
        try await savingWork { () async throws(BroError) in try await kv.set(Self.versionKey, value: .integer(version)) }
        savedVersion = version
        try await savingWork { () async throws(BroError) in
            try await kv.set(Self.lastGoodKey, value: next.document)
            try fs.writeAtomically(path, bytes: ConfigCodec.encode(next), mode: Self.fileMode)
        }
        saving = false
        config = next
        problems.clear(.configInvalid)
        bus.publish(.configChanged, data: .object([("version", .integer(savedVersion))]))
        try await checkSecrets()
        return try await envelope()
    }

    private func savingWork(_ work: @escaping @Sendable () async throws(BroError) -> Void) async throws(BroError) {
        do {
            try await executor.worker(work)
        } catch {
            throw BroMessage(.configSaveFailed, [("reason", .string(error.code))], severity: .error).toError(cause: error)
        }
    }

    /// The version, the document, its issues and every secret it refers to (in document order) with whether it is
    /// set: in any of its sources; a source that fails counts as not having it.
    public func envelope() async throws(BroError) -> ConfigEnvelope {
        let config = self.config
        let version = savedVersion
        let names = Self.secretNames(config.document)
        let secrets = try await executor.worker { [self] () async throws(BroError) -> [SecretStatus] in
            names.map { SecretStatus(name: $0, set: resolve($0, config: config).value != nil) }
        }
        return ConfigEnvelope(version: version, config: config, issues: ConfigValidator.validate(config), secrets: secrets)
    }

    /// A secret's value, from the first source that has it: the system keystore, BROMELIA_SECRET_<NAME> (upper case,
    /// '.' → '_'), the secrets folder; a source `config` prefers (secrets.bySecret, else secrets.prefer) comes first when
    /// it has one. Nil when none has it; the first failure when a source failed and none has it (fs.notPrivate on a
    /// share, L3-8). Sources that disagree raise (or update) the secret's problem on the executor. Any thread but the
    /// executor: the keystore may block.
    public nonisolated func secret(_ name: String, config: Config) throws(BroError) -> String? {
        let r = resolve(name, config: config)
        _ = executor.post { [self] in report(name, r) }
        if r.value == nil, let failure = r.failure { throw failure }
        return r.value
    }

    /// Stores a secret in the keystore ('' removes it), publishes configChanged{version} (the version doesn't change)
    /// and checks the secrets again. The keystore's failure is returned.
    public func setSecret(_ name: String, value: String) async throws(BroError) {
        let keystore = self.keystore
        try await executor.worker { () async throws(BroError) in
            if value.isEmpty { try keystore.remove(name) } else { try keystore.set(name, value: value) }
        }
        bus.publish(.configChanged, data: .object([("version", .integer(savedVersion))]))
        try await checkSecrets(also: name)
    }

    /// The export bundle: the configuration with its secret references, never their values.
    public func export() -> [UInt8] { ConfigCodec.exportBundle(config) }

    /// What a secret's sources hold: the value used and where it came from, every source that has a value (in order),
    /// whether they disagree and whether the preference chose the one used.
    private struct Resolution: Sendable {
        var value: String?
        var using: SecretSource?
        var sources: [SecretSource]
        var conflict: Bool
        var preferred: Bool
        var failure: BroError?
    }

    private nonisolated func resolve(_ name: String, config: Config) -> Resolution {
        var found: [(source: SecretSource, value: String)] = []
        var failure: BroError?
        func read(_ source: SecretSource, _ store: any Keystore) {
            do {
                if let v = try store.get(name) { found.append((source, v)) }
            } catch {
                if failure == nil { failure = error }
            }
        }
        if keystore.isSystemStore() { read(.keystore, keystore) }
        if let given = fromEnvironment(name) { found.append((.environment, given)) }
        read(.file, secretFiles)
        let prefer = Self.preference(config, name)
        let chosen = found.firstIndex { $0.source == prefer } ?? (found.isEmpty ? nil : 0)
        return Resolution(value: chosen.map { found[$0].value }, using: chosen.map { found[$0].source }, sources: found.map(\.source),
                          conflict: Set(found.map(\.value)).count > 1, preferred: chosen.map { found[$0].source == prefer } ?? false,
                          failure: failure)
    }

    /// secrets.bySecret[name], else secrets.prefer; nil for none.
    private nonisolated static func preference(_ config: Config, _ name: String) -> SecretSource? {
        let secrets = config.document["secrets"]
        return (secrets?["bySecret"]?[name]?.string ?? secrets?["prefer"]?.string).flatMap(SecretSource.init(rawValue:))
    }

    private static func label(_ source: SecretSource) -> BroMessage {
        switch source {
        case .keystore: BroMessage(.secretSourceKeystore)
        case .environment: BroMessage(.secretSourceEnvironment)
        case .file: BroMessage(.secretSourceFile)
        }
    }

    /// Raises, updates or clears a secret's problem (on the executor).
    private func report(_ name: String, _ r: Resolution) {
        let scope = ProblemScope(kind: .secret, id: name)
        guard r.conflict, let using = r.using else {
            problems.clear(.secretConflict, scope: scope)
            problems.clear(.secretConflictPreferred, scope: scope)
            return
        }
        let params: [(key: String, value: JsonValue)] = [("name", .string(name)), ("using", Self.label(using).toJson()),
                                                         ("sources", .array(r.sources.map { Self.label($0).toJson() }))]
        if r.preferred {
            problems.clear(.secretConflict, scope: scope)
            problems.raise(BroMessage(.secretConflictPreferred, params, severity: .warning), scope: scope, actions: [.openSettings])
        } else {
            problems.clear(.secretConflictPreferred, scope: scope)
            problems.raise(BroMessage(.secretConflict, params, severity: .error), scope: scope, actions: [.openSettings])
        }
    }

    /// Checks every secret the configuration refers to (and `also`), and clears the problems of secrets it no longer
    /// refers to.
    private func checkSecrets(also extra: String? = nil) async throws(BroError) {
        let config = self.config
        var names = Self.secretNames(config.document)
        if let extra, !names.contains(extra) { names.append(extra) }
        let checked = names
        let results = try await executor.worker { [self] () async throws(BroError) -> [Resolution] in
            checked.map { resolve($0, config: config) }
        }
        for (name, result) in zip(names, results) { report(name, result) }
        for p in problems.problems() {
            if let scope = p.scope, scope.kind == .secret, !names.contains(scope.id) { problems.clear(p.message.code, scope: scope) }
        }
    }

    private nonisolated func fromEnvironment(_ name: String) -> String? {
        let variable = Self.environmentPrefix + String(name.uppercased().map { $0 == "." ? "_" : $0 })
        guard let value = environment[variable], !value.isEmpty else { return nil }
        return value
    }

    private nonisolated static func decode(_ doc: JsonValue) throws(BroError) -> Config {
        try ConfigCodec.decode(JsonValue.encodeCanonical(doc))
    }

    private nonisolated static func defaults() -> Config {
        // A document of {"version": 3} always decodes.
        (try? decode(.object([("version", .integer(3))])))!
    }

    /// The names of the document's SecretRefs ({"secret": name}), in document order, each once.
    private nonisolated static func secretNames(_ doc: JsonValue) -> [String] {
        var names: [String] = []
        func walk(_ v: JsonValue) {
            switch v {
            case .object(let members):
                if members.count == 1, members[0].key == "secret", case .string(let s) = members[0].value {
                    if !names.contains(s) { names.append(s) }
                } else {
                    for m in members { walk(m.value) }
                }
            case .array(let items):
                for item in items { walk(item) }
            default:
                break
            }
        }
        walk(doc)
        return names
    }
}
