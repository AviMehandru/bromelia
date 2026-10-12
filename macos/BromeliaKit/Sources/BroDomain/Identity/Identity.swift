import BroFoundation

/// What a disc is: the movie or show on it, movie or TV, its format and format code, whether the backup stays
/// encrypted, its label and why it's taken for a movie or a show.
public struct Identity: Sendable, Equatable {
    public var name: String
    public var kind: MediaKind
    public var format: DiscFormat
    public var formatCode: FormatCode
    public var encrypted: Bool
    public var label: Label
    public var reason: BroMessage

    public init(name: String, kind: MediaKind, format: DiscFormat, formatCode: FormatCode, encrypted: Bool, label: Label, reason: BroMessage) {
        self.name = name
        self.kind = kind
        self.format = format
        self.formatCode = formatCode
        self.encrypted = encrypted
        self.label = label
        self.reason = reason
    }

    /// The name: typed by the user, else the disc's name when it looks written by a person ("The Dark Knight", not
    /// DARK_KNIGHT_D1), else the label's title, else the label, else "Disc". The label merges the volume name's set
    /// information with the disc name's.
    public static func resolve(_ inputs: IdentityInputs) -> Identity {
        let listing = inputs.listing
        let volume = (listing?.volumeName.isEmpty == false) ? listing!.volumeName : inputs.discLabel
        let discName = listing?.name ?? ""
        let fromVolume = LabelParser.parse(volume)
        let nameLooksHuman = !discName.isEmpty && !discName.contains("_") && discName != volume
            && (discName.contains(" ") || discName != discName.uppercased())
        let fromName = LabelParser.parse(discName)
        var label = fromVolume
        if label.title.isEmpty { label.title = fromName.title }
        label.season = fromVolume.season ?? fromName.season
        label.disc = fromVolume.disc ?? fromName.disc
        label.part = fromVolume.part ?? fromName.part
        label.volume = fromVolume.volume ?? fromName.volume
        label.looksLikeSeries = fromVolume.looksLikeSeries || fromName.looksLikeSeries

        var name = DriveJoin.trimSpaces(inputs.nameOverride)
        if name.isEmpty { name = nameLooksHuman && !fromName.title.isEmpty ? fromName.title : label.title }
        if name.isEmpty { name = inputs.discLabel.isEmpty ? "Disc" : inputs.discLabel }

        let format = inputs.format ?? FormatDetector.detect(listing, flags: inputs.flags, indexBdmv: nil)
        let decision = inputs.kindOverride.map { Decision(value: $0, reason: BroMessage(.identityReasonChosen)) }
            ?? KindHeuristics.decide(label, listing: listing, playAllEpisodes: inputs.playAllEpisodes)
        return Identity(name: name, kind: decision.value, format: format, formatCode: FormatDetector.code(format, encrypted: inputs.encrypted),
                        encrypted: inputs.encrypted, label: label, reason: decision.reason)
    }
}

extension Identity {
    /// The identity as JSON (dtos.json's Identity); the label's set is `LabelParser.setDescription`.
    public static func encode(_ identity: Identity) -> JsonValue {
        let l = identity.label
        func n(_ v: Int?) -> JsonValue { v.map { .integer(Int64($0)) } ?? .null }
        return .object([
            ("name", .string(identity.name)), ("kind", .string(identity.kind.rawValue)), ("format", .string(identity.format.rawValue)),
            ("formatCode", .string(identity.formatCode.text)), ("encrypted", .bool(identity.encrypted)),
            ("label", .object([("title", .string(l.title)), ("season", n(l.season)), ("part", n(l.part)), ("volume", n(l.volume)),
                               ("disc", n(l.disc)), ("set", .string(LabelParser.setDescription(l))), ("looksLikeSeries", .bool(l.looksLikeSeries))])),
            ("reason", identity.reason.toJson()),
        ])
    }

    /// An identity from its JSON (`encode`); what the JSON leaves out takes its empty value.
    public static func decode(_ json: JsonValue) -> Identity {
        let l = json["label"]
        func n(_ key: String) -> Int? { l?[key]?.int.map { Int($0) } }
        return Identity(
            name: json["name"]?.string ?? "", kind: json["kind"]?.string.flatMap(MediaKind.init(rawValue:)) ?? .movie,
            format: json["format"]?.string.flatMap(DiscFormat.init(rawValue:)) ?? .unknown, formatCode: FormatCode(json["formatCode"]?.string ?? ""),
            encrypted: json["encrypted"]?.bool ?? false,
            label: Label(title: l?["title"]?.string ?? "", season: n("season"), part: n("part"), volume: n("volume"), disc: n("disc"),
                         looksLikeSeries: l?["looksLikeSeries"]?.bool ?? false),
            reason: BroMessage.decode(json["reason"] ?? .null))
    }
}
