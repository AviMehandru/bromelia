import BroDomain
import BroFoundation

/// Step 6 (plan §20.1): decides what the job makes and appends the rest of its steps.
/// - The profile: ProfileResolver.resolve with the identity's facts (name, label, format code, kind), the drive entry and
///   whether the rip is automatic; the chosen profile and library are its session layer (the chosen profile's fields,
///   then the chosen library): a session's choices, else the request's options.profile and options.library.
/// - The mode: the request's (options.mode); MKV when titles were chosen by hand; else ModeChooser by the disc's format.
/// - A mode that makes MKV files needs the listing (disc.noInformation) and titles: the session's ticked titles
///   (numbered as Reconcile found them; titles.noneChosenExist when none is on the disc), else TitleRules.select (each
///   title logged; its error, titles.manualRequired, titles.noneMatched), then OnePass.plan and the space they need
///   (SpaceEstimate.forPlan).
/// - The library: the profile's, else defaultLibrary, else the first (library.none; library.notFound). An offline one
///   doesn't fail the job: Decide waits for it, blocked, then checks the archive and the space.
/// - Archived before (not for infoOnly): ArchiveCheck. skip ends the job (disc.alreadyArchived); ask appends Decide.
/// - The space is reserved here when no Decide follows (SpaceCheck).
/// - Appends [Decide?], Acquire, Release, VerifyRips (archive.verifyRips, not for infoOnly), Name, Seal, Commit, Notify.
///   Its report's plan has the mode, profile and library.
/// Data: PlanOutput.
public struct PlanStep: Step {
    public init() {}

    public var kind: StepKind { .plan }

    public var resumability: Resumability { .idempotent }

    public func resources(_ job: JobSnapshot) -> ResourceSet { .none }

