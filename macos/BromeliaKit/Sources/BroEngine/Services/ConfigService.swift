import BroDomain
import BroFoundation
import BroPorts

/// The configuration (plan §11.1, §15.1, §15.7, §24), on the executor: config.json (mode 0600), its version (kv
/// config.version, so it survives a restart) and the last good document (kv config.lastGood), and the secrets it
/// refers to. The engine owns the secrets (owner, 2026-10-12, review L3-6): clients set them through `setSecret` and
/// only ever see whether one is set. File, keystore and store work runs on workers.
/// shared/fixtures/engine/config-service.cases.json.
@EngineActor
public final class ConfigService: ConfigSource {
    private nonisolated static let versionKey = "config.version"
    private nonisolated static let lastGoodKey = "config.lastGood"
    private nonisolated static let fileMode = 0o600
    private nonisolated static let environmentPrefix = "BROMELIA_SECRET_"

    private let executor: EngineExecutor
    private let bus: EventBus
    private let problems: ProblemService
    private let fs: any FileSystem
    private nonisolated let keystore: any Keystore
    private let kv: any KeyValueRepository
    private let path: String
    private nonisolated let environment: [String: String]
    private var config = ConfigService.defaults()
    private var savedVersion: Int64 = 0
    private var saving = false

    /// `path`: config.json. `environment`: the process's BROMELIA_SECRET_<NAME> variables, read once by the
    /// composition root (the engine never reads the environment itself).
    public init(executor: EngineExecutor, bus: EventBus, problems: ProblemService, fs: any FileSystem, keystore: any Keystore,
                kv: any KeyValueRepository, path: String, environment: [String: String]) {
        self.executor = executor
        self.bus = bus
        self.problems = problems
        self.fs = fs
        self.keystore = keystore
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
            return
        }
        config = Self.defaults()
        if let lastGood, lastGood.members != nil, let c = try? Self.decode(lastGood) { config = c }
        problems.raise(BroMessage(.configInvalid, [("count", .integer(Int64(errors)))], severity: .error), actions: [.openSettings])
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
    /// configChanged{version}.
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
    /// set: in the keystore, or given as BROMELIA_SECRET_<NAME>; a keystore failure counts as not set.
    public func envelope() async throws(BroError) -> ConfigEnvelope {
        let config = self.config
        let version = savedVersion
        let names = Self.secretNames(config.document)
        let secrets = try await executor.worker { [self] () async throws(BroError) -> [SecretStatus] in
            names.map { SecretStatus(name: $0, set: isSet($0)) }
        }
        return ConfigEnvelope(version: version, config: config, issues: ConfigValidator.validate(config), secrets: secrets)
    }

    /// A secret's value: the keystore's, else BROMELIA_SECRET_<NAME> (upper case, '.' → '_'), else nil; the keystore's
    /// failure when it fails and the environment has nothing (fs.notPrivate on a share, L3-8). Any thread but the
    /// executor: the keystore may block.
    public nonisolated func secret(_ name: String) throws(BroError) -> String? {
        var failure: BroError?
        do {
            if let value = try keystore.get(name) { return value }
        } catch {
            failure = error
        }
        if let given = fromEnvironment(name) { return given }
        if let failure { throw failure }
        return nil
    }

    /// Stores a secret in the keystore ('' removes it) and publishes configChanged{version} (the version doesn't
    /// change). The keystore's failure is returned.
    public func setSecret(_ name: String, value: String) async throws(BroError) {
        let keystore = self.keystore
        try await executor.worker { () async throws(BroError) in
            if value.isEmpty { try keystore.remove(name) } else { try keystore.set(name, value: value) }
        }
        bus.publish(.configChanged, data: .object([("version", .integer(savedVersion))]))
    }

    /// The export bundle: the configuration with its secret references, never their values.
    public func export() -> [UInt8] { ConfigCodec.exportBundle(config) }

    private nonisolated func isSet(_ name: String) -> Bool {
        if (try? keystore.get(name)) != nil { return true }
        return fromEnvironment(name) != nil
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
