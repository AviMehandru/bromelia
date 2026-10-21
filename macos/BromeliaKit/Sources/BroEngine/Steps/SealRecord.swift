import BroDomain
import BroFoundation
import BroPorts

/// The archive record SealStep writes (bromelia-<unit8>.json, shared/schema/archive-record-3.json), from what the job's
/// steps found. Messages are kept as codes with parameters and as text in the configured language.
public enum SealRecord {
    /// A version 3 record. status: errors when MakeMKV reported read errors, else success. disc: the listing Probe read and
    /// the identity Plan resolved. acquisition: the mode, "Backup" for a backup else "Rip", the job's source as the request
    /// gives it (drive:<id>, iso:<path>, folder:<path>, file:<path>), the drive, the profile, MakeMKV's version line and the
    /// app's. titles: each title Acquire ripped that the listing has, with its read errors. files: `files`, as SHA256SUMS
    /// lists them. attempts: this job's. counts: the warnings and errors in the job's log (`log`). problems: each read error
    /// (makemkv.message), then rip.readErrors. logs: the log files copied into the folder.
    public static func build(_ job: JobSnapshot, plan: PlanOutput, acquired: AcquireOutput, unitId: Id, files: [UnitFile], log: [LogLineDto],
                             logs: [String], finishedAt: Instant, appVersion: String, messages: any MessageRenderer) -> JsonValue {
        let probe = JobSnapshot.output(job, .probe).flatMap { $0.members != nil ? ProbeOutput.decode($0) : nil }
        let listing = probe?.listing
        let identity = plan.identity
        let source = DiscRequest.decode(job.request)?.source
        var driveId: String?
        if case .drive(let id)? = source { driveId = id }
        let readErrors = AcquireOutput.readErrorCount(acquired)
        func n(_ v: Int?) -> JsonValue { v.map { .integer(Int64($0)) } ?? .null }
        func s(_ v: String?) -> JsonValue { v.map { .string($0) } ?? .null }
        let started = Instant.format(job.startedAt ?? finishedAt)

        let label = if let listing, !listing.name.isEmpty { listing.name } else if let source { StepSources.label(source, nil) } else { "" }
        let disc: JsonValue = .object([
            ("label", .string(label)), ("volumeName", .string(listing?.volumeName ?? "")), ("type", .string(listing?.typeText ?? "")),
            ("format", .string(identity.format.rawValue)), ("formatCode", .string(identity.formatCode.text)), ("encrypted", .bool(identity.encrypted)),
            ("season", n(identity.label.season)), ("part", n(identity.label.part)), ("volume", n(identity.label.volume)),
            ("disc", n(identity.label.disc)), ("set", .string(LabelParser.setDescription(identity.label))),
            ("fingerprint", s(probe?.fingerprint)), ("libreDrive", s(probe?.libreDrive)),
        ])

        var acquisition: [(key: String, value: JsonValue)] = [
            ("mode", .string(plan.mode.rawValue)), ("rip", .string(plan.mode == .backup || plan.mode == .backupDecrypted ? "Backup" : "Rip")),
            ("source", .string(sourceText(source))),
        ]
        if let driveId { acquisition.append(("drive", .object([("id", .string(driveId))]))) }
        acquisition.append(("profile", .string(plan.profileId)))
        acquisition.append(("makemkv", .string(probe?.makemkvVersion ?? "")))
        acquisition.append(("bromelia", .string(appVersion)))
        acquisition.append(("tools", .object([])))

        let titles: [JsonValue] = acquired.titles.compactMap { t in
            guard let listed = listing?.titles.first(where: { $0.index == t.index }) else { return nil }
            return .object([("index", .integer(Int64(t.index))), ("sourceTitleId", n(listed.sourceTitleId)), ("sourceFile", .string(listed.sourceFile)),
                            ("duration", .string(listed.duration)), ("durationSeconds", .integer(Int64(listed.durationSeconds))),
                            ("chapters", .integer(Int64(listed.chapters))), ("sizeBytes", .integer(listed.sizeBytes)),
                            ("segmentMap", .string(listed.segmentMap)), ("readErrors", .integer(Int64(t.readErrors.count)))])
        }

        var attempt: [(key: String, value: JsonValue)] = [("job", .string(job.id.value))]
        if let driveId { attempt.append(("drive", .string(driveId))) }
        attempt.append(("startedAt", .string(started)))
        attempt.append(("titles", .object(acquired.titles.map { (String($0.index), .integer(Int64($0.readErrors.count))) })))
        attempt.append(("kept", .array(acquired.titles.map { .integer(Int64($0.index)) })))

        func problem(_ m: BroMessage, readError: Bool) -> JsonValue {
            var members: [(key: String, value: JsonValue)] = [("code", .string(MessageCode.wire(m.code))), ("params", .object(m.params)),
                                                              ("text", .string(messages.render(m)))]
            if readError { members.append(("readError", .bool(true))) }
            return .object(members)
        }
        var problems = (acquired.titles.flatMap(\.readErrors) + acquired.readErrors)
            .map { problem(BroMessage(.makemkvMessage, [("text", .string($0))], severity: .error), readError: true) }
        if readErrors > 0 {
            problems.append(problem(BroMessage(.ripReadErrors, [("count", .integer(Int64(readErrors)))], severity: .error), readError: false))
        }

        return .object([
            ("format", .string("bromelia-archive")), ("version", .integer(3)),
            ("unit", .object([("id", .string(unitId.value)), ("short", .string(String(unitId.value.prefix(8)))), ("library", .string(plan.libraryId))])),
            ("status", .string(readErrors > 0 ? "errors" : "success")),
            ("name", .string(identity.name)), ("kind", .string(identity.kind.rawValue)),
            ("disc", disc), ("acquisition", .object(acquisition)),
            ("job", .object([("id", .string(job.id.value)), ("startedAt", .string(started)), ("finishedAt", .string(Instant.format(finishedAt)))])),
            ("titles", .array(titles)), ("episodes", .array([])),
            ("files", .array(files.map(SealOutput.file))), ("attempts", .array([.object(attempt)])),
            ("counts", .object([("warnings", .integer(Int64(log.filter { $0.severity == .warning }.count))),
                                ("errors", .integer(Int64(log.filter { $0.severity == .error }.count)))])),
            ("problems", .array(problems)), ("logs", .array(logs.map { .string($0) })),
        ])
    }

    /// The job's source as the record names it: drive:<id> (MakeMKV's numbering of a drive doesn't last), or iso:, folder:,
    /// file: and the path.
    private static func sourceText(_ source: DiscSource?) -> String {
        switch source {
        case .drive(let id)?: return "drive:" + id
        case .iso(let path)?: return "iso:" + path
        case .folder(let path)?: return "folder:" + path
        case .file(let path)?: return "file:" + path
        case nil: return ""
        }
    }
}
