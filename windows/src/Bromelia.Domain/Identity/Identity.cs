using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>What a disc is: the movie or show on it, movie or TV, its format and format code, whether the backup
/// stays encrypted, its label and why it's taken for a movie or a show.</summary>
public sealed record Identity(string Name, MediaKind Kind, DiscFormat Format, FormatCode FormatCode, bool Encrypted, Label Label, BroMessage Reason)
{
    /// <summary>The name: typed by the user, else the disc's name when it looks written by a person ("The Dark
    /// Knight", not DARK_KNIGHT_D1), else the label's title, else the label, else "Disc". The label merges the
    /// volume name's set information with the disc name's.</summary>
    public static Identity Resolve(IdentityInputs inputs)
    {
        var listing = inputs.Listing;
        var volume = listing is { VolumeName.Length: > 0 } ? listing.VolumeName : inputs.DiscLabel;
        var discName = listing?.Name ?? "";
        var fromVolume = LabelParser.Parse(volume);
        bool nameLooksHuman = discName.Length > 0 && !discName.Contains('_') && discName != volume
                              && (discName.Contains(' ') || discName != discName.ToUpperInvariant());
        var fromName = LabelParser.Parse(discName);
        var label = fromVolume with
        {
            Title = fromVolume.Title.Length > 0 ? fromVolume.Title : fromName.Title,
            Season = fromVolume.Season ?? fromName.Season,
            Disc = fromVolume.Disc ?? fromName.Disc,
            Part = fromVolume.Part ?? fromName.Part,
            Volume = fromVolume.Volume ?? fromName.Volume,
            LooksLikeSeries = fromVolume.LooksLikeSeries || fromName.LooksLikeSeries,
        };

        var name = inputs.NameOverride.Trim(' ', '	');
        if (name.Length == 0) name = nameLooksHuman && fromName.Title.Length > 0 ? fromName.Title : label.Title;
        if (name.Length == 0) name = inputs.DiscLabel.Length == 0 ? "Disc" : inputs.DiscLabel;

        var format = inputs.Format ?? FormatDetector.Detect(listing, inputs.Flags, null);
        var decision = inputs.KindOverride is { } k
            ? new Decision(k, new BroMessage(MessageCode.IdentityReasonChosen))
            : KindHeuristics.Decide(label, listing, inputs.PlayAllEpisodes);
        return new Identity(name, decision.Value, format, FormatDetector.Code(format, inputs.Encrypted), inputs.Encrypted, label, decision.Reason);
    }

    /// <summary>The identity as JSON (dtos.json's Identity); the label's set is <see cref="LabelParser.SetDescription"/>.</summary>
    public static JsonValue Encode(Identity identity)
    {
        var l = identity.Label;
        JsonValue N(int? v) => v is { } x ? JsonValue.Of(x) : JsonValue.Null.Instance;
        return JsonValue.Of(("name", JsonValue.Of(identity.Name)), ("kind", JsonValue.Of(EnumWire.Name(identity.Kind))),
            ("format", JsonValue.Of(EnumWire.Name(identity.Format))), ("formatCode", JsonValue.Of(identity.FormatCode.Text)),
            ("encrypted", JsonValue.Of(identity.Encrypted)),
            ("label", JsonValue.Of(("title", JsonValue.Of(l.Title)), ("season", N(l.Season)), ("part", N(l.Part)), ("volume", N(l.Volume)),
                ("disc", N(l.Disc)), ("set", JsonValue.Of(LabelParser.SetDescription(l))), ("looksLikeSeries", JsonValue.Of(l.LooksLikeSeries)))),
            ("reason", identity.Reason.ToJson()));
    }

    /// <summary>An identity from its JSON (<see cref="Encode"/>); what the JSON leaves out takes its empty value.</summary>
    public static Identity Decode(JsonValue json)
    {
        var l = json["label"];
        int? N(string key) => (int?)l?[key]?.AsInteger;
        return new Identity(json["name"]?.AsString ?? "", EnumWire.Parse<MediaKind>(json["kind"]?.AsString) ?? MediaKind.Movie,
            EnumWire.Parse<DiscFormat>(json["format"]?.AsString) ?? DiscFormat.Unknown, new FormatCode(json["formatCode"]?.AsString ?? ""),
            json["encrypted"]?.AsBool ?? false,
            new Label(l?["title"]?.AsString ?? "", N("season"), N("part"), N("volume"), N("disc"), l?["looksLikeSeries"]?.AsBool ?? false),
            BroMessage.Decode(json["reason"] ?? JsonValue.Null.Instance));
    }
}
