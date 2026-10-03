import BroFoundation

/// A post-processing step of the configuration (config-3.json's Step), with its defaults.
public struct StepDefinition: Sendable, Equatable {
    public var id: String
    public var kind: StepType
    public var name = ""
    public var enabled = true
    public var runOn: RunOn = .success
    public var background = false
    public var affectsOutcome = false
    public var timeoutSeconds = 0
    public var command = CommandSettings()
    public var handbrake = HandBrakeSettings()

    public init(id: String, kind: StepType) {
        self.id = id
        self.kind = kind
        self.background = kind == .handbrake
    }

    /// A step from its JSON, with the schema's defaults for what it leaves out (background: true for handbrake,
    /// false for command).
    public static func decode(_ json: JsonValue) -> StepDefinition {
        var issues: [Issue] = []
        let node = SchemaWalker.def("Step")
        let j = SchemaWalker.normalize(json, node, "", fill: true, &issues)
        var s = StepDefinition(id: j["id"]?.string ?? "", kind: StepType(rawValue: j["kind"]?.string ?? "") ?? .command)
        s.name = j["name"]?.string ?? ""
        s.enabled = j["enabled"]?.bool ?? true
        s.runOn = RunOn(rawValue: j["runOn"]?.string ?? "") ?? .success
        s.background = j["background"]?.bool ?? (s.kind == .handbrake)
        s.affectsOutcome = j["affectsOutcome"]?.bool ?? false
        s.timeoutSeconds = Int(j["timeoutSeconds"]?.int ?? 0)
        let c = SchemaWalker.normalize(j["command"] ?? .object([]), node["properties"]!["command"]!, "", fill: true, &issues)
        s.command.executable = c["executable"]?.string ?? ""
        s.command.interpreter = c["interpreter"]?.string ?? ""
        s.command.arguments = c["arguments"]?.string ?? ""
        s.command.workingDirectory = c["workingDirectory"]?.string ?? ""
        s.command.perFile = c["perFile"]?.bool ?? false
        s.command.environment = Dictionary((c["environment"]?.members ?? []).map { ($0.key, $0.value.string ?? "") }, uniquingKeysWith: { $1 })
        let h = SchemaWalker.normalize(j["handbrake"] ?? .object([]), node["properties"]!["handbrake"]!, "", fill: true, &issues)
        s.handbrake.executable = h["executable"]?.string ?? ""
        s.handbrake.preset = h["preset"]?.string ?? ""
        s.handbrake.presetFile = h["presetFile"]?.string ?? ""
        s.handbrake.outputPath = h["outputPath"]?.string ?? ""
        s.handbrake.extraArguments = h["extraArguments"]?.string ?? ""
        return s
    }
}
