using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Where a video-disc job reads (dtos.json's SessionSource): a drive, a disc image, a folder holding a disc
/// structure, or a file inside one (SourceResolver opens the disc it belongs to).</summary>
public abstract record DiscSource
{
    public sealed record Drive(string DriveId) : DiscSource;
    public sealed record Iso(string Path) : DiscSource;
    public sealed record Folder(string Path) : DiscSource;
    public sealed record File(string Path) : DiscSource;

    /// <summary><c>{"drive": id}</c>, <c>{"iso": path}</c>, <c>{"folder": path}</c> or <c>{"file": path}</c>.</summary>
    public static JsonValue Encode(DiscSource source) => source switch
    {
        Drive d => JsonValue.Of(("drive", JsonValue.Of(d.DriveId))),
        Iso i => JsonValue.Of(("iso", JsonValue.Of(i.Path))),
        Folder f => JsonValue.Of(("folder", JsonValue.Of(f.Path))),
        File f => JsonValue.Of(("file", JsonValue.Of(f.Path))),
        _ => JsonValue.Null.Instance,
    };

    /// <summary>The source <see cref="Encode"/> wrote; null for anything else.</summary>
    public static DiscSource? Decode(JsonValue json) =>
        json["drive"]?.AsString is { } d ? new Drive(d)
        : json["iso"]?.AsString is { } i ? new Iso(i)
        : json["folder"]?.AsString is { } f ? new Folder(f)
        : json["file"]?.AsString is { } file ? new File(file)
        : null;
}
