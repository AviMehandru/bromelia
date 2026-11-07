using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>The details of a video-disc job's request (<see cref="JobRequest.Details"/>): its source and, when the
/// titles were chosen on an opened disc, that session's snapshot.</summary>
public sealed record DiscRequest(DiscSource Source, SessionSnapshot? Session = null)
{
    /// <summary><c>{"source": …, "session": {"listing": …, "choices": …}}</c>; session left out when there is
    /// none.</summary>
    public static JsonValue Encode(DiscRequest request) => request.Session is { } s
        ? JsonValue.Of(("source", DiscSource.Encode(request.Source)),
            ("session", JsonValue.Of(("listing", Listing.Encode(s.Listing)), ("choices", SessionChoices.Encode(s.Choices)))))
        : JsonValue.Of(("source", DiscSource.Encode(request.Source)));

    /// <summary>The request's details: the source they name, else the request's drive; null when there is neither.</summary>
    public static DiscRequest? Decode(JobRequest request)
    {
        var details = request.Details;
        var source = (details?["source"] is { } s ? DiscSource.Decode(s) : null)
            ?? (request.DriveId is { } d ? new DiscSource.Drive(d) : null);
        if (source == null) return null;
        var session = details?["session"] is JsonValue.Object o
            ? new SessionSnapshot(Listing.Decode(o["listing"] ?? JsonValue.Null.Instance), SessionChoices.Decode(o["choices"] ?? JsonValue.Null.Instance))
            : null;
        return new DiscRequest(source, session);
    }
}