    public func run(_ context: StepContext) async throws(BroError) -> StepReport {
        let job = context.job
        let request = try StepSources.request(job)
        let probe = ProbeOutput.decode(JobSnapshot.output(job, .probe) ?? .null)
        var identity = Identity.decode(JobSnapshot.output(job, .identify) ?? .object([]))
        var drive: DriveLocation?
        if case .drive(let id) = request.source { drive = context.driveLookup.find(id) }
        let configId = drive?.configId
        let choices = request.session?.choices
        let listing = probe.listing

        let label = listing.map(\.name).flatMap { $0.isEmpty ? nil : $0 } ?? StepSources.label(request.source, drive)
        let facts = RuleFacts(name: identity.name, label: label, formatCode: identity.formatCode.text, kind: identity.kind.rawValue,
                              automatic: job.request.automatic)
        let chosenProfile = choices?.profile ?? job.request.options?["profile"]?.string
        let chosenLibrary = choices?.library ?? job.request.options?["library"]?.string
        let profile = ProfileResolver.resolve(context.config, driveId: configId, facts: facts,
                                              sessionChoices: StepSources.layer(context.config, chosenProfile, chosenLibrary))
        let profileId = chosenProfile ?? profile.trace.last(where: { $0.layer == .driveProfile })?.id
            ?? profile.trace.first(where: { $0.layer == .defaultProfile })?.id ?? "default"
        let byHand = choices?.titles.contains(where: \.selected) ?? false
        let mode = job.request.options?["mode"]?.string.flatMap(RipMode.init(rawValue:))
            ?? (byHand ? .mkv : ModeChooser.mode(identity.format, flags: drive?.flags ?? DiscFlags(raw: 0), content: .video,
                                                 profile: Profile(profile.profile), chosenByHand: false))
            ?? .mkv
        if (mode == .backup) != identity.encrypted { identity = try IdentifyStep.resolve(context, encrypted: mode == .backup) }

        var titles: [TitleChoice] = []
        var onePass: Int?
        var need: Int64?
        if mode == .mkv || mode == .backupThenMkv {
            guard let listing else { return PlanStep.failed(BroMessage(.discNoInformation, severity: .error)) }
            if let choices, byHand {
                let map = JobSnapshot.output(job, .reconcile).flatMap { $0.members != nil ? ReconcileOutput.decode($0).titleMap : nil }
                for var t in choices.titles where t.selected {
                    t.index = map?[t.index] ?? t.index
                    if listing.titles.contains(where: { $0.index == t.index }) { titles.append(t) }
                }
                titles.sort { $0.index < $1.index }
                if titles.isEmpty { return PlanStep.failed(BroMessage(.titlesNoneChosenExist, severity: .error)) }
            } else {
                let selection = TitleRules.select(listing, rules: TitleSettings.decode(profile.profile["titles"] ?? .object([])))
                if let error = selection.error { return PlanStep.failed(error) }
                if selection.requiresManualChoice {
                    return PlanStep.failed(BroMessage(.titlesManualRequired, [("profile", .string(PlanStep.profileName(context.config, profileId)))],
                                                      severity: .error))
                }
                for trace in selection.trace {
                    guard let t = listing.titles.first(where: { $0.index == trace.index }) else { continue }
                    var p: [(key: String, value: JsonValue)] = [("title", .integer(Int64(t.index))), ("duration", .string(t.duration)),
                                                                ("chapters", .integer(Int64(t.chapters)))]
                    if !trace.selected { p.append(("reason", trace.reason.toJson())) }
                    context.log.log(BroMessage(trace.selected ? .titlesSelected : .titlesSkipped, p))
                }
                if selection.indices.isEmpty { return PlanStep.failed(BroMessage(.titlesNoneMatched, severity: .error)) }
                titles = selection.indices.map { TitleChoice(index: $0, selected: true) }
            }
            let picked = Set(titles.filter { $0.tracks != nil }.map(\.index))
            if picked.isEmpty {
                onePass = OnePass.plan(titles.map(\.index), listing: listing,
                                       currentMinLength: profile.profile["makemkv"]?["minLengthSeconds"]?.int.map { Int($0) })?.minLength
            }
            let chosen = listing.titles.filter { t in titles.contains { $0.index == t.index } }
            let bytes = SpaceEstimate.forPlan(chosen, handPicked: picked, splitTitle: nil).count
            need = bytes > 0 ? bytes : nil
        }

        guard let libraryId = profile.profile["library"]?.string ?? context.config.document["defaultLibrary"]?.string
            ?? Config.libraries(context.config).first?.id else { return PlanStep.failed(BroMessage(.libraryNone, severity: .error)) }
        var library: LibraryHandle?
        do throws(BroError) {
            library = try context.library.require(libraryId)
        } catch {
            if error.code == MessageCode.wire(.libraryNotFound) { throw error }
            // Offline: Decide waits for it.
        }

        var ask: ArchivedMatch?
        var skip: BroMessage?
        if library != nil, mode != .infoOnly, let match = try await ArchiveCheck.find(context, probe.fingerprint) {
            skip = ArchiveCheck.apply(context, match, configId: configId)
            if ArchiveCheck.asks(context, configId: configId) { ask = match }
        }
        let decide = library == nil || ask != nil
        var reserved = false
        if skip == nil, !decide, let n = need, let library {
            try await SpaceCheck.reserve(context, library.path, n)
            reserved = true
        }

        let output = PlanOutput(mode: mode, profileId: profileId, profile: profile.profile, libraryId: libraryId, libraryOffline: library == nil,
                                titles: titles, onePassMinLength: onePass, spaceNeed: need, spaceReserved: reserved, askArchived: ask, identity: identity)
        let plan = JobPlan(steps: [], mode: mode.rawValue, profileId: profileId, libraryId: libraryId)
        if let skip { return StepReport(data: PlanOutput.encode(output), skipJob: skip, plan: plan) }
        var steps: [StepKind] = decide ? [.decide] : []
        steps += [.acquire, .release]
        if mode != .infoOnly, profile.profile["archive"]?["verifyRips"]?.bool != false { steps.append(.verifyRips) }
        steps += [.name, .seal, .commit, .notify]
        return StepReport(data: PlanOutput.encode(output), appendSteps: steps, plan: plan)
    }

    static func profileName(_ config: Config, _ profileId: String) -> String {
        let name = Config.profiles(config).first(where: { $0.id == profileId })?.name ?? ""
        return name.isEmpty ? profileId : name
    }

    static func failed(_ message: BroMessage) -> StepReport { StepReport(data: .object([]), error: message.toError()) }
}
