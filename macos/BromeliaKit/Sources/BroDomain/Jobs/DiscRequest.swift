import BroFoundation

/// The details of a video-disc job's request (`JobRequest.details`): its source and, when the titles were chosen on an
/// opened disc, that session's snapshot.
public struct DiscRequest: Sendable, Equatable {
    public var source: DiscSource
    public var session: SessionSnapshot?

    public init(source: DiscSource, session: SessionSnapshot? = nil) {
        self.source = source
        self.session = session
    }

    /// `{"source": …, "session": {"listing": …, "choices": …}}`; session left out when there is none.
    public static func encode(_ request: DiscRequest) -> JsonValue {
        var members: [(key: String, value: JsonValue)] = [("source", DiscSource.encode(request.source))]
        if let s = request.session {
            members.append(("session", .object([("listing", Listing.encode(s.listing)), ("choices", SessionChoices.encode(s.choices))])))
        }
        return .object(members)
    }

    /// The request's details: the source they name, else the request's drive; nil when there is neither.
    public static func decode(_ request: JobRequest) -> DiscRequest? {
        let details = request.details
        guard let source = details?["source"].flatMap(DiscSource.decode) ?? request.driveId.map({ .drive(driveId: $0) }) else { return nil }
        var session: SessionSnapshot?
        if let o = details?["session"], o.members != nil {
            session = SessionSnapshot(listing: Listing.decode(o["listing"] ?? .null), choices: SessionChoices.decode(o["choices"] ?? .null))
        }
        return DiscRequest(source: source, session: session)
    }
}
