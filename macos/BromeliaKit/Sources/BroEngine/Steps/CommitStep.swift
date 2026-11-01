import BroDomain
import BroFoundation
import BroPorts

/// Step 14 (plan §20.1, §15.4; §22.3 steps 4–6 through LibraryService): moves the unit Seal made in the job's staging
/// folder into the library. Nothing is hashed again: the files are Seal's (unit_files rows), the record Seal's data. The
/// intent's items are Name's, by the names they have in staging (each in place: staging is laid out as the unit's
/// folder), a folder told from Seal's files, then Bromelia's own files Seal wrote (the record, the logs, the note); its
/// destination is <library>/<Name's folder>, merged item by item when Name says the folder is shared. A unit Seal tagged
/// (READ ERRORS) goes to _Quarantine/<Name's name> [<tag>] instead (commit.quarantined), never merged, and the step
/// reports it quarantined; otherwise archive.moved. A folder of its own that another job committed after Name ran takes the
/// next free name (archive.outputFolder, a warning), unless naming.conflictPolicy is skip (owner H3b-2), and never a name the library has called taken. LibraryService waits while a roll forward of the unit runs (the job's
/// cancel ends that wait), finishes an open intent of the unit and gives a unit committed already its result again, so a
/// rerun rolls forward. Holds the library write. Data: CommitOutput. Skipped when Plan or Seal hasn't run. Paths compare
/// by their bytes.
public struct CommitStep: Step {
    private static let quarantineFolder = "_Quarantine"
    private static let slash = UInt8(ascii: "/")

    public init() {}

    public var kind: StepKind { .commit }

    /// Plan §20.1's "rolls forward": a rerun finds the unit's open intent and finishes it.
    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet {
        guard let plan = PlanOutput.of(job), !plan.libraryId.isEmpty else { return .none }
        return ResourceSet(libraryWrites: [plan.libraryId])
    }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        let job = context.job
        guard let plan = PlanOutput.of(job), let seal = SealOutput.of(job) else { return StepReport(skipped: true) }
        guard let named = NameOutput.of(job) else { throw BroMessage(.nameNoFolder, severity: .error).toError() }
        let root = try context.library.require(plan.libraryId).path
        if context.cancel.isCancelled { throw BroError(MessageCode.wire(.jobCancelled)) }
        let staging = try context.library.stagingDirectory(plan.libraryId, jobId: job.id)

        let tag = seal.tag
        var path = tag != nil ? Self.quarantineFolder + "/" + named.name : named.folder
        let now = context.clock.now()
        var intent = CommitIntent(unitId: seal.unitId, jobId: job.id, staging: staging, destination: root + "/" + path, merge: named.merge && tag == nil,
                                  quarantine: false, createdAt: now, items: Self.items(named, seal))
        var unit = Self.unit(job, plan, named, seal, path, now)
        context.progress.report(0, operation: BroMessage(.archiveMoving), current: nil)
        var committedResult: CommitResult?
        if let tag {
            committedResult = try await context.library.quarantine(unit, intent: intent, files: seal.files, tag: tag, cancel: context.cancel)
        } else {
            // A folder of its own that another job committed after Name ran (two discs of the same name at once): the next
            // free name, as Name would have given it, unless the profile says skip (owner H3b-2). The library refuses a
            // taken folder before it writes anything, and an open intent or a unit committed already never gets that far.
            let free = !named.merge && NamingSettings.decode(plan.profile["naming"] ?? .object([])).conflictPolicy != .skip
            // Each name the library called taken, the first being Name's: never asked for again, even when the listing
            // shows it under other bytes (a file system that ignores case beyond ASCII, or normalization).
            var refusedNames: [String] = []
            while committedResult == nil {
                do throws(BroError) {
                    committedResult = try await context.library.commit(unit, intent: intent, files: seal.files, cancel: context.cancel)
                } catch {
                    let refused = error.params.first { $0.key == "path" }?.value.string
                    guard free, error.code == MessageCode.wire(.libraryFolderExists), let refused,
                          refused.utf8.elementsEqual(intent.destination.utf8) else { throw error }
                    let cut = path.utf8.lastIndex(of: Self.slash)
                    let parent = cut.map { String(decoding: path.utf8[..<$0], as: UTF8.self) } ?? ""
                    let last = cut.map { String(decoding: path.utf8[path.utf8.index(after: $0)...], as: UTF8.self) } ?? path
                    let directory = parent.isEmpty ? root : root + "/" + parent
                    refusedNames.append(last)
                    let name = ConflictNamer.next(refusedNames[0], existing: try context.fs.list(directory).map(\.name) + refusedNames, isFolder: true)
                    path = parent.isEmpty ? name : parent + "/" + name
                    intent.destination = root + "/" + path
                    unit.path = path
                    context.log.log(BroMessage(.archiveOutputFolder, [("path", .string(intent.destination))], severity: .warning))
                }
            }
        }
        let result = committedResult!

