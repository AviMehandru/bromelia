import BroDomain
import BroFoundation
import BroPorts

/// MakeMKV as the engine sets it up (plan §11.1), as far as phase 3 needs it: what each makemkvcon run gets
/// (`MakemkvSetup`). The settings are SettingsLayers.merge of the configuration's makemkv.settings and the profile's
/// (the drive's own settings are already in it), with the registration key (makemkv.registrationKey through
/// ConfigService.secret), else the key MakeMKV is registered with (app_Key in the user's settings.conf, on macOS and
/// Linux, where a run reads only its own; the registry keeps it on Windows), and the user's MakeMKV data folder. The
/// profile file is the generated one (ProfileXml.render) or the custom file, the switches come from the profile's
/// makemkv, and the run lives in the job's home folder with its transcript in the job's makemkv.txt. Version,
/// registration state and the beta key are phase 5. Every call is safe from any thread; it reads the user's
/// settings.conf, so not on the executor. shared/fixtures/engine/makemkv-service.cases.json.
@EngineActor
public final class MakemkvService: MakemkvSetup {
    private nonisolated let fs: any FileSystem
    private nonisolated let config: ConfigService
    private nonisolated let dataDir: String
    private nonisolated let userSettings: String?
    private nonisolated let home: String

    /// `dataDir`: the user's MakeMKV data folder, for app_DataDir ("" on Windows: not set). `userSettings`: the user's
    /// settings.conf, where MakeMKV keeps its key (nil on Windows). `home`: the user's home folder, for a leading ~ in a
    /// custom profile path or app_DataDir.
    public init(fs: any FileSystem, config: ConfigService, dataDir: String, userSettings: String?, home: String) {
        self.fs = fs
        self.config = config
        self.dataDir = dataDir
        self.userSettings = userSettings
        self.home = home
    }

    /// What one run of `profile` gets, from the job's configuration; a failure to read the key (the secret's, or the
    /// user's settings.conf) is the call's.
    public nonisolated func invocation(_ config: Config, profile: EffectiveProfile, selectionOverride: String?,
                                       jobDirectory: String) throws(BroError) -> MakemkvInvocation {
        let makemkv = profile.profile["makemkv"]
        let file = makemkv?["profile"]
        let mode = file?["mode"]?.string ?? "makemkvDefault"
        let generated = file?["generated"] ?? .object([])
        let rule = generated["selectionRule"]?.string ?? ""
        var xml: String?
        var profilePath: String?
        var selection = selectionOverride
        if mode == "generated" {
            xml = ProfileXml.render(selectionOverride.map { Self.with(generated, "selectionRule", $0) } ?? generated)
            if selection == nil && !rule.allSatisfy(\.isWhitespace) { selection = rule }
        } else if mode == "customFile" {
            let custom = expand(file?["customPath"]?.string ?? "")
            if !custom.isEmpty { profilePath = custom }
        }

        var settings = SettingsLayers.merge(Self.strings(config.document["makemkv"]?["settings"]), profile: Self.strings(makemkv?["settings"]),
                                            drive: [:], registrationKey: try key(config), selectionOverride: selection, dataDir: dataDir)
        if let d = settings["app_DataDir"] { settings["app_DataDir"] = expand(d) }

        let options = MakemkvOptions(profilePath: profilePath, minLengthSeconds: makemkv?["minLengthSeconds"]?.int.map { Int($0) },
                                     cacheMB: makemkv?["cacheMB"]?.int.map { Int($0) }, directIO: makemkv?["directIO"]?.bool,
                                     extraArguments: makemkv?["extraArguments"]?.string ?? "")
        let stall = config.document["limits"]?["stallTimeoutMinutes"]?.int ?? 0
        return MakemkvInvocation(settings: MakemkvRunSettings(settings: settings, profileXml: xml, dataDir: dataDir, workDirectory: jobDirectory + "/home"),
                                 options: options, stallTimeout: stall > 0 ? Duration(seconds: Double(stall * 60)) : nil,
                                 transcript: jobDirectory + "/makemkv.txt")
    }

    /// makemkv.registrationKey's secret when it has a value, else app_Key in the user's settings.conf, else none.
    private nonisolated func key(_ config: Config) throws(BroError) -> String? {
        if let name = config.document["makemkv"]?["registrationKey"]?["secret"]?.string,
           let key = try self.config.secret(name, config: config), !key.isEmpty {
            return key
        }
        guard let userSettings, fs.exists(userSettings) else { return nil }
        let installed = SettingsConf.parse(String(decoding: try fs.read(userSettings), as: UTF8.self))["app_Key"] ?? ""
        return installed.isEmpty ? nil : installed
    }

    /// ~ and ~/… under the user's home; anything else as it is.
    private nonisolated func expand(_ path: String) -> String {
        if path == "~" { return home }
        return path.hasPrefix("~/") ? home + path.dropFirst() : path
    }

    private nonisolated static func strings(_ map: JsonValue?) -> [String: String] {
        var o: [String: String] = [:]
        for (k, v) in map?.members ?? [] { o[k] = v.string ?? "" }
        return o
    }

    private nonisolated static func with(_ obj: JsonValue, _ key: String, _ value: String) -> JsonValue {
        .object((obj.members ?? []).filter { $0.key != key } + [(key: key, value: .string(value))])
    }
}
