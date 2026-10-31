import BroDomain
import BroFoundation

/// What the video-disc steps share about a job's source: its request details, where makemkvcon opens it, the mode it
/// would get before Plan, and the disc's label before there is a listing.
public enum StepSources {
    /// The job's DiscRequest; internal.unexpected when it names no source.
    public static func request(_ job: JobSnapshot) throws(BroError) -> DiscRequest {
        guard let r = DiscRequest.decode(job.request) else {
            throw BroError(MessageCode.wire(.internalUnexpected), [("detail", .string("the job has no source"))])
        }
        return r
    }

    /// A drive's location now; drive.notFound when it isn't connected.
    public static func drive(_ lookup: any DriveLookup, _ driveId: String) throws(BroError) -> DriveLocation {
        guard let at = lookup.find(driveId) else { throw notFound(driveId) }
        return at
    }

    /// Where makemkvcon opens the source: disc:N for a drive (drive.notFound when it isn't connected or MakeMKV hasn't
    /// numbered it), the image, or the disc a folder or file belongs to (SourceResolver).
    public static func makemkv(_ source: DiscSource, _ lookup: any DriveLookup) throws(BroError) -> MakemkvSource {
        switch source {
        case .drive(let id):
            let at = try drive(lookup, id)
            guard let index = at.makemkvIndex else { throw notFound(id) }
            return .drive(index: index, device: at.device)
        case .iso(let path): return .iso(path: path)
        case .folder(let path): return SourceResolver.source(path, isDirectory: true)
        case .file(let path): return SourceResolver.source(path, isDirectory: false)
        }
    }

    /// The profile before Plan: the drive entry's, with no facts yet but whether the rip is automatic, and the request's
    /// options.profile above it (as Plan has it).
    public static func profile(_ context: StepContext, _ configId: String?) -> EffectiveProfile {
        ProfileResolver.resolve(context.config, driveId: configId, facts: RuleFacts(name: "", label: "", formatCode: "", automatic: context.job.request.automatic),
                                sessionChoices: layer(context.config, context.job.request.options?["profile"]?.string, nil))
    }

    /// A chosen profile and library as the resolver's session layer: the profile's fields (an id that isn't configured
    /// adds none), then the library. Nil with nothing chosen.
    static func layer(_ config: Config, _ profileId: String?, _ libraryId: String?) -> JsonValue? {
        var members: [(key: String, value: JsonValue)] = []
        if let profileId, let p = Config.profiles(config).first(where: { $0.id == profileId }), let m = p.json.members {
            members += m.filter { $0.key != "id" && $0.key != "name" }
        }
        if let libraryId {
            members.removeAll { $0.key == "library" }
            members.append(("library", .string(libraryId)))
        }
        return members.isEmpty ? nil : .object(members)
    }

    /// The mode before Plan: the request's (options.mode), else the profile's mode.default.
    public static func baseMode(_ job: JobSnapshot, _ profile: EffectiveProfile) -> RipMode {
        job.request.options?["mode"]?.string.flatMap(RipMode.init(rawValue:))
            ?? profile.profile["mode"]?["default"]?.string.flatMap(RipMode.init(rawValue:)) ?? .mkv
    }

    /// The disc's label before a listing names it: the drive's disc label, an image's name without its extension, or
    /// the folder's (or file's folder's) name.
    public static func label(_ source: DiscSource, _ drive: DriveLocation?) -> String {
        func last(_ p: String) -> String {
            var p = Substring(p)
            while let c = p.last, c == "/" || c == "\\" { p = p.dropLast() }
            if let i = p.lastIndex(where: { $0 == "/" || $0 == "\\" }) { return String(p[p.index(after: i)...]) }
            return String(p)
        }
        switch source {
        case .drive: return drive?.label ?? ""
        case .iso(let path):
            let name = last(path)
            if let dot = name.lastIndex(of: "."), dot > name.startIndex { return String(name[..<dot]) }
            return name
        case .folder(let path): return last(path)
        case .file(let path):
            if case .file(let root) = SourceResolver.source(path, isDirectory: false) { return last(root) }
            return last(path)
        }
    }

    private static func notFound(_ driveId: String) -> BroError { BroError(MessageCode.wire(.driveNotFound), [("driveId", .string(driveId))]) }
}