        context.log.log(tag != nil
            ? BroMessage(.commitQuarantined, [("path", .string(result.destination))], severity: .warning)
            : BroMessage(.archiveMoved, [("count", .integer(Int64(intent.items.count))), ("path", .string(result.destination))]))
        let prefix = Array((root + "/").utf8)
        let committed = result.destination.utf8.starts(with: prefix)
            ? String(decoding: result.destination.utf8.dropFirst(prefix.count), as: UTF8.self) : result.destination
        return StepReport(data: CommitOutput.encode(CommitOutput(unitId: seal.unitId, library: plan.libraryId, path: committed, merged: result.merged,
                                                                 quarantined: tag != nil)),
                          quarantined: tag != nil)
    }

    /// Name's items in order, each in place (a folder when Seal hashed no file of that name), then the record, the logs
    /// and the note Seal wrote. Only files carry a SHA-256 (Seal's): Bromelia's own files aren't in SHA256SUMS, and their
    /// names hold the unit's id.
    private static func items(_ named: NameOutput, _ seal: SealOutput) -> [CommitItem] {
        var items: [CommitItem] = []
        for item in named.files {
            var bytes = Array(item.path.utf8)
            while bytes.last == slash { bytes.removeLast() }
            let path = String(decoding: bytes, as: UTF8.self)
            let file = seal.files.first { $0.path.utf8.elementsEqual(bytes) }
            items.append(CommitItem(seq: items.count, fromPath: path, toPath: path, sha256: file?.sha256, isDir: file == nil, moved: false))
        }
        for own in [seal.record] + seal.logs + (seal.note.map { [$0] } ?? []) {
            items.append(CommitItem(seq: items.count, fromPath: own, toPath: own, sha256: nil, isDir: false, moved: false))
        }
        return items
    }

    /// The archive_units row, from what Seal's record holds: its identity (Name's when it corrected it, else Plan's), the
    /// disc's label (the listing's name, else the source's), Probe's fingerprint and MakeMKV version, Seal's status,
    /// record and files.
    private static func unit(_ job: JobSnapshot, _ plan: PlanOutput, _ named: NameOutput, _ seal: SealOutput, _ path: String, _ now: Instant) -> UnitRecord {
        let probe = JobSnapshot.output(job, .probe).flatMap { $0.members != nil ? ProbeOutput.decode($0) : nil }
        let identity = named.identity ?? plan.identity
        let source = DiscRequest.decode(job.request)?.source
        let label = if let listing = probe?.listing, !listing.name.isEmpty { listing.name } else if let source { StepSources.label(source, nil) } else { "" }
        return UnitRecord(id: seal.unitId, libraryId: plan.libraryId, physicalDiscId: nil, jobId: job.id, path: path, recordFile: seal.record, recordVersion: 3,
                          state: .committing, status: UnitStatus(rawValue: seal.status) ?? .success, name: identity.name, kind: identity.kind,
                          format: identity.format.rawValue, formatCode: identity.formatCode.text, encrypted: identity.encrypted,
                          fingerprint: probe?.fingerprint, label: label, season: identity.label.season, part: identity.label.part,
                          volume: identity.label.volume, disc: identity.label.disc, makemkvVersion: probe?.makemkvVersion ?? "",
                          bytes: seal.files.reduce(0) { $0 + $1.size }, fileCount: seal.files.count, attempts: 1, createdAt: now)
    }
}
