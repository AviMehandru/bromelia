/// What the host knows and the engine can't ask the system for (the composition root fills it): the app's version, the
/// process id, the platform (macos, windows or linux), the data folder, the processor count (limits.cpu 0 = cores / 2).
public struct EngineFacts: Sendable, Equatable {
    public var version: String
    public var pid: Int64
    public var platform: String
    public var dataDir: String
    public var cores: Int

    public init(version: String, pid: Int64, platform: String, dataDir: String, cores: Int) {
        self.version = version
        self.pid = pid
        self.platform = platform
        self.dataDir = dataDir
        self.cores = cores
    }
}
