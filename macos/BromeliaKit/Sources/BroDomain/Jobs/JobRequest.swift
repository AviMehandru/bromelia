import BroFoundation

/// What a job was asked to do (stored as `jobs.request`). The kind-specific part (the session snapshot of a
/// hand-picked rip, a verify target, a command step) is in `details`; the engine's phase gives it types.
public struct JobRequest: Sendable, Equatable {
    public var kind: JobKind
    public var title: String
    public var automatic: Bool
    public var driveId: String?
    public var mediaGeneration: Int64?
    public var sessionId: Id?
    public var parentId: Id?
    public var unitId: Id?
    public var options: JsonValue?
    public var details: JsonValue?

    public init(kind: JobKind, title: String, automatic: Bool = false, driveId: String? = nil, mediaGeneration: Int64? = nil,
                sessionId: Id? = nil, parentId: Id? = nil, unitId: Id? = nil, options: JsonValue? = nil, details: JsonValue? = nil) {
        self.kind = kind
        self.title = title
        self.automatic = automatic
        self.driveId = driveId
        self.mediaGeneration = mediaGeneration
        self.sessionId = sessionId
        self.parentId = parentId
        self.unitId = unitId
        self.options = options
        self.details = details
    }
}
