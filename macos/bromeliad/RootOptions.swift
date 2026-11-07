/// What the graph is built for. There are no defaults: the platform's data folder holds the real config.json and
/// secrets, so a test always passes its own.
public struct RootOptions: Sendable {
    /// The data folder: bromelia.db, jobs, secrets, logs (made when it isn't there).
    public var dataDir: String
    /// config.json; nil: <data>/config.json.
    public var configPath: String?
    /// The user's home: MakeMKV's folder (~/Library/MakeMKV) and ~ in paths.
    public var home: String
    /// Whether secrets go to the Keychain (false in tests: the secrets folder only).
    public var systemKeystore: Bool
    /// Whether the tools are looked for where they are usually installed and on PATH (false in tests: no tool is found,
    /// so nothing runs makemkvcon).
    public var systemTools: Bool
    /// Whether the optical drives are used (false in tests: no device monitor, no drive control, no MakeMKV drive scan, so
    /// nothing asks a drive anything; real hosts pass true).
    public var drives: Bool
    /// The process's environment (the host passes it): ConfigService gets its BROMELIA_SECRET_* entries, the tool locator
    /// its PATH.
    public var environment: [String: String]

    public init(dataDir: String, configPath: String?, home: String, systemKeystore: Bool, systemTools: Bool, drives: Bool,
                environment: [String: String]) {
        self.dataDir = dataDir
        self.configPath = configPath
        self.home = home
        self.systemKeystore = systemKeystore
        self.systemTools = systemTools
        self.drives = drives
        self.environment = environment
    }
}
