namespace Bromelia.Domain;

/// <summary>A track's kind (MakeMKV attribute 1).</summary>
public enum TrackKind
{
    Video,
    Audio,
    Subtitle,
    Attachment,
    Unknown,
}
