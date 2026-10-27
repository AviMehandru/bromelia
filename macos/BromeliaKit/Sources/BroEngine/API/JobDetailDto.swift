import BroFoundation

/// A job with its steps, files and commands (dtos.json JobDetail: the Job's members, then these). Children are left out
/// until JobQuery can ask for a job's children (decision 171).
public struct JobDetailDto: Sendable, Equatable {
    public var job: JobDto
    public var steps: [JobStepDto]
    public var files: [JobFileDto]
    public var commands: [String]
    public var logBytes: Int64

    public init(job: JobDto, steps: [JobStepDto], files: [JobFileDto], commands: [String], logBytes: Int64) {
        self.job = job
        self.steps = steps
        self.files = files
        self.commands = commands
        self.logBytes = logBytes
    }

    /// The Job's members, then steps, files, commands and logBytes; optional members left out when absent.
    public static func json(_ detail: JobDetailDto) -> JsonValue {
        guard case var .object(m) = JobDto.json(detail.job) else { return .null }
        m.append(("steps", .array(detail.steps.map(stepJson))))
        m.append(("files", .array(detail.files.map(fileJson))))
        m.append(("commands", .array(detail.commands.map { .string($0) })))
        m.append(("logBytes", .integer(detail.logBytes)))
        return .object(m)
    }

    private static func stepJson(_ step: JobStepDto) -> JsonValue {
        var m: [(key: String, value: JsonValue)] = [("seq", .integer(Int64(step.seq))), ("kind", .string(step.kind.rawValue)),
                                                    ("state", .string(step.state.rawValue))]
        if let started = step.startedAt { m.append(("startedAt", .string(Instant.format(started)))) }
        if let finished = step.finishedAt { m.append(("finishedAt", .string(Instant.format(finished)))) }
        if let error = step.error { m.append(("error", JsonDto.error(error))) }
        if let summary = step.summary { m.append(("summary", summary.toJson())) }
        return .object(m)
    }

    private static func fileJson(_ file: JobFileDto) -> JsonValue {
        var m: [(key: String, value: JsonValue)] = [("path", .string(file.path))]
        if let size = file.size { m.append(("size", .integer(size))) }
        if let sha256 = file.sha256 { m.append(("sha256", .string(sha256))) }
        if let role = file.role { m.append(("role", .string(role))) }
        return .object(m)
    }
}
