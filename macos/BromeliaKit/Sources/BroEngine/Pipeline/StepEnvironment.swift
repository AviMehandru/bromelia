import BroPorts

/// The parts of a StepContext every job shares; the composition root builds it. `dataDirectory`: the engine's data
/// folder (a job's folder is <data>/jobs/<id>). `messages`: the host's renderer (MessageTextRenderer over the configured
/// language's texts). `appVersion`: the app's version, written into archive records.
public struct StepEnvironment: Sendable {
    public var tools: ToolSet
    public var fs: any FileSystem
    public var clock: any Clock
    public var drives: any DriveControl
    public var library: any LibraryPort
    public var catalog: any CatalogQueries
    public var config: any ConfigSource
    public var driveLookup: any DriveLookup
    public var makemkvSetup: any MakemkvSetup
    public var dataDirectory: String
    public var messages: any MessageRenderer
    public var appVersion: String

    public init(tools: ToolSet, fs: any FileSystem, clock: any Clock, drives: any DriveControl, library: any LibraryPort,
                catalog: any CatalogQueries, config: any ConfigSource, driveLookup: any DriveLookup, makemkvSetup: any MakemkvSetup,
                dataDirectory: String, messages: any MessageRenderer, appVersion: String) {
        self.tools = tools
        self.fs = fs
        self.clock = clock
        self.drives = drives
        self.library = library
        self.catalog = catalog
        self.config = config
        self.driveLookup = driveLookup
        self.makemkvSetup = makemkvSetup
        self.dataDirectory = dataDirectory
        self.messages = messages
        self.appVersion = appVersion
    }
}
